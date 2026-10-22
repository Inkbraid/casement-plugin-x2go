// Cursor shapes from x2gokdrive's CURSOR messages, as straight RGBA for
// Casement. Two kinds arrive, told apart by their data size:
// - width x height x 4: an ARGB cursor, 32-bit pixels with premultiplied
//   alpha, bytes B, G, R, A;
// - width x height x 2: a core cursor: the source bitmap and then the mask
//   bitmap, each copied as width x height bytes from server memory, of
//   which only the first rows of ((width + 31) / 32) * 4 bytes, least
//   significant bit first, are the bitmap.
// A message with no data refers to a cursor sent before, by serial.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "x2go/kdrive.h"

namespace x2go {

struct CursorImage {
    int width = 0;
    int height = 0;
    int hot_x = 0;
    int hot_y = 0;
    std::vector<std::uint8_t> rgba;
};

/// Decodes a cursor that carries data. Nothing for an unknown layout.
std::optional<CursorImage> decode_cursor(const kdrive::Cursor& c);

/// Cursors by serial: the server never deletes them in practice, so the
/// cache grows by one entry per distinct cursor until REINIT.
class CursorCache {
public:
    /// The image for a CURSOR message: decoded and stored, or looked up.
    const CursorImage* apply(const kdrive::Cursor& c);
    void erase(std::uint32_t serial) { cursors_.erase(serial); }
    void clear() { cursors_.clear(); }

private:
    std::map<std::uint32_t, CursorImage> cursors_;
};

}  // namespace x2go
