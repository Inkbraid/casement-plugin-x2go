#include "x2go/clipboard.h"

#include <zlib.h>

#include <algorithm>
#include <format>
#include <utility>

namespace x2go {

namespace {

// The reference client compresses text over 1 KB; the server inflates any
// chunk that says it is compressed.
constexpr std::size_t compress_above = 1024;
constexpr std::size_t max_selection = std::size_t{64} << 20;
constexpr std::size_t first_data = kdrive::event_size - 20;

void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    for (std::size_t i = 0; i < 4; ++i) v[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
}

bool valid_utf8(std::span<const std::uint8_t> s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned c = s[i];
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; ++k)
            if ((s[i + k] >> 6) != 2) return false;
        i += n;
    }
    return true;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> zlib_compress(std::span<const std::uint8_t> data) {
    if (data.size() > max_selection) return std::nullopt;
    uLongf size = compressBound(static_cast<uLong>(data.size()));
    std::vector<std::uint8_t> out(size);
    if (compress2(out.data(), &size, data.data(), static_cast<uLong>(data.size()), Z_DEFAULT_COMPRESSION) != Z_OK)
        return std::nullopt;
    out.resize(size);
    return out;
}

std::optional<std::vector<std::uint8_t>> zlib_inflate(std::span<const std::uint8_t> data, std::size_t size) {
    if (size > max_selection) return std::nullopt;
    std::vector<std::uint8_t> out(size);
    uLongf got = static_cast<uLongf>(size);
    // uncompress() wants room for one more byte to tell a complete stream
    // from a truncated one when the output is empty.
    if (size == 0) return out;
    if (uncompress(out.data(), &got, data.data(), static_cast<uLong>(data.size())) != Z_OK || got != size)
        return std::nullopt;
    return out;
}

std::vector<std::uint8_t> selection_events(std::span<const std::uint8_t> data, std::uint8_t selection,
                                           std::uint8_t format, std::size_t max_chunk) {
    std::vector<std::uint8_t> out;
    if (data.empty() || max_chunk == 0) return out;
    const std::size_t total = data.size();
    for (std::size_t at = 0; at < total; at += max_chunk) {
        auto part = data.subspan(at, std::min(max_chunk, total - at));
        std::span<const std::uint8_t> payload = part;
        std::optional<std::vector<std::uint8_t>> packed;
        std::uint32_t compressed = 0;
        if (format == kdrive::fmt_utf8 && part.size() > compress_above) {
            packed = zlib_compress(part);
            if (packed && packed->size() < part.size()) {
                payload = *packed;
                compressed = static_cast<std::uint32_t>(packed->size());
            }
        }
        std::vector<std::uint8_t> ev(kdrive::event_size, 0);
        put32(ev, 0, static_cast<std::uint32_t>(kdrive::Ev::selection));
        put32(ev, 4, static_cast<std::uint32_t>(part.size()));
        ev[8] = selection;
        ev[9] = format;
        ev[10] = static_cast<std::uint8_t>(at == 0);
        ev[11] = static_cast<std::uint8_t>(at + part.size() >= total);
        put32(ev, 12, compressed);
        put32(ev, 16, static_cast<std::uint32_t>(total));
        const std::size_t head = std::min(first_data, payload.size());
        std::copy_n(payload.begin(), head, ev.begin() + 20);
        out.insert(out.end(), ev.begin(), ev.end());
        for (std::size_t p = head; p < payload.size(); p += kdrive::event_size) {
            const std::size_t n = std::min(kdrive::event_size, payload.size() - p);
            const auto from = payload.begin() + static_cast<std::ptrdiff_t>(p);
            out.insert(out.end(), from, from + static_cast<std::ptrdiff_t>(n));
            out.insert(out.end(), kdrive::event_size - n, 0);
        }
    }
    return out;
}

std::optional<SelectionReceiver::Complete> SelectionReceiver::add(const kdrive::SelectionChunk& c,
                                                                   std::string& error) {
    if (c.first) {
        active_ = true;
        selection_ = c.selection;
        format_ = c.format;
        data_.clear();
        if (c.total <= max_selection) data_.reserve(c.total);
    } else if (!active_ || c.selection != selection_) {
        error = "a selection chunk without its first chunk";
        return std::nullopt;
    }
    if (data_.size() + c.chunk_size > max_selection) {
        active_ = false;
        error = std::format("a selection of more than {} bytes", max_selection);
        return std::nullopt;
    }
    if (c.compressed_size) {
        auto raw = zlib_inflate(c.data, c.chunk_size);
        if (!raw) {
            active_ = false;
            error = std::format("a compressed selection chunk ({} to {} bytes) didn't inflate", c.compressed_size,
                                c.chunk_size);
            return std::nullopt;
        }
        data_.insert(data_.end(), raw->begin(), raw->end());
    } else {
        data_.insert(data_.end(), c.data.begin(), c.data.end());
    }
    if (!c.last) return std::nullopt;
    active_ = false;
    Complete done{selection_, format_, std::move(data_)};
    data_.clear();
    return done;
}

std::optional<std::string> selection_text(std::uint32_t format, std::span<const std::uint8_t> data) {
    if (format != kdrive::fmt_utf8 && format != kdrive::fmt_string) return std::nullopt;
    while (!data.empty() && data.back() == 0) data = data.first(data.size() - 1);
    if (valid_utf8(data)) return std::string(data.begin(), data.end());
    // Latin-1, as X's STRING target is defined.
    std::string out;
    out.reserve(data.size() + data.size() / 4);
    for (std::uint8_t b : data) {
        if (b < 0x80) {
            out += static_cast<char>(b);
        } else {
            out += static_cast<char>(0xC0 | (b >> 6));
            out += static_cast<char>(0x80 | (b & 0x3F));
        }
    }
    return out;
}

}  // namespace x2go
