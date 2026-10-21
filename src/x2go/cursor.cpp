#include "x2go/cursor.h"

#include <utility>

namespace x2go {

namespace {

constexpr int max_cursor_side = 1024;

}  // namespace

std::optional<CursorImage> decode_cursor(const kdrive::Cursor& c) {
    const int w = c.width, h = c.height;
    if (w <= 0 || h <= 0 || w > max_cursor_side || h > max_cursor_side) return std::nullopt;
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    CursorImage img;
    img.width = w;
    img.height = h;
    img.hot_x = c.hot_x;
    img.hot_y = c.hot_y;
    img.rgba.assign(pixels * 4, 0);

    if (c.data.size() == pixels * 4) {
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::uint8_t* p = c.data.data() + i * 4;
            const unsigned a = p[3];
            std::uint8_t* q = img.rgba.data() + i * 4;
            if (a == 0) continue;
            // Undo the premultiplication, rounding to nearest.
            auto straight = [a](unsigned v) {
                unsigned s = (v * 255 + a / 2) / a;
                return static_cast<std::uint8_t>(s > 255 ? 255 : s);
            };
            q[0] = straight(p[2]);
            q[1] = straight(p[1]);
            q[2] = straight(p[0]);
            q[3] = static_cast<std::uint8_t>(a);
        }
        return img;
    }

    if (c.data.size() == pixels * 2) {
        const std::size_t stride = static_cast<std::size_t>((w + 31) / 32) * 4;
        const std::uint8_t* source = c.data.data();
        const std::uint8_t* mask = c.data.data() + pixels;
        if (stride * static_cast<std::size_t>(h) > pixels) {
            // A cursor narrower than 4 bytes per row doesn't fit its own
            // padded bitmap into width x height bytes; never seen.
            return std::nullopt;
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t byte = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x / 8);
                const int bit = x % 8;
                if (!((mask[byte] >> bit) & 1)) continue;
                const std::uint8_t* colour = ((source[byte] >> bit) & 1) ? c.fg : c.bg;
                std::uint8_t* q = img.rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                                     static_cast<std::size_t>(x)) * 4;
                q[0] = colour[0];
                q[1] = colour[1];
                q[2] = colour[2];
                q[3] = 255;
            }
        }
        return img;
    }
    return std::nullopt;
}

const CursorImage* CursorCache::apply(const kdrive::Cursor& c) {
    if (c.data.empty()) {
        auto it = cursors_.find(c.serial);
        return it == cursors_.end() ? nullptr : &it->second;
    }
    auto img = decode_cursor(c);
    if (!img) return nullptr;
    auto [it, inserted] = cursors_.insert_or_assign(c.serial, std::move(*img));
    (void)inserted;
    return &it->second;
}

}  // namespace x2go
