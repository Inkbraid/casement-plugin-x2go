// Clipboard chunks in both directions.

#include "check.h"

#include <algorithm>
#include <string>

#include "fixtures.h"
#include "util.h"
#include "x2go/clipboard.h"
#include "x2go/kdrive.h"

using namespace x2go;
using testing::bytes;
using testing::u32;

namespace {

std::string repeat(std::string_view s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

/// Reads SELECTIONEVENT chunks back the way the server does.
std::vector<std::uint8_t> server_reads(const std::vector<std::uint8_t>& events, int& chunks) {
    std::vector<std::uint8_t> data;
    std::size_t at = 0;
    chunks = 0;
    while (at < events.size()) {
        const std::uint8_t* e = events.data() + at;
        if (u32(events, at) != 9) throw std::runtime_error("not a SELECTIONEVENT");
        const std::uint32_t size = u32(events, at + 4), compressed = u32(events, at + 12);
        const std::size_t n = compressed ? compressed : size;
        std::vector<std::uint8_t> chunk(e + 20, e + 20 + std::min<std::size_t>(n, 21));
        at += kdrive::event_size;
        while (chunk.size() < n) {
            const std::size_t take = std::min<std::size_t>(n - chunk.size(), kdrive::event_size);
            chunk.insert(chunk.end(), events.begin() + static_cast<std::ptrdiff_t>(at),
                         events.begin() + static_cast<std::ptrdiff_t>(at + take));
            at += kdrive::event_size;
        }
        if (compressed) chunk = *zlib_inflate(chunk, size);
        data.insert(data.end(), chunk.begin(), chunk.end());
        ++chunks;
    }
    return data;
}

}  // namespace

TEST(sends_short_text_in_one_event) {
    auto ev = selection_events(bytes(std::string_view("hello")), kdrive::sel_clipboard, kdrive::fmt_utf8);
    REQUIRE(ev.size() == kdrive::event_size);
    CHECK_EQ(u32(ev, 0), 9u);
    CHECK_EQ(u32(ev, 4), 5u);
    CHECK_EQ(ev[8], 1);
    CHECK_EQ(ev[9], 1);
    CHECK_EQ(ev[10], 1);
    CHECK_EQ(ev[11], 1);
    CHECK_EQ(u32(ev, 12), 0u);
    CHECK_EQ(u32(ev, 16), 5u);
    CHECK(std::string(ev.begin() + 20, ev.begin() + 25) == "hello");
    CHECK(selection_events({}, kdrive::sel_clipboard, kdrive::fmt_utf8).empty());
}

TEST(sends_long_text_compressed_like_the_capture) {
    // The capture: 1440 bytes the server accepted (its clipboard then had
    // exactly this text), compressed with zlib's default level.
    const std::string text = repeat("fixture from the client ", 60);
    auto ev = selection_events(bytes(text), kdrive::sel_clipboard, kdrive::fmt_utf8);
    auto captured = bytes(fixtures::client_selection_compressed);
    CHECK_EQ(ev.size(), captured.size());
    CHECK(std::equal(ev.begin(), ev.begin() + 12, captured.begin()));  // type, sizes, flags
    CHECK_EQ(u32(ev, 16), u32(captured, 16));
    CHECK(ev == captured);
    int chunks = 0;
    auto back = server_reads(ev, chunks);
    CHECK(back == bytes(text));
    CHECK_EQ(chunks, 1);
}

TEST(splits_large_selections_into_chunks) {
    std::string text;
    for (int i = 0; i < 5000; ++i) text += std::to_string(i * 7919 % 10007) + ",";
    auto ev = selection_events(bytes(text), kdrive::sel_clipboard, kdrive::fmt_utf8, 4096);
    int chunks = 0;
    auto back = server_reads(ev, chunks);
    CHECK(back == bytes(text));
    CHECK_EQ(chunks, static_cast<int>((text.size() + 4095) / 4096));
    // First and last flags, and the total in every header.
    CHECK_EQ(ev[10], 1);
    CHECK_EQ(ev[11], 0);
    CHECK_EQ(u32(ev, 16), static_cast<std::uint32_t>(text.size()));
}

TEST(reassembles_selections_from_the_server) {
    SelectionReceiver r;
    std::string error;
    kdrive::Parser p;
    p.feed(bytes(fixtures::selection_plain));
    p.feed(bytes(fixtures::selection_compressed));
    auto plain = r.add(std::get<kdrive::SelectionChunk>(*p.next()), error);
    REQUIRE(plain);
    CHECK_EQ(plain->selection, 1u);
    CHECK(plain->data == bytes(std::string_view("fixture-clip")));
    auto big = r.add(std::get<kdrive::SelectionChunk>(*p.next()), error);
    REQUIRE(big);
    CHECK(big->data == bytes(repeat("0123456789abcdef", 200)));
    CHECK(error.empty());

    // Chunks in order, the last one completing it.
    kdrive::SelectionChunk a;
    a.selection = 1;
    a.format = 1;
    a.first = true;
    a.data = bytes(std::string_view("abc"));
    a.chunk_size = 3;
    a.total = 6;
    kdrive::SelectionChunk b = a;
    b.first = false;
    b.last = true;
    b.data = bytes(std::string_view("def"));
    CHECK(!r.add(a, error));
    auto both = r.add(b, error);
    REQUIRE(both);
    CHECK(both->data == bytes(std::string_view("abcdef")));
    // A continuation without its start.
    CHECK(!r.add(b, error));
    CHECK(!error.empty());
}

TEST(turns_selection_data_into_text) {
    auto utf8 = selection_text(kdrive::fmt_utf8, bytes(std::string_view("Grüße\0\0", 9)));
    REQUIRE(utf8);
    CHECK_EQ(*utf8, std::string("Grüße"));
    std::vector<std::uint8_t> latin1{'G', 0xFC, 'n', 't', 'e', 'r'};
    auto converted = selection_text(kdrive::fmt_string, latin1);
    REQUIRE(converted);
    CHECK_EQ(*converted, std::string("Günter"));
    CHECK(!selection_text(kdrive::fmt_pixmap, latin1));
}
