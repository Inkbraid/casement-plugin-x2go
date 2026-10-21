// The frame cache and drawing, with the captured frames decoded by WIC
// and synthetic frames decoded by a stand-in.

#include "check.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include "fixtures.h"
#include "util.h"
#include "x2go/frames.h"
#include "x2go/image.h"
#include "x2go/kdrive.h"

using namespace x2go;
using testing::bytes;

namespace {

kdrive::Frame frame_of(const char* fixture) {
    kdrive::Parser p;
    p.feed(bytes(fixture));
    auto m = p.next();
    return std::get<kdrive::Frame>(std::move(*m));
}

/// A stand-in image format for synthetic frames: width, height (one byte
/// each) and one grey level for the whole image.
bool fake_decode(std::span<const std::uint8_t> data, Image& out) {
    if (data.size() != 3) return false;
    out.width = data[0];
    out.height = data[1];
    out.pixels.assign(std::size_t{out.width} * out.height * 4, data[2]);
    return true;
}

kdrive::Region data_region(std::int32_t x, std::int32_t y, std::uint8_t w, std::uint8_t h, std::uint8_t grey) {
    kdrive::Region r;
    r.x = x;
    r.y = y;
    r.width = w;
    r.height = h;
    r.data = {w, h, grey};
    return r;
}

std::uint8_t grey_at(const Image& img, std::uint32_t x, std::uint32_t y) {
    return img.pixels[(std::size_t{y} * img.width + x) * 4];
}

bool close_to(int a, int b, int tolerance) { return std::abs(a - b) <= tolerance; }

}  // namespace

TEST(wic_decodes_the_captured_jpeg_and_png) {
    std::string error;
    WicDecoder wic(error);
    REQUIRE(error.empty());
    auto main = frame_of(fixtures::main_image);
    Image img;
    REQUIRE(wic.decode(main.regions[0].data, img));
    CHECK_EQ(img.width, 800u);
    CHECK_EQ(img.height, 600u);
    // Taken before the desktop started: black.
    const std::uint8_t* p = img.pixels.data() + (std::size_t{599} * 800 + 799) * 4;
    CHECK(p[0] < 12 && p[1] < 12 && p[2] < 12);

    auto tile = frame_of(fixtures::update_png_tile);
    Image t;
    REQUIRE(wic.decode(tile.regions[0].data, t));
    CHECK_EQ(t.width, 40u);
    CHECK_EQ(t.height, 40u);

    auto small = frame_of(fixtures::update_jpeg_16);
    Image s;
    REQUIRE(wic.decode(small.regions[0].data, s));
    CHECK_EQ(s.width, 16u);
    // The test desktop's root window, #336699 (BGRX in memory).
    const std::uint8_t* q = s.pixels.data();
    CHECK(close_to(q[0], 0x99, 12) && close_to(q[1], 0x66, 12) && close_to(q[2], 0x33, 12));

    CHECK(!wic.decode(std::vector<std::uint8_t>{1, 2, 3}, s));
}

TEST(applies_captured_frames) {
    std::string error;
    WicDecoder wic(error);
    FrameCache cache([&wic](std::span<const std::uint8_t> d, Image& out) { return wic.decode(d, out); });
    Blit b;
    std::string why;
    REQUIRE(cache.apply(frame_of(fixtures::main_image), b, why) == FrameCache::Result::ok);
    REQUIRE(b.resize);
    CHECK_EQ(b.resize->first, 800u);
    CHECK_EQ(b.x, 0);
    CHECK_EQ(cache.size(), 0u);  // main images aren't cached

    REQUIRE(cache.apply(frame_of(fixtures::cached_glyph), b, why) == FrameCache::Result::ok);
    CHECK(!b.resize);
    CHECK_EQ(b.x, 134);
    CHECK_EQ(b.y, 63);
    CHECK(cache.contains(0xac921683u));
    auto glyph = b.image;

    REQUIRE(cache.apply(frame_of(fixtures::cache_hit_1), b, why) == FrameCache::Result::ok);
    CHECK(b.image == glyph);
    CHECK_EQ(b.x, 140);
    REQUIRE(cache.apply(frame_of(fixtures::cache_hit_2), b, why) == FrameCache::Result::ok);
    CHECK_EQ(b.x, 146);

    REQUIRE(cache.apply(frame_of(fixtures::update_png_tile), b, why) == FrameCache::Result::ok);
    CHECK_EQ(b.x, 40);
    CHECK_EQ(b.y, 40);
    CHECK_EQ(b.image->width, 40u);

    const std::uint32_t gone[] = {0xac921683u};
    cache.erase(gone);
    CHECK(cache.apply(frame_of(fixtures::cache_hit_1), b, why) == FrameCache::Result::miss);
    CHECK(!why.empty());
}

