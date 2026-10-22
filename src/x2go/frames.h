// The client's side of x2gokdrive's picture: decoding frames, the frame
// cache the server refers to by crc, and what to draw where.
//
// A frame is one of four kinds:
// - main image (crc 0, x = y = -1): the whole screen at a new size;
// - screen update (crc 0): one image drawn at the frame's position;
// - new cached frame (crc != 0, regions > 0): built from decoded images and
//   copies out of other cached frames, stored, then drawn;
// - cache hit (crc != 0, no regions): a stored frame drawn again.
// DELETED drops entries the server dropped; REINIT drops them all.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "x2go/kdrive.h"

namespace x2go {

/// A BGRX picture, 4 bytes per pixel, rows without padding.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;

    std::size_t stride() const { return std::size_t{width} * 4; }
};

/// Decodes a JPEG or PNG file into `out`. False when it can't.
using Decoder = std::function<bool(std::span<const std::uint8_t> data, Image& out)>;

/// What a frame changes on the screen.
struct Blit {
    /// The screen's new size: set it (which clears it) before drawing.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> resize;
    std::shared_ptr<const Image> image;
    std::int32_t x = 0;
    std::int32_t y = 0;
};

/// Copies `src` to (x, y) of a BGRX buffer, clipped to it. Returns the
/// rectangle drawn (empty when nothing is visible).
struct Rect {
    std::int64_t x = 0, y = 0, w = 0, h = 0;
};
Rect draw(const Image& src, std::int32_t x, std::int32_t y, std::uint8_t* dst, std::size_t dst_stride,
          std::uint32_t dst_width, std::uint32_t dst_height);

class FrameCache {
public:
    explicit FrameCache(Decoder decoder) : decoder_(std::move(decoder)) {}

    enum class Result {
        ok,
        /// The frame refers to a cached frame the client doesn't have; the
        /// picture can't be trusted until the caches are rebuilt.
        miss,
        /// An image didn't decode or a region doesn't fit.
        bad,
    };

    /// Turns a frame into what to draw. `error` explains `bad` and `miss`.
    Result apply(const kdrive::Frame& frame, Blit& out, std::string& error);

    void erase(std::span<const std::uint32_t> crcs);
    void clear() { frames_.clear(); }
    std::size_t size() const { return frames_.size(); }
    bool contains(std::uint32_t crc) const { return frames_.count(crc) != 0; }

private:
    bool decode(const kdrive::Region& r, Image& out, std::string& error);

    Decoder decoder_;
    std::map<std::uint32_t, std::shared_ptr<const Image>> frames_;
};

}  // namespace x2go
