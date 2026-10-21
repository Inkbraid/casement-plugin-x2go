// The X2Go KDrive protocol between x2gokdrive and its client, as spoken
// over the graphics port: a 32-byte cookie and fixed 41-byte events from
// the client; messages with a 56-byte header from the server. Integers are
// little-endian (the server writes them in its native order; only
// little-endian servers can talk to a little-endian client).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace x2go::kdrive {

/// The feature version this client announces: extended selections,
/// keepalives (and UDP, which it never asks for).
inline constexpr std::uint16_t feature_version = 3;
/// The operating system it announces. Windows clients get the clipboard
/// pushed, CLIPBOARD only.
inline constexpr std::uint16_t os_windows = 1;

inline constexpr std::size_t cookie_size = 32;
inline constexpr std::size_t event_size = 41;
inline constexpr std::size_t header_size = 56;
inline constexpr std::size_t region_header_size = 64;

/// Server to client message types.
enum class Msg : std::uint32_t {
    frame = 0,
    deleted = 1,
    cursor = 2,
    deleted_cursor = 3,
    selection = 4,
    server_version = 5,
    demand_client_selection = 6,
    reinit = 7,
    win_update = 8,
    srv_keepalive = 9,
    srv_disconnect = 10,
    cache_frame = 11,
    udp_open = 12,
    udp_failed = 13,
};

/// Client to server event types.
enum class Ev : std::uint32_t {
    key_press = 2,
    key_release = 3,
    mouse_press = 4,
    mouse_release = 5,
    mouse_motion = 6,
    geometry = 7,
    update = 8,
    selection = 9,
    client_version = 10,
    demand_selection = 11,
    keepalive = 12,
    cache_rebuild = 13,
    disconnect_client = 15,
};

/// X selections and the data formats the protocol knows.
inline constexpr std::uint8_t sel_primary = 0;
inline constexpr std::uint8_t sel_clipboard = 1;
inline constexpr std::uint8_t fmt_string = 0;
inline constexpr std::uint8_t fmt_utf8 = 1;
inline constexpr std::uint8_t fmt_pixmap = 2;

/// X modifier mask bits, as the server reads them from key and button
/// events.
inline constexpr std::uint32_t mask_shift = 1;
inline constexpr std::uint32_t mask_lock = 2;
inline constexpr std::uint32_t mask_control = 4;
inline constexpr std::uint32_t mask_mod1 = 8;
inline constexpr std::uint32_t mask_mod2 = 16;
inline constexpr std::uint32_t mask_mod3 = 32;
inline constexpr std::uint32_t mask_mod4 = 64;
inline constexpr std::uint32_t mask_mod5 = 128;

using Event = std::array<std::uint8_t, event_size>;

Event key_event(bool press, std::uint32_t state, std::uint32_t keycode);
Event button_event(bool press, std::uint32_t state, std::uint32_t button);
Event motion_event(std::uint32_t x, std::uint32_t y);
/// A new screen size, with one screen covering it.
Event geometry_event(std::uint16_t width, std::uint16_t height);
/// Asks for an area of the screen as a screen update.
Event update_event(std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t y);
Event client_version_event(std::uint16_t version = feature_version, std::uint16_t os = os_windows);
Event demand_selection_event(std::uint16_t selection);
/// KEEPALIVE, CACHEREBUILD, DISCONNECTCLIENT and other events without fields.
Event plain_event(Ev type);

/// Thrown when the server's stream can't be parsed. The stream has no
/// lengths of its own, so nothing after such a message can be read.
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Region {
    /// 0: `data` holds an image; otherwise copy from the cached frame with
    /// this crc.
    std::uint32_t source_crc = 0;
    std::int32_t source_x = 0;
    std::int32_t source_y = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> data;
};

struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    /// 0: not cached; otherwise the id the server uses for this picture.
    std::uint32_t crc = 0;
    std::uint32_t window = 0;
    std::vector<Region> regions;

    /// The whole screen: sent after GEOMETRY and after resuming.
    bool main_image() const { return crc == 0 && x == -1 && y == -1; }
};

struct Cursor {
    std::uint8_t fg[3] = {};
    std::uint8_t bg[3] = {};
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t hot_x = 0;
    std::uint16_t hot_y = 0;
    std::uint32_t serial = 0;
    /// Empty: the cursor with this serial was sent before.
    std::vector<std::uint8_t> data;
};

struct SelectionChunk {
    std::uint32_t selection = 0;
    std::uint32_t format = 0;
    /// Uncompressed size of this chunk.
    std::uint32_t chunk_size = 0;
    bool first = false;
    bool last = false;
    std::uint32_t compressed_size = 0;
    std::uint32_t total = 0;
    /// As sent: compressed when `compressed_size` is not 0.
    std::vector<std::uint8_t> data;

    /// A notification without data: the server has a new selection and
    /// sends it when asked with DEMANDSELECTION.
    bool notification() const { return first && last && chunk_size == 0 && total == 0; }
};

/// Frames the server dropped from its cache (DELETED).
struct Deleted {
    std::vector<std::uint32_t> crcs;
};
/// Cursors the server dropped (DELETEDCURSOR).
struct DeletedCursors {
    std::vector<std::uint32_t> serials;
};
struct ServerVersion {
    std::uint16_t version = 0;
};
struct DemandClientSelection {
    std::uint16_t selection = 0;
};
/// Messages without fields: REINIT, SRVKEEPALIVE, SRVDISCONNECT, and the
/// window and UDP messages a desktop session over TCP never gets.
struct Signal {
    Msg type;
};

using Message = std::variant<Frame, Cursor, SelectionChunk, Deleted, DeletedCursors, ServerVersion,
                             DemandClientSelection, Signal>;

/// Splits the server's byte stream into messages.
class Parser {
public:
    void feed(std::span<const std::uint8_t> data);
    /// The next complete message, or nothing until more bytes arrive.
    /// Throws ProtocolError for a stream it can't follow.
    std::optional<Message> next();
    /// Bytes received but not yet returned as messages.
    std::size_t buffered() const { return buffer_.size() - pos_; }

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t pos_ = 0;
};

std::string_view to_string(Msg type);

}  // namespace x2go::kdrive
