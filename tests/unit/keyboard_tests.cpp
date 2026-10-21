// Scancode translation and the key event scheme.

#include "check.h"

#include <format>
#include <string>

#include "util.h"
#include "x2go/keyboard.h"

using namespace x2go;
using namespace x2go::keys;
using testing::u32;
using Clock = Keyboard::Clock;
using namespace std::chrono_literals;

namespace {

/// Actions as text: "P38/1" key press of 38 with mask 1, "R50/0" release,
/// "B" barrier, "M10,20" motion, "D1/0" and "U1/0" button down and up.
std::string show(const std::vector<Action>& actions) {
    std::string out;
    for (const auto& a : actions) {
        if (!out.empty()) out += ' ';
        if (a.kind == Action::Kind::barrier) {
            out += 'B';
            continue;
        }
        const auto& e = a.event;
        switch (u32(e, 0)) {
            case 2: out += std::format("P{}/{}", u32(e, 8), u32(e, 4)); break;
            case 3: out += std::format("R{}/{}", u32(e, 8), u32(e, 4)); break;
            case 4: out += std::format("D{}/{}", u32(e, 8), u32(e, 4)); break;
            case 5: out += std::format("U{}/{}", u32(e, 8), u32(e, 4)); break;
            case 6: out += std::format("M{},{}", u32(e, 4), u32(e, 8)); break;
            default: out += '?';
        }
    }
    return out;
}

struct Typist {
    Keyboard kb;
    Clock::time_point now = Clock::time_point{} + 1h;
    explicit Typist(bool altgr = true, LockState locks = {}) : kb(altgr, locks, RepeatTiming{600ms, 40ms}) {}
    std::string key(std::uint32_t sc, bool down) {
        now += 5ms;
        return show(kb.key(sc, down, now));
    }
    std::string tap(std::uint32_t sc) {
        std::string down = key(sc, true);
        return down + " | " + key(sc, false);
    }
};

}  // namespace

TEST(translates_scancodes_to_x_keycodes) {
    CHECK_EQ(x_keycode(0x01), 9);     // Esc
    CHECK_EQ(x_keycode(0x1E), 38);    // A
    CHECK_EQ(x_keycode(0x39), 65);    // space
    CHECK_EQ(x_keycode(0x2A), 50);    // left Shift
    CHECK_EQ(x_keycode(0x56), 94);    // the ISO key next to left Shift
    CHECK_EQ(x_keycode(0x57), 95);    // F11
    CHECK_EQ(x_keycode(0x54), 107);   // SysRq
    CHECK_EQ(x_keycode(0x59), 125);   // keypad =
    CHECK_EQ(x_keycode(0x64), 191);   // F13
    CHECK_EQ(x_keycode(0x6E), 201);   // F23
    CHECK_EQ(x_keycode(0x76), 202);   // F24
    CHECK_EQ(x_keycode(0x73), 97);    // Ro
    CHECK_EQ(x_keycode(0x11C), 104);  // keypad Enter
    CHECK_EQ(x_keycode(0x11D), 105);  // right Ctrl
    CHECK_EQ(x_keycode(0x135), 106);  // keypad /
    CHECK_EQ(x_keycode(0x137), 107);  // Print Screen
    CHECK_EQ(x_keycode(0x138), 108);  // AltGr
    CHECK_EQ(x_keycode(0x148), 111);  // Up
    CHECK_EQ(x_keycode(0x14B), 113);  // Left
    CHECK_EQ(x_keycode(0x153), 119);  // Delete
    CHECK_EQ(x_keycode(0x15B), 133);  // left Windows
    CHECK_EQ(x_keycode(0x15D), 135);  // Menu
    CHECK_EQ(x_keycode(0x120), 121);  // mute
    CHECK_EQ(x_keycode(0x12A), 0);    // the fake Shift of E0 sequences
    CHECK_EQ(x_keycode(0x55), 0);
    CHECK_EQ(x_keycode(0x245), 0);
}

