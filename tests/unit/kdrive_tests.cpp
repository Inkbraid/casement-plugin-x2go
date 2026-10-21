// The KDrive wire protocol against bytes captured from a real session.

#include "check.h"

#include <algorithm>
#include <random>
#include <string>
#include <utility>
#include <variant>

#include "fixtures.h"
#include "util.h"
#include "x2go/cursor.h"
#include "x2go/kdrive.h"

using namespace x2go;
using namespace x2go::kdrive;
using testing::as_vector;
using testing::bytes;

namespace {

Message parse_one(const char* fixture) {
    Parser p;
    auto b = bytes(fixture);
    p.feed(b);
    auto m = p.next();
    if (!m) throw std::runtime_error("no message");
    if (p.buffered() != 0) throw std::runtime_error("bytes left over");
    return std::move(*m);
}

template <class T>
T parse_as(const char* fixture) {
    Message m = parse_one(fixture);
    if (!std::holds_alternative<T>(m)) throw std::runtime_error("another message type");
    return std::get<T>(std::move(m));
}

}  // namespace

TEST(builds_events_as_the_server_took_them) {
    CHECK(as_vector(client_version_event()) == bytes(fixtures::client_clientversion));
    CHECK(as_vector(geometry_event(800, 600)) == bytes(fixtures::client_geometry));
    CHECK(as_vector(update_event(16, 16, 0, 0)) == bytes(fixtures::client_update16));
    CHECK(as_vector(motion_event(700, 500)) == bytes(fixtures::client_motion_root));
    CHECK(as_vector(key_event(true, 0, 38)) == bytes(fixtures::client_key_a));
    CHECK(as_vector(plain_event(Ev::cache_rebuild)) == bytes(fixtures::client_cacherebuild));
    CHECK(as_vector(plain_event(Ev::keepalive)) == bytes(fixtures::client_keepalive));
}

TEST(builds_input_events) {
    auto e = key_event(false, mask_shift | mask_mod2, 50);
    CHECK_EQ(testing::u32(e, 0), 3u);
    CHECK_EQ(testing::u32(e, 4), 17u);
    CHECK_EQ(testing::u32(e, 8), 50u);
    auto b = button_event(true, mask_control, 3);
    CHECK_EQ(testing::u32(b, 0), 4u);
    CHECK_EQ(testing::u32(b, 4), 4u);
    CHECK_EQ(testing::u32(b, 8), 3u);
    auto r = button_event(false, 0, 1);
    CHECK_EQ(testing::u32(r, 0), 5u);
    auto d = demand_selection_event(sel_clipboard);
    CHECK_EQ(testing::u32(d, 0), 11u);
    CHECK_EQ(d[4], 1);
    CHECK_EQ(testing::u32(plain_event(Ev::disconnect_client), 0), 15u);
    // GEOMETRY describes one screen covering the new size.
    auto g = geometry_event(1280, 720);
    CHECK_EQ(g[4] | g[5] << 8, 1280);
    CHECK_EQ(g[6] | g[7] << 8, 720);
    CHECK_EQ(g[8], 0);
    CHECK_EQ(g[9] | g[10] << 8, 1280);
    CHECK_EQ(g[11] | g[12] << 8, 720);
    for (std::size_t i = 13; i < event_size; ++i) CHECK_EQ(g[i], 0);
}

TEST(parses_server_version_and_signals) {
    CHECK_EQ(parse_as<ServerVersion>(fixtures::server_version).version, 8);
    CHECK(parse_as<Signal>(fixtures::reinit).type == Msg::reinit);
    CHECK(parse_as<Signal>(fixtures::srv_keepalive).type == Msg::srv_keepalive);
    CHECK(parse_as<Signal>(fixtures::srv_disconnect).type == Msg::srv_disconnect);
}