TEST(builds_frames_from_regions_and_cached_copies) {
    FrameCache cache(fake_decode);
    Blit b;
    std::string why;

    // A cached 20x10 frame from two images side by side.
    kdrive::Frame f;
    f.width = 20;
    f.height = 10;
    f.x = 100;
    f.y = 50;
    f.crc = 7;
    f.regions = {data_region(0, 0, 10, 10, 0x11), data_region(10, 0, 10, 10, 0x22)};
    REQUIRE(cache.apply(f, b, why) == FrameCache::Result::ok);
    CHECK_EQ(b.x, 100);
    CHECK_EQ(grey_at(*b.image, 0, 0), 0x11);
    CHECK_EQ(grey_at(*b.image, 19, 9), 0x22);

    // A new frame copying the right half of frame 7 and adding an image.
    kdrive::Frame g;
    g.width = 10;
    g.height = 20;
    g.crc = 8;
    kdrive::Region copy;
    copy.source_crc = 7;
    copy.source_x = 10;
    copy.source_y = 0;
    copy.x = 0;
    copy.y = 0;
    copy.width = 10;
    copy.height = 10;
    g.regions = {copy, data_region(0, 10, 10, 10, 0x33)};
    REQUIRE(cache.apply(g, b, why) == FrameCache::Result::ok);
    CHECK_EQ(grey_at(*b.image, 0, 0), 0x22);
    CHECK_EQ(grey_at(*b.image, 5, 15), 0x33);
    CHECK_EQ(cache.size(), 2u);

    // A copy from a frame the cache doesn't have.
    g.crc = 9;
    g.regions[0].source_crc = 99;
    CHECK(cache.apply(g, b, why) == FrameCache::Result::miss);

    // A region that doesn't fit its frame.
    kdrive::Frame bad;
    bad.width = 5;
    bad.height = 5;
    bad.crc = 10;
    bad.regions = {data_region(0, 0, 10, 10, 1)};
    CHECK(cache.apply(bad, b, why) == FrameCache::Result::bad);

    // An image that doesn't decode.
    kdrive::Frame broken;
    broken.width = 10;
    broken.height = 10;
    broken.x = 3;
    broken.y = 4;
    kdrive::Region r = data_region(3, 4, 10, 10, 1);
    r.data = {1};
    broken.regions = {r};
    CHECK(cache.apply(broken, b, why) == FrameCache::Result::bad);

    // A screen update: region positions are absolute.
    kdrive::Frame update;
    update.width = 10;
    update.height = 10;
    update.x = 30;
    update.y = 40;
    update.regions = {data_region(30, 40, 10, 10, 0x44)};
    REQUIRE(cache.apply(update, b, why) == FrameCache::Result::ok);
    CHECK_EQ(b.x, 30);
    CHECK_EQ(grey_at(*b.image, 9, 9), 0x44);
    CHECK_EQ(cache.size(), 2u);

    cache.clear();
    CHECK_EQ(cache.size(), 0u);
}

TEST(draws_clipped_to_the_screen) {
    Image img;
    img.width = 4;
    img.height = 4;
    img.pixels.assign(64, 0xAB);
    std::vector<std::uint8_t> screen(10 * 10 * 4, 0);
    Rect r = draw(img, 8, -2, screen.data(), 40, 10, 10);
    CHECK_EQ(r.x, 8);
    CHECK_EQ(r.y, 0);
    CHECK_EQ(r.w, 2);
    CHECK_EQ(r.h, 2);
    CHECK_EQ(screen[(0 * 10 + 8) * 4], 0xAB);
    CHECK_EQ(screen[(1 * 10 + 9) * 4 + 3], 0xAB);
    CHECK_EQ(screen[(2 * 10 + 9) * 4], 0);
    CHECK_EQ(screen[(0 * 10 + 7) * 4], 0);
    Rect none = draw(img, 20, 20, screen.data(), 40, 10, 10);
    CHECK_EQ(none.w, 0);
}