TEST(knows_which_layouts_have_altgr) {
    CHECK(ralt_is_altgr("de"));
    CHECK(ralt_is_altgr("de(nodeadkeys)"));
    CHECK(ralt_is_altgr("gb"));
    CHECK(ralt_is_altgr("fr"));
    CHECK(!ralt_is_altgr("us"));
    CHECK(!ralt_is_altgr("ru"));
    CHECK(!ralt_is_altgr("jp"));
    CHECK(ralt_is_altgr("us(intl)"));
    CHECK(ralt_is_altgr("us(altgr-intl)"));
    CHECK(ralt_is_altgr("ru(phonetic_azerty)"));
    CHECK(!ralt_is_altgr("fr(dvorak)"));
    CHECK(!ralt_is_altgr("gr(simple)"));
    CHECK(ralt_is_altgr("zz"));  // unknown: AltGr is the safe guess
}

TEST(checks_and_derives_layout_names) {
    CHECK(valid_layout("de"));
    CHECK(valid_layout("de(nodeadkeys)"));
    CHECK(valid_layout("in(tamilnet_TAB)"));
    CHECK(!valid_layout(""));
    CHECK(!valid_layout("DE"));
    CHECK(!valid_layout("de;reboot"));
    CHECK(!valid_layout("de("));
    CHECK(!valid_layout("de()"));
    CHECK(!valid_layout("de(x y)"));
    CHECK_EQ(layout_for_klid("00000407"), std::string("de"));
    CHECK_EQ(layout_for_klid("00000809"), std::string("gb"));
    CHECK_EQ(layout_for_klid("00020409"), std::string("us(intl)"));
    CHECK_EQ(layout_for_klid("0000100c"), std::string("ch(fr)"));
    CHECK_EQ(layout_for_klid("E0010411"), std::string("jp"));  // an IME: the language's layout
    CHECK_EQ(layout_for_klid("00001234"), std::string());
    CHECK(valid_layout(windows_layout()));
}

TEST(types_plain_and_shifted_keys) {
    Typist t;
    CHECK_EQ(t.tap(0x1E), std::string("P38/0 | R38/0"));
    CHECK_EQ(t.key(0x2A, true), std::string(""));  // Shift only changes the mask
    CHECK_EQ(t.tap(0x1E), std::string("P38/1 | R38/1"));
    // Releasing Shift releases the server's Shift key explicitly.
    CHECK_EQ(t.key(0x2A, false), std::string("R50/1"));
    CHECK_EQ(t.tap(0x30), std::string("P56/0 | R56/0"));
}

TEST(taps_lone_modifiers) {
    Typist t;
    CHECK_EQ(t.tap(0x15B), std::string(" | P133/0"));  // Windows key alone: Super tap
    CHECK_EQ(t.key(0x1D, true), std::string(""));
    CHECK_EQ(t.key(0x1D, false), std::string("P37/0"));
    // Ctrl+Super: the tap carries the Control bit.
    CHECK_EQ(t.key(0x1D, true), std::string(""));
    CHECK_EQ(t.key(0x15B, true), std::string(""));
    CHECK_EQ(t.key(0x15B, false), std::string("P133/4"));
    CHECK_EQ(t.key(0x1D, false), std::string("R37/4"));
}

TEST(waits_for_the_server_before_setting_a_released_modifier_again) {
    Typist t;
    t.key(0x2A, true);
    CHECK_EQ(t.tap(0x1E), std::string("P38/1 | R38/1"));
    CHECK_EQ(t.key(0x2A, false), std::string("R50/1"));
    CHECK_EQ(t.tap(0x30), std::string("P56/0 | R56/0"));
    t.key(0x2A, true);
    // Shift was released in what the server may still read as one batch.
    CHECK_EQ(t.tap(0x2E), std::string("B P54/1 | R54/1"));
    CHECK_EQ(t.key(0x2A, false), std::string("R50/1"));
}

TEST(sends_lock_keys_with_a_barrier) {
    Typist t;
    CHECK_EQ(t.key(0x3A, true), std::string("P66/0 B"));
    CHECK_EQ(t.key(0x3A, true), std::string(""));  // repeats don't toggle again
    CHECK_EQ(t.key(0x3A, false), std::string(""));
    CHECK_EQ(t.tap(0x1E), std::string("P38/2 | R38/2"));
    CHECK_EQ(t.key(0x3A, true), std::string("P66/2 B"));
    t.key(0x3A, false);
    CHECK_EQ(t.key(0x45, true), std::string("P77/0 B"));
    t.key(0x45, false);
    CHECK_EQ(t.tap(0x4F), std::string("P87/16 | R87/16"));  // keypad 1 with Num Lock

    Typist on(true, LockState{false, true});
    CHECK_EQ(on.tap(0x4F), std::string("P87/16 | R87/16"));
}

