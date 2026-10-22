#include "x2go/kdrive.h"

#include <format>
#include <utility>

namespace x2go::kdrive {

namespace {

// Limits for sizes read from the stream, far above anything a server
// sends; they stop a corrupt stream from allocating gigabytes.
constexpr std::uint32_t max_regions = 64;
constexpr std::size_t max_payload = std::size_t{256} << 20;
constexpr std::uint32_t max_count = 1u << 20;

void put16(Event& e, std::size_t at, std::uint16_t v) {
    e[at] = static_cast<std::uint8_t>(v);
    e[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(Event& e, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) e[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

Event typed(Ev type) {
    Event e{};
    put32(e, 0, static_cast<std::uint32_t>(type));
    return e;
}

}  // namespace

Event key_event(bool press, std::uint32_t state, std::uint32_t keycode) {
    Event e = typed(press ? Ev::key_press : Ev::key_release);
    put32(e, 4, state);
    put32(e, 8, keycode);
    return e;
}

Event button_event(bool press, std::uint32_t state, std::uint32_t button) {
    Event e = typed(press ? Ev::mouse_press : Ev::mouse_release);
    put32(e, 4, state);
    put32(e, 8, button);
    return e;
}

Event motion_event(std::uint32_t x, std::uint32_t y) {
    Event e = typed(Ev::mouse_motion);
    put32(e, 4, x);
    put32(e, 8, y);
    return e;
}

Event geometry_event(std::uint16_t width, std::uint16_t height) {
    Event e = typed(Ev::geometry);
    put16(e, 4, width);
    put16(e, 6, height);
    // Offset 8: index of the primary screen. From offset 9, up to four
    // screen records (width, height, x, y); the server configures one RandR
    // output per record and reports the others as disconnected, so the one
    // screen is always described.
    e[8] = 0;
    put16(e, 9, width);
    put16(e, 11, height);
    return e;
}

Event update_event(std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t y) {
    Event e = typed(Ev::update);
    put32(e, 4, width);
    put32(e, 8, height);
    put32(e, 12, x);
    put32(e, 16, y);
    return e;
}

Event client_version_event(std::uint16_t version, std::uint16_t os) {
    Event e = typed(Ev::client_version);
    put16(e, 4, version);
    put16(e, 6, os);
    return e;
}

Event demand_selection_event(std::uint16_t selection) {
    Event e = typed(Ev::demand_selection);
    put16(e, 4, selection);
    return e;
}

Event plain_event(Ev type) { return typed(type); }

void Parser::feed(std::span<const std::uint8_t> data) {
    if (pos_ == buffer_.size()) {
        buffer_.clear();
        pos_ = 0;
    } else if (pos_ > (std::size_t{1} << 20)) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    buffer_.insert(buffer_.end(), data.begin(), data.end());
}

std::optional<Message> Parser::next() {
    const std::size_t avail = buffered();
    if (avail < header_size) return std::nullopt;
    const std::uint8_t* h = buffer_.data() + pos_;
    const std::uint32_t type = get32(h);
    auto need = [&](std::size_t n) { return n <= avail; };
    auto consume = [&](std::size_t n) { pos_ += n; };

    switch (static_cast<Msg>(type)) {
        case Msg::frame: {
            const std::uint32_t regions = get32(h + 20);
            if (regions > max_regions) throw ProtocolError(std::format("a frame with {} regions", regions));
            // Walk the region headers first: the frame is complete only when
            // every region's data is here.
            std::size_t size = header_size;
            for (std::uint32_t i = 0; i < regions; ++i) {
                if (!need(size + region_header_size)) return std::nullopt;
                const std::uint8_t* r = h + size;
                std::size_t data = get32(r) == 0 ? get32(r + 28) : 0;
                if (data > max_payload) throw ProtocolError(std::format("a frame region of {} bytes", data));
                size += region_header_size + data;
                if (size > max_payload) throw ProtocolError("a frame over 256 MB");
            }
            if (!need(size)) return std::nullopt;
            Frame f;
            f.width = get32(h + 4);
            f.height = get32(h + 8);
            f.x = static_cast<std::int32_t>(get32(h + 12));
            f.y = static_cast<std::int32_t>(get32(h + 16));
            f.crc = get32(h + 24);
            f.window = get32(h + 28);
            f.regions.reserve(regions);
            std::size_t at = header_size;
            for (std::uint32_t i = 0; i < regions; ++i) {
                const std::uint8_t* r = h + at;
                Region g;
                g.source_crc = get32(r);
                g.source_x = static_cast<std::int32_t>(get32(r + 4));
                g.source_y = static_cast<std::int32_t>(get32(r + 8));
                g.x = static_cast<std::int32_t>(get32(r + 12));
                g.y = static_cast<std::int32_t>(get32(r + 16));
                g.width = get32(r + 20);
                g.height = get32(r + 24);
                at += region_header_size;
                if (g.source_crc == 0) {
                    std::size_t n = get32(r + 28);
                    g.data.assign(h + at, h + at + n);
                    at += n;
                }
                f.regions.push_back(std::move(g));
            }
            consume(size);
            return f;
        }
        case Msg::deleted:
        case Msg::deleted_cursor: {
            const std::uint32_t count = get32(h + 4);
            if (count > max_count) throw ProtocolError(std::format("a list of {} deleted ids", count));
            const std::size_t size = header_size + std::size_t{count} * 4;
            if (!need(size)) return std::nullopt;
            std::vector<std::uint32_t> ids(count);
            for (std::uint32_t i = 0; i < count; ++i) ids[i] = get32(h + header_size + std::size_t{i} * 4);
            consume(size);
            if (static_cast<Msg>(type) == Msg::deleted) return Deleted{std::move(ids)};
            return DeletedCursors{std::move(ids)};
        }
        case Msg::cursor: {
            const std::uint32_t data = get32(h + 24);
            if (data > max_payload) throw ProtocolError(std::format("a cursor of {} bytes", data));
            const std::size_t size = header_size + data;
            if (!need(size)) return std::nullopt;
            Cursor c;
            for (int i = 0; i < 3; ++i) {
                c.fg[i] = h[4 + i];
                c.bg[i] = h[7 + i];
            }
            c.width = get16(h + 10);
            c.height = get16(h + 12);
            c.hot_x = get16(h + 14);
            c.hot_y = get16(h + 16);
            c.serial = get32(h + 20);
            c.data.assign(h + header_size, h + size);
            consume(size);
            return c;
        }
        case Msg::selection: {
            SelectionChunk s;
            s.selection = get32(h + 4);
            s.format = get32(h + 8);
            s.chunk_size = get32(h + 12);
            s.first = get32(h + 16) != 0;
            s.last = get32(h + 20) != 0;
            s.compressed_size = get32(h + 24);
            s.total = get32(h + 28);
            const std::size_t data = s.compressed_size ? s.compressed_size : s.chunk_size;
            if (data > max_payload) throw ProtocolError(std::format("a selection chunk of {} bytes", data));
            const std::size_t size = header_size + data;
            if (!need(size)) return std::nullopt;
            s.data.assign(h + header_size, h + size);
            consume(size);
            return s;
        }
        case Msg::server_version: {
            ServerVersion v{get16(h + 4)};
            consume(header_size);
            return v;
        }
        case Msg::demand_client_selection: {
            DemandClientSelection d{get16(h + 4)};
            consume(header_size);
            return d;
        }
        case Msg::win_update: {
            const std::uint32_t data = get32(h + 4);
            if (data > max_payload) throw ProtocolError(std::format("a window update of {} bytes", data));
            if (!need(header_size + data)) return std::nullopt;
            consume(header_size + data);
            return Signal{Msg::win_update};
        }
        case Msg::reinit:
        case Msg::srv_keepalive:
        case Msg::srv_disconnect:
        case Msg::udp_open:
        case Msg::udp_failed:
            consume(header_size);
            return Signal{static_cast<Msg>(type)};
        case Msg::cache_frame:
            // Only ever sent over UDP, which this client never opens.
            throw ProtocolError("a CACHEFRAME message on the TCP stream");
    }
    throw ProtocolError(std::format("unknown message type {}", type));
}

std::string_view to_string(Msg type) {
    switch (type) {
        case Msg::frame: return "FRAME";
        case Msg::deleted: return "DELETED";
        case Msg::cursor: return "CURSOR";
        case Msg::deleted_cursor: return "DELETEDCURSOR";
        case Msg::selection: return "SELECTION";
        case Msg::server_version: return "SERVERVERSION";
        case Msg::demand_client_selection: return "DEMANDCLIENTSELECTION";
        case Msg::reinit: return "REINIT";
        case Msg::win_update: return "WINUPDATE";
        case Msg::srv_keepalive: return "SRVKEEPALIVE";
        case Msg::srv_disconnect: return "SRVDISCONNECT";
        case Msg::cache_frame: return "CACHEFRAME";
        case Msg::udp_open: return "UDPOPEN";
        case Msg::udp_failed: return "UDPFAILED";
    }
    return "unknown";
}

}  // namespace x2go::kdrive