TEST(parses_frames) {
    auto main = parse_as<Frame>(fixtures::main_image);
    CHECK(main.main_image());
    CHECK_EQ(main.width, 800u);
    CHECK_EQ(main.height, 600u);
    REQUIRE(main.regions.size() == 1);
    CHECK_EQ(main.regions[0].width, 800u);
    CHECK_EQ(main.regions[0].source_crc, 0u);
    CHECK(main.regions[0].data.size() > 1000);
    CHECK_EQ(main.regions[0].data[0], 0xFF);
    CHECK_EQ(main.regions[0].data[1], 0xD8);

    auto glyph = parse_as<Frame>(fixtures::cached_glyph);
    CHECK(!glyph.main_image());
    CHECK_EQ(glyph.crc, 0xac921683u);
    CHECK_EQ(glyph.x, 134);
    CHECK_EQ(glyph.y, 63);
    CHECK_EQ(glyph.width, 12u);
    CHECK_EQ(glyph.height, 13u);
    REQUIRE(glyph.regions.size() == 1);
    CHECK_EQ(glyph.regions[0].x, 0);

    auto hit = parse_as<Frame>(fixtures::cache_hit_1);
    CHECK_EQ(hit.crc, glyph.crc);
    CHECK(hit.regions.empty());
    CHECK_EQ(hit.x, 140);

    auto tile = parse_as<Frame>(fixtures::update_png_tile);
    CHECK_EQ(tile.crc, 0u);
    CHECK_EQ(tile.x, 40);
    REQUIRE(tile.regions.size() == 1);
    CHECK_EQ(tile.regions[0].x, 40);  // absolute in screen updates
    CHECK_EQ(tile.regions[0].data[1], 'P');
}

TEST(parses_cursors) {
    auto xterm = parse_as<Cursor>(fixtures::cursor_xterm);
    CHECK_EQ(xterm.width, 9);
    CHECK_EQ(xterm.height, 16);
    CHECK_EQ(xterm.hot_x, 4);
    CHECK_EQ(xterm.hot_y, 8);
    CHECK_EQ(xterm.serial, 79u);
    CHECK_EQ(xterm.data.size(), 9u * 16u * 2u);
    CHECK_EQ(xterm.bg[0], 255);
    auto again = parse_as<Cursor>(fixtures::cursor_cached);
    CHECK_EQ(again.serial, 79u);
    CHECK(again.data.empty());
}

TEST(parses_selections) {
    auto plain = parse_as<SelectionChunk>(fixtures::selection_plain);
    CHECK_EQ(plain.selection, 1u);
    CHECK_EQ(plain.format, 1u);
    CHECK(plain.first && plain.last);
    CHECK_EQ(plain.compressed_size, 0u);
    CHECK(plain.data == bytes(std::string_view("fixture-clip")));
    auto packed = parse_as<SelectionChunk>(fixtures::selection_compressed);
    CHECK_EQ(packed.chunk_size, 3200u);
    CHECK_EQ(packed.compressed_size, 47u);
    CHECK_EQ(packed.data.size(), 47u);
    CHECK(!packed.notification());
}

TEST(parses_a_stream_cut_anywhere) {
    const char* order[] = {fixtures::server_version, fixtures::main_image,     fixtures::cursor_xterm,
                           fixtures::update_png_tile, fixtures::cached_glyph,  fixtures::cache_hit_1,
                           fixtures::cache_hit_2,     fixtures::cursor_root,   fixtures::cursor_cached,
                           fixtures::selection_plain, fixtures::selection_compressed, fixtures::reinit,
                           fixtures::srv_keepalive,   fixtures::srv_disconnect};
    std::vector<std::uint8_t> stream;
    std::vector<std::size_t> kinds;
    for (const char* f : order) {
        auto b = bytes(f);
        stream.insert(stream.end(), b.begin(), b.end());
        kinds.push_back(parse_one(f).index());
    }
    std::mt19937 rng(7);
    for (int round = 0; round < 20; ++round) {
        Parser p;
        std::vector<std::size_t> seen;
        std::size_t at = 0;
        while (at < stream.size()) {
            std::size_t n = std::min<std::size_t>(stream.size() - at, 1 + rng() % (round < 10 ? 7 : 3000));
            p.feed(std::span(stream.data() + at, n));
            at += n;
            while (auto m = p.next()) seen.push_back(m->index());
        }
        CHECK(seen == kinds);
        CHECK_EQ(p.buffered(), 0u);
    }
}