TEST(drops_the_left_ctrl_windows_sends_with_altgr) {
    Typist de(true);
    CHECK_EQ(de.key(0x1D, true), std::string(""));
    CHECK_EQ(de.key(0x138, true), std::string(""));
    CHECK_EQ(de.tap(0x10), std::string("P24/128 | R24/128"));  // AltGr+Q: @ on a German keyboard
    CHECK_EQ(de.key(0x138, false), std::string("R92/128"));
    CHECK_EQ(de.key(0x1D, false), std::string(""));
    CHECK_EQ(de.tap(0x10), std::string("P24/0 | R24/0"));

    // Without the fake Ctrl, the same.
    Typist plain(true);
    plain.key(0x138, true);
    CHECK_EQ(plain.tap(0x10), std::string("P24/128 | R24/128"));

    // A real Ctrl held before AltGr stays.
    Typist both(true);
    both.key(0x1D, true);
    both.now += 500ms;
    both.key(0x138, true);
    CHECK_EQ(both.tap(0x10), std::string("P24/132 | R24/132"));

    // Right Alt on a layout without AltGr is Alt.
    Typist us(false);
    us.key(0x138, true);
    CHECK_EQ(us.tap(0x21), std::string("P41/8 | R41/8"));
    CHECK_EQ(us.key(0x138, false), std::string("R64/8"));
}

TEST(sends_pause_as_one_key) {
    Typist t;
    CHECK_EQ(t.key(0x21D, true), std::string(""));
    CHECK_EQ(t.key(0x45, true), std::string("P127/0"));
    CHECK_EQ(t.key(0x21D, false), std::string(""));
    CHECK_EQ(t.key(0x45, false), std::string(""));
    // Num Lock afterwards is still Num Lock.
    CHECK_EQ(t.key(0x45, true), std::string("P77/0 B"));
}

TEST(repeats_held_keys_unless_casement_does) {
    Typist t;
    t.key(0x1E, true);
    CHECK(!t.kb.tick(t.now + 100ms).size());
    auto first = t.kb.next_tick();
    REQUIRE(first);
    CHECK_EQ(show(t.kb.tick(*first)), std::string("P38/0"));
    auto second = t.kb.next_tick();
    REQUIRE(second);
    CHECK(*second - *first == 40ms);
    // Only the newest key repeats.
    t.now = *second;
    t.key(0x30, true);
    CHECK_EQ(show(t.kb.tick(t.now + 650ms)), std::string("P56/0"));
    t.key(0x30, false);
    CHECK(!t.kb.next_tick());
    t.key(0x1E, false);

    // Casement's own repeats switch this off.
    Typist c;
    c.key(0x1E, true);
    CHECK_EQ(c.key(0x1E, true), std::string("P38/0"));
    CHECK(c.kb.casement_repeats());
    CHECK(!c.kb.next_tick());
    c.key(0x1E, false);
    c.key(0x30, true);
    CHECK(!c.kb.next_tick());
}

TEST(moves_clicks_and_scrolls) {
    Keyboard kb(true, {}, {});
    CHECK_EQ(show(kb.pointer(10, 20, 0)), std::string("M10,20"));
    CHECK_EQ(show(kb.pointer(10, 20, 1)), std::string("D1/0"));
    CHECK_EQ(show(kb.pointer(12, 20, 5)), std::string("M12,20 D3/0"));
    CHECK_EQ(show(kb.pointer(12, 20, 0)), std::string("U1/0 U3/0"));
    CHECK_EQ(show(kb.pointer(-5, 3, 2)), std::string("M0,3 D2/0"));
    CHECK_EQ(show(kb.wheel(2)), std::string("D4/0 U4/0 D4/0 U4/0"));
    CHECK_EQ(show(kb.wheel(-1)), std::string("D5/0 U5/0"));
    // Ctrl+click carries the mask.
    auto t0 = Clock::time_point{} + 1h;
    kb.key(0x1D, true, t0);
    CHECK_EQ(show(kb.pointer(0, 3, 3)), std::string("D1/4"));
}
