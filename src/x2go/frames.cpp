#include "x2go/frames.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace x2go {

namespace {

constexpr std::uint32_t max_side = 16384;

std::shared_ptr<Image> blank(std::uint32_t width, std::uint32_t height) {
    auto img = std::make_shared<Image>();
    img->width = width;
    img->height = height;
    img->pixels.assign(std::size_t{width} * height * 4, 0);
    return img;
}

/// Copies the rectangle (sx, sy, w, h) of `src` to (dx, dy) of `dst`. False
/// when the rectangle isn't inside both.
bool copy_rect(const Image& src, std::int64_t sx, std::int64_t sy, std::int64_t w, std::int64_t h, Image& dst,
               std::int64_t dx, std::int64_t dy) {
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0 || w < 0 || h < 0) return false;
    if (sx + w > src.width || sy + h > src.height || dx + w > dst.width || dy + h > dst.height) return false;
    const std::size_t bytes = static_cast<std::size_t>(w) * 4;
    for (std::int64_t row = 0; row < h; ++row) {
        std::memcpy(dst.pixels.data() + static_cast<std::size_t>(dy + row) * dst.stride() + static_cast<std::size_t>(dx) * 4,
                    src.pixels.data() + static_cast<std::size_t>(sy + row) * src.stride() + static_cast<std::size_t>(sx) * 4,
                    bytes);
    }
    return true;
}

}  // namespace

Rect draw(const Image& src, std::int32_t x, std::int32_t y, std::uint8_t* dst, std::size_t dst_stride,
          std::uint32_t dst_width, std::uint32_t dst_height) {
    const std::int64_t x0 = std::max<std::int64_t>(x, 0);
    const std::int64_t y0 = std::max<std::int64_t>(y, 0);
    const std::int64_t x1 = std::min<std::int64_t>(std::int64_t{x} + src.width, dst_width);
    const std::int64_t y1 = std::min<std::int64_t>(std::int64_t{y} + src.height, dst_height);
    if (x1 <= x0 || y1 <= y0) return {};
    const std::size_t bytes = static_cast<std::size_t>(x1 - x0) * 4;
    for (std::int64_t row = y0; row < y1; ++row) {
        std::memcpy(dst + static_cast<std::size_t>(row) * dst_stride + static_cast<std::size_t>(x0) * 4,
                    src.pixels.data() + static_cast<std::size_t>(row - y) * src.stride() +
                        static_cast<std::size_t>(x0 - x) * 4,
                    bytes);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

bool FrameCache::decode(const kdrive::Region& r, Image& out, std::string& error) {
    if (!decoder_(r.data, out)) {
        error = std::format("an image of {} bytes ({}x{}) didn't decode", r.data.size(), r.width, r.height);
        return false;
    }
    if (out.width != r.width || out.height != r.height) {
        error = std::format("an image decoded to {}x{} instead of {}x{}", out.width, out.height, r.width, r.height);
        return false;
    }
    return true;
}

FrameCache::Result FrameCache::apply(const kdrive::Frame& f, Blit& out, std::string& error) {
    out = Blit{};
    if (f.width == 0 || f.height == 0 || f.width > max_side || f.height > max_side) {
        error = std::format("a frame of {}x{}", f.width, f.height);
        return Result::bad;
    }

    if (f.crc != 0 && f.regions.empty()) {
        auto it = frames_.find(f.crc);
        if (it == frames_.end()) {
            error = std::format("cached frame {:08x} is missing", f.crc);
            return Result::miss;
        }
        out.image = it->second;
        out.x = f.x;
        out.y = f.y;
        return Result::ok;
    }
    if (f.regions.empty()) {
        error = "a frame without regions or crc";
        return Result::bad;
    }

    // Region positions are relative to the frame in cached frames, and
    // absolute (the frame's own position) in screen updates.
    const bool update = f.crc == 0 && !f.main_image();
    const std::int64_t ox = update ? f.x : 0;
    const std::int64_t oy = update ? f.y : 0;

    std::shared_ptr<Image> picture;
    const kdrive::Region& first = f.regions.front();
    if (f.regions.size() == 1 && first.source_crc == 0 && first.x - ox == 0 && first.y - oy == 0 &&
        first.width == f.width && first.height == f.height) {
        // The usual case: one image covering the frame.
        picture = std::make_shared<Image>();
        if (!decode(first, *picture, error)) return Result::bad;
    } else {
        picture = blank(f.width, f.height);
        for (const auto& r : f.regions) {
            const std::int64_t dx = r.x - ox, dy = r.y - oy;
            if (r.source_crc != 0) {
                auto it = frames_.find(r.source_crc);
                if (it == frames_.end()) {
                    error = std::format("cached frame {:08x} is missing", r.source_crc);
                    return Result::miss;
                }
                if (!copy_rect(*it->second, r.source_x, r.source_y, r.width, r.height, *picture, dx, dy)) {
                    error = std::format("a copy of {}x{} from {},{} to {},{} doesn't fit", r.width, r.height,
                                        r.source_x, r.source_y, dx, dy);
                    return Result::bad;
                }
            } else {
                Image img;
                if (!decode(r, img, error)) return Result::bad;
                if (!copy_rect(img, 0, 0, img.width, img.height, *picture, dx, dy)) {
                    error = std::format("a region of {}x{} at {},{} doesn't fit a frame of {}x{}", img.width,
                                        img.height, dx, dy, f.width, f.height);
                    return Result::bad;
                }
            }
        }
    }

    if (f.main_image()) {
        out.resize = std::pair{f.width, f.height};
        out.x = out.y = 0;
    } else {
        out.x = f.x;
        out.y = f.y;
    }
    if (f.crc != 0) frames_[f.crc] = picture;
    out.image = std::move(picture);
    return Result::ok;
}

void FrameCache::erase(std::span<const std::uint32_t> crcs) {
    for (std::uint32_t crc : crcs) frames_.erase(crc);
}

}  // namespace x2go
