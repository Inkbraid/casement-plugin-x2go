// Clipboard data in the KDrive protocol.
//
// Client to server: SELECTIONEVENT, a 20-byte header with the first 21
// bytes of the chunk in one 41-byte event, the rest of the chunk in raw
// 41-byte blocks that must follow it directly. Text over 1024 bytes is
// zlib-compressed.
//
// Server to client: SELECTION chunks, zlib-compressed when the server
// chose to, concatenated until the last one.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "x2go/kdrive.h"

namespace x2go {

/// The event stream for one selection, ready to send as one piece.
/// `max_chunk` splits large data into chunks the server reassembles.
std::vector<std::uint8_t> selection_events(std::span<const std::uint8_t> data, std::uint8_t selection,
                                           std::uint8_t format, std::size_t max_chunk = std::size_t{1} << 20);

/// zlib (RFC 1950) helpers. Empty optional on failure.
std::optional<std::vector<std::uint8_t>> zlib_compress(std::span<const std::uint8_t> data);
std::optional<std::vector<std::uint8_t>> zlib_inflate(std::span<const std::uint8_t> data, std::size_t size);

/// Reassembles selection chunks from the server.
class SelectionReceiver {
public:
    struct Complete {
        std::uint32_t selection = 0;
        std::uint32_t format = 0;
        std::vector<std::uint8_t> data;
    };

    /// Adds a chunk; returns the selection once its last chunk is in.
    /// Notifications (no data) are not passed here. `error` is set for a
    /// chunk that can't be used.
    std::optional<Complete> add(const kdrive::SelectionChunk& chunk, std::string& error);

private:
    bool active_ = false;
    std::uint32_t selection_ = 0;
    std::uint32_t format_ = 0;
    std::vector<std::uint8_t> data_;
};

/// Clipboard text for Casement from selection data: UTF-8 as it comes
/// (format UTF_STRING), Latin-1 converted (STRING), trailing NULs dropped.
/// Nothing for images or data that isn't text.
std::optional<std::string> selection_text(std::uint32_t format, std::span<const std::uint8_t> data);

/// The largest clipboard text sent to the server.
inline constexpr std::size_t max_clipboard = std::size_t{16} << 20;

}  // namespace x2go