TEST(rejects_what_it_cannot_follow) {
    std::vector<std::uint8_t> unknown(header_size, 0);
    unknown[0] = 99;
    Parser p;
    p.feed(unknown);
    bool thrown = false;
    try {
        p.next();
    } catch (const ProtocolError&) {
        thrown = true;
    }
    CHECK(thrown);

    // A frame claiming thousands of regions.
    std::vector<std::uint8_t> frame(header_size, 0);
    frame[20] = 0x10;
    frame[21] = 0x27;
    Parser q;
    q.feed(frame);
    thrown = false;
    try {
        q.next();
    } catch (const ProtocolError&) {
        thrown = true;
    }
    CHECK(thrown);

    // Incomplete messages just wait.
    auto main = bytes(fixtures::main_image);
    Parser r;
    r.feed(std::span(main.data(), main.size() - 1));
    CHECK(!r.next());
    r.feed(std::span(main.data() + main.size() - 1, 1));
    CHECK(r.next().has_value());
}

TEST(decodes_core_cursors) {
    auto c = parse_as<Cursor>(fixtures::cursor_xterm);
    auto img = decode_cursor(c);
    REQUIRE(img);
    CHECK_EQ(img->width, 9);
    CHECK_EQ(img->height, 16);
    CHECK_EQ(img->hot_x, 4);
    CHECK_EQ(img->hot_y, 8);
    REQUIRE(img->rgba.size() == 9u * 16u * 4u);
    // The I-beam: a black stem with a white outline, transparent around it.
    auto px = [&](int x, int y) { return img->rgba.data() + (static_cast<std::size_t>(y) * 9 + static_cast<std::size_t>(x)) * 4; };
    CHECK_EQ(px(4, 8)[3], 255);
    CHECK_EQ(px(4, 8)[0], 0);
    CHECK_EQ(px(0, 8)[3], 0);
    int opaque = 0, white = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 9; ++x) {
            if (px(x, y)[3] == 255) ++opaque;
            if (px(x, y)[3] == 255 && px(x, y)[0] == 255) ++white;
        }
    CHECK(opaque > 20);
    CHECK(white > 0);

    auto root = decode_cursor(parse_as<Cursor>(fixtures::cursor_root));
    REQUIRE(root);
    CHECK_EQ(root->width, 16);
    CHECK_EQ(root->hot_x, 7);
}

TEST(decodes_argb_cursors_to_straight_alpha) {
    Cursor c;
    c.width = 2;
    c.height = 1;
    c.serial = 5;
    // B, G, R, A premultiplied: half-transparent (0x20, 0x40, 0x80) and opaque red.
    c.data = {0x40, 0x20, 0x10, 0x80, 0x00, 0x00, 0xFF, 0xFF};
    auto img = decode_cursor(c);
    REQUIRE(img);
    std::vector<std::uint8_t> want{0x20, 0x40, 0x80, 0x80, 0xFF, 0x00, 0x00, 0xFF};
    CHECK(img->rgba == want);

    CursorCache cache;
    CHECK(cache.apply(c) != nullptr);
    Cursor ref;
    ref.serial = 5;
    const CursorImage* again = cache.apply(ref);
    REQUIRE(again);
    CHECK(again->rgba == want);
    cache.clear();
    CHECK(cache.apply(ref) == nullptr);

    Cursor odd = c;
    odd.data.pop_back();
    CHECK(!decode_cursor(odd));
}
