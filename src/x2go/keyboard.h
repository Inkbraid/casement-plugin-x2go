// Keyboard and pointer input for x2gokdrive.
//
// The server types characters through its own XKB keymap (rules evdev,
// the layout chosen at session start), so the client sends positions on
// the keyboard: X keycodes, which are Linux evdev codes + 8. Casement's
// set-1 scancodes map onto them with a table.
//
// How the server applies key events shapes everything here: every
// KEYPRESS is a complete key stroke (press and release); the modifier mask
// sent with a key or button event is enforced by pressing or releasing the
// server's own key for each modifier bit; and that comparison uses the
// state from before the current batch of events (up to 99 per read). So:
// - modifier keys only change the mask; a modifier pressed and released
//   alone is sent as a tap;
// - releasing a modifier is sent as an explicit release of the key the
//   server pressed for it, which keeps the order inside a batch;
// - Caps Lock and Num Lock are sent as keys, followed by a barrier (a
//   round trip) before the mask shows the new lock state; a barrier also
//   comes before a modifier is set again that was released since the last
//   one, because the server could miss that inside a batch.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "x2go/kdrive.h"

namespace x2go::keys {

/// The X keycode for a Casement scancode (set 1, 0x100 added for E0
/// keys), or 0 for keys without one.
std::uint8_t x_keycode(std::uint32_t scancode);

/// Whether right Alt is AltGr (ISO_Level3_Shift, modifier Mod5) in an XKB
/// layout such as "de" or "us(intl)"; otherwise it is Alt (Mod1).
bool ralt_is_altgr(std::string_view layout);

/// Checks a layout as the connection gives it: "de", "de(nodeadkeys)".
bool valid_layout(std::string_view layout);

/// The XKB layout for a Windows keyboard layout id (KLID such as
/// "00000407"), or empty when there is no good match.
std::string layout_for_klid(std::string_view klid);

/// The XKB layout for the keyboard layout of this process's input
/// language; "us" when there is no good match.
std::string windows_layout();

/// Initial Caps Lock and Num Lock state, as Windows reports it to this
/// process when it starts.
struct LockState {
    bool caps = false;
    bool num = false;
};
LockState windows_lock_state();

struct RepeatTiming {
    std::chrono::milliseconds delay{650};
    std::chrono::milliseconds interval{33};
};
/// Windows' keyboard repeat delay and rate, with a margin added to the
/// delay so that repeats Casement forwards itself arrive first.
RepeatTiming windows_repeat_timing();

struct Action {
    enum class Kind { event, barrier };
    Kind kind = Kind::event;
    kdrive::Event event{};
};

class Keyboard {
public:
    using Clock = std::chrono::steady_clock;

    Keyboard(bool ralt_altgr, LockState locks, RepeatTiming timing = {});

    /// A `key` message from Casement.
    std::vector<Action> key(std::uint32_t scancode, bool down, Clock::time_point now);
    /// A `pointer` message: position and the button mask (1 left, 2 middle,
    /// 4 right).
    std::vector<Action> pointer(std::int32_t x, std::int32_t y, std::uint32_t buttons);
    /// A `wheel` message: notches, positive away from the user.
    std::vector<Action> wheel(std::int32_t delta);
    /// Key repeats this side generates, when Casement doesn't send its own.
    std::vector<Action> tick(Clock::time_point now);
    /// When `tick` has something to do next.
    std::optional<Clock::time_point> next_tick() const;

    /// The modifier mask for the next event.
    std::uint32_t mask() const;
    /// Whether Casement was seen sending key repeats (a second key down
    /// without an up).
    bool casement_repeats() const { return casement_repeats_; }

private:
    enum Mod { lshift, rshift, lctrl, rctrl, lalt, ralt, lwin, rwin, mod_count };

    std::uint32_t bit(int mod) const;
    void commit_lctrl();
    void send(std::vector<Action>& out, bool press, std::uint8_t keycode, std::uint32_t mask);
    void send_button(std::vector<Action>& out, bool press, std::uint32_t button);
    void barrier(std::vector<Action>& out);

    bool ralt_altgr_;
    RepeatTiming timing_;
    std::array<bool, mod_count> held_{};
    int tap_ = -1;
    bool pending_lctrl_ = false;
    Clock::time_point pending_lctrl_at_{};
    bool fake_lctrl_ = false;
    bool caps_;
    bool num_;
    std::uint32_t sent_mask_ = 0;
    std::uint32_t cleared_ = 0;
    bool pause_prefix_ = false;
    bool pause_up_ = false;
    std::set<std::uint32_t> down_;
    std::set<std::uint32_t> locks_down_;
    std::int64_t pointer_x_ = -1;
    std::int64_t pointer_y_ = -1;
    std::uint32_t buttons_ = 0;
    bool casement_repeats_ = false;

    struct Repeat {
        std::uint32_t scancode = 0;
        std::uint8_t keycode = 0;
        Clock::time_point started{};
        Clock::time_point next{};
    };
    std::optional<Repeat> repeat_;
};

}  // namespace x2go::keys
