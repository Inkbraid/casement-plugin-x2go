#include "x2go/keyboard.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>

namespace x2go::keys {

namespace {

using kdrive::mask_control;
using kdrive::mask_lock;
using kdrive::mask_mod1;
using kdrive::mask_mod2;
using kdrive::mask_mod4;
using kdrive::mask_mod5;
using kdrive::mask_shift;

// Set-1 scancodes Casement sends.
constexpr std::uint32_t sc_lctrl = 0x1D;
constexpr std::uint32_t sc_lshift = 0x2A;
constexpr std::uint32_t sc_rshift = 0x36;
constexpr std::uint32_t sc_lalt = 0x38;
constexpr std::uint32_t sc_caps = 0x3A;
constexpr std::uint32_t sc_numlock = 0x45;
constexpr std::uint32_t sc_rctrl = 0x11D;
constexpr std::uint32_t sc_ralt = 0x138;
constexpr std::uint32_t sc_lwin = 0x15B;
constexpr std::uint32_t sc_rwin = 0x15C;
// Pause arrives as this, then 0x45.
constexpr std::uint32_t sc_pause_prefix = 0x21D;

constexpr std::uint8_t kc_caps = 66;
constexpr std::uint8_t kc_numlock = 77;
constexpr std::uint8_t kc_pause = 127;

// The keys the server presses for a modifier bit: the lowest keycode in
// its modifier map (Shift_L, Control_L, Alt_L, Super_L, ISO_Level3_Shift).
std::uint8_t server_key_for(std::uint32_t bit) {
    switch (bit) {
        case mask_shift: return 50;
        case mask_control: return 37;
        case mask_mod1: return 64;
        case mask_mod4: return 133;
        case mask_mod5: return 92;
    }
    return 0;
}

// E0 keys: Linux keycode + 8.
struct E0Key {
    std::uint8_t code;
    std::uint8_t keycode;
};
constexpr E0Key e0_keys[] = {
    {0x10, 173},  // previous track
    {0x19, 171},  // next track
    {0x1C, 104},  // keypad Enter
    {0x1D, 105},  // right Ctrl
    {0x20, 121},  // mute
    {0x21, 148},  // calculator
    {0x22, 172},  // play/pause
    {0x24, 174},  // stop
    {0x2E, 122},  // volume down
    {0x30, 123},  // volume up
    {0x32, 180},  // browser home
    {0x35, 106},  // keypad /
    {0x37, 107},  // Print Screen
    {0x38, 108},  // right Alt
    {0x46, 127},  // Break (Ctrl+Pause)
    {0x47, 110},  // Home
    {0x48, 111},  // Up
    {0x49, 112},  // Page Up
    {0x4B, 113},  // Left
    {0x4D, 114},  // Right
    {0x4F, 115},  // End
    {0x50, 116},  // Down
    {0x51, 117},  // Page Down
    {0x52, 118},  // Insert
    {0x53, 119},  // Delete
    {0x5B, 133},  // left Windows
    {0x5C, 134},  // right Windows
    {0x5D, 135},  // Menu
    {0x5E, 124},  // power
    {0x5F, 150},  // sleep
    {0x63, 151},  // wake
    {0x65, 225},  // browser search
    {0x66, 164},  // favourites
    {0x67, 181},  // browser refresh
    {0x68, 136},  // browser stop
    {0x69, 167},  // browser forward
    {0x6A, 166},  // browser back
    {0x6B, 165},  // my computer
    {0x6C, 163},  // mail
    {0x6D, 234},  // media select
};

// XKB layouts (xkb-data 2.41) whose right Alt is Alt_R, not AltGr, in the
// default variant, and the exceptions among their variants and those of
// the others. Read from the compiled keymaps of every layout and variant.
constexpr std::string_view no_altgr_layouts[] = {"am", "au", "bg", "brai", "by", "cm", "cn", "et",
                                                 "id", "jp", "kg", "kr", "kz", "la", "mk", "mm",
                                                 "mv", "np", "nz", "ru", "th", "tz", "us", "uz"};
constexpr std::string_view altgr_variants[] = {
    "by(intl)", "by(latin)", "by(phonetic)", "by(ru)", "cm(azerty)", "cm(dvorak)", "cm(french)", "cm(mmuock)",
    "cm(qwerty)", "cn(altgr-pinyin)", "id(melayu-phonetic)", "id(melayu-phoneticx)", "id(pegon-phonetic)",
    "kz(ext)", "la(stea)", "mm(zawgyi)", "mm(zgt)", "nz(mao)", "ru(ab)", "ru(bak)", "ru(chm)", "ru(cv)",
    "ru(cv_latin)", "ru(kom)", "ru(mac)", "ru(phonetic_azerty)", "ru(phonetic_dvorak)", "ru(sah)", "ru(srp)",
    "ru(tt)", "ru(udm)", "ru(xal)", "us(alt-intl)", "us(altgr-intl)", "us(colemak)", "us(colemak_dh)",
    "us(colemak_dh_iso)", "us(colemak_dh_ortho)", "us(colemak_dh_wide)", "us(colemak_dh_wide_iso)",
    "us(dvorak-alt-intl)", "us(dvorak-intl)", "us(dvorak-mac)", "us(euro)", "us(haw)", "us(hbs)", "us(intl)",
    "us(mac)", "us(norman)", "us(olpc2)", "us(rus)", "us(symbolic)", "us(workman-intl)"};
constexpr std::string_view no_altgr_variants[] = {
    "af(fa-olpc)", "af(ps-olpc)", "af(uz-olpc)", "ara(buckwalter)", "ca(eng)", "ca(ike)", "cz(ucw)",
    "fr(dvorak)", "ge(os)", "ge(ru)", "gr(nodeadkeys)", "gr(simple)", "ie(ogam)", "il(biblical)", "il(lyx)",
    "in(ben_gitanjali)", "in(ben_inscript)", "in(iipa)", "in(marathi)", "in(mni)", "in(tam)",
    "in(tam_tamilnumbers)", "in(tamilnet)", "in(tamilnet_TAB)", "in(tamilnet_TSCII)",
    "in(tamilnet_tamilnumbers)", "kr(kr104)", "lk(tam_TAB)", "lk(tam_unicode)", "no(mac)",
    "no(mac_nodeadkeys)", "pk(ara)", "sy(syc)", "sy(syc_phonetic)", "ua(homophonic)", "ua(legacy)",
    "ua(phonetic)", "ua(typewriter)", "ua(winkeys)"};

// Windows keyboard layouts (KLIDs) and their XKB equivalents.
struct Klid {
    std::string_view klid;
    std::string_view layout;
};
constexpr Klid klids[] = {
    {"00000401", "ara"},          {"00000402", "bg"},           {"00000404", "us"},
    {"00000405", "cz"},           {"00000406", "dk"},           {"00000407", "de"},
    {"00000408", "gr"},           {"00000409", "us"},           {"0000040A", "es"},
    {"0000040B", "fi"},           {"0000040C", "fr"},           {"0000040D", "il"},
    {"0000040E", "hu"},           {"0000040F", "is"},           {"00000410", "it"},
    {"00000411", "jp"},           {"00000412", "kr"},           {"00000413", "nl"},
    {"00000414", "no"},           {"00000415", "pl"},           {"00000416", "br"},
    {"00000418", "ro(winkeys)"},  {"00000419", "ru"},           {"0000041A", "hr"},
    {"0000041B", "sk"},           {"0000041C", "al"},           {"0000041D", "se"},
    {"0000041E", "th"},           {"0000041F", "tr"},           {"00000422", "ua"},
    {"00000423", "by"},           {"00000424", "si"},           {"00000425", "ee"},
    {"00000426", "lv"},           {"00000427", "lt(ibm)"},      {"00000429", "ir"},
    {"0000042A", "vn"},           {"0000042B", "am"},           {"0000042C", "az"},
    {"0000042F", "mk"},           {"00000437", "ge"},           {"0000043F", "kz"},
    {"00000452", "gb(extd)"},     {"00000461", "np"},           {"0000046E", "ch(fr)"},
    {"00000804", "us"},           {"00000807", "ch"},           {"00000809", "gb"},
    {"0000080A", "latam"},        {"0000080C", "be"},           {"00000813", "be"},
    {"00000816", "pt"},           {"0000081A", "rs(latin)"},    {"0000083B", "se"},
    {"00000C0C", "ca(fr-legacy)"}, {"00000C1A", "rs"},          {"0000100C", "ch(fr)"},
    {"00001009", "ca"},           {"00001809", "ie"},           {"00010402", "us"},
    {"00010405", "cz(qwerty)"},   {"00010407", "de"},           {"00010409", "us(dvorak)"},
    {"0001040A", "es"},           {"0001040C", "fr(afnor)"},    {"0001040E", "hu"},
    {"00010410", "it"},           {"00010415", "pl(qwertz)"},   {"00010416", "br"},
    {"00010418", "ro(std)"},      {"00010419", "ru(typewriter)"}, {"0001041B", "sk(qwerty)"},
    {"0001041F", "tr(f)"},        {"00010426", "lv"},           {"00010427", "lt"},
    {"0001080C", "be"},           {"0001083B", "fi"},           {"00011009", "ca(multix)"},
    {"00020405", "cz(qwerty)"},   {"00020409", "us(intl)"},     {"0002040C", "fr(bepo_afnor)"},
    {"00020418", "ro"},           {"00020422", "ua"},           {"00030402", "bg"},
    {"00030409", "us"},           {"00040402", "bg(phonetic)"}, {"00040409", "us(dvorak-l)"},
    {"00050409", "us(dvorak-r)"},
};

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

template <std::size_t N>
bool listed(const std::string_view (&list)[N], std::string_view s) {
    return std::find(std::begin(list), std::end(list), s) != std::end(list);
}

}  // namespace

std::uint8_t x_keycode(std::uint32_t sc) {
    if (sc >= 0x100 && sc < 0x200) {
        for (const auto& k : e0_keys)
            if (k.code == sc - 0x100) return k.keycode;
        return 0;
    }
    switch (sc) {
        case 0x54: return 107;  // SysRq (Alt+Print)
        case 0x55: return 0;
        case 0x59: return 125;  // keypad =
        case 0x70: return 101;  // Katakana/Hiragana
        case 0x73: return 97;   // Ro
        case 0x76: return 202;  // F24
        case 0x79: return 100;  // Henkan
        case 0x7B: return 102;  // Muhenkan
        case 0x7D: return 132;  // Yen
        case 0x7E: return 129;  // keypad comma
    }
    if (sc >= 0x01 && sc <= 0x58) return static_cast<std::uint8_t>(sc + 8);
    if (sc >= 0x64 && sc <= 0x6E) return static_cast<std::uint8_t>(sc - 0x64 + 191);  // F13-F23
    return 0;
}

bool valid_layout(std::string_view s) {
    auto name_char = [](char c, bool variant) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               (variant && c >= 'A' && c <= 'Z');
    };
    std::size_t open = s.find('(');
    std::string_view layout = s.substr(0, open);
    if (layout.empty() || layout.size() > 32) return false;
    for (char c : layout)
        if (!name_char(c, false)) return false;
    if (open == std::string_view::npos) return true;
    if (s.back() != ')' || s.size() - open < 3) return false;
    std::string_view variant = s.substr(open + 1, s.size() - open - 2);
    if (variant.size() > 64) return false;
    for (char c : variant)
        if (!name_char(c, true)) return false;
    return true;
}

bool ralt_is_altgr(std::string_view layout) {
    if (listed(altgr_variants, layout)) return true;
    if (listed(no_altgr_variants, layout)) return false;
    std::string_view base = layout.substr(0, layout.find('('));
    // Unknown layouts get AltGr: where right Alt is plain Alt, AltGr still
    // types normally; the other way round, AltGr characters would be lost.
    return !listed(no_altgr_layouts, base);
}

std::string layout_for_klid(std::string_view klid) {
    std::string id = upper(klid);
    for (const auto& k : klids)
        if (k.klid == id) return std::string(k.layout);
    // Input method editors (E0010411 and so on) and unlisted variants of a
    // language: the language's default layout.
    if (id.size() == 8) {
        std::string base = "0000" + id.substr(4);
        for (const auto& k : klids)
            if (k.klid == base) return std::string(k.layout);
    }
    return {};
}

std::string windows_layout() {
    wchar_t name[KL_NAMELENGTH] = {};
    if (GetKeyboardLayoutNameW(name)) {
        std::string klid;
        for (const wchar_t* p = name; *p; ++p) klid += static_cast<char>(*p & 0x7F);
        std::string layout = layout_for_klid(klid);
        if (!layout.empty()) return layout;
    }
    return "us";
}

LockState windows_lock_state() {
    return {(GetKeyState(VK_CAPITAL) & 1) != 0, (GetKeyState(VK_NUMLOCK) & 1) != 0};
}

RepeatTiming windows_repeat_timing() {
    RepeatTiming t;
    int delay = 1;     // 0-3: 250 ms to 1 s
    DWORD speed = 31;  // 0-31: about 2.5 to 30 repeats a second
    if (!SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0)) delay = 1;
    if (!SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0)) speed = 31;
    delay = std::clamp(delay, 0, 3);
    speed = std::min<DWORD>(speed, 31);
    t.delay = std::chrono::milliseconds(250 * (delay + 1) + 150);
    const double rate = 2.5 + (30.0 - 2.5) * static_cast<double>(speed) / 31.0;
    t.interval = std::chrono::milliseconds(static_cast<int>(1000.0 / rate));
    return t;
}

Keyboard::Keyboard(bool ralt_altgr, LockState locks, RepeatTiming timing)
    : ralt_altgr_(ralt_altgr), timing_(timing), caps_(locks.caps), num_(locks.num) {}

std::uint32_t Keyboard::bit(int mod) const {
    switch (mod) {
        case lshift:
        case rshift: return mask_shift;
        case lctrl:
        case rctrl: return mask_control;
        case lalt: return mask_mod1;
        case ralt: return ralt_altgr_ ? mask_mod5 : mask_mod1;
        case lwin:
        case rwin: return mask_mod4;
    }
    return 0;
}

std::uint32_t Keyboard::mask() const {
    std::uint32_t m = 0;
    if (caps_) m |= mask_lock;
    if (num_) m |= mask_mod2;
    for (int i = 0; i < mod_count; ++i)
        if (held_[static_cast<std::size_t>(i)]) m |= bit(i);
    return m;
}

void Keyboard::commit_lctrl() {
    if (!pending_lctrl_) return;
    pending_lctrl_ = false;
    held_[lctrl] = true;
}

void Keyboard::barrier(std::vector<Action>& out) {
    Action a;
    a.kind = Action::Kind::barrier;
    out.push_back(a);
    cleared_ = 0;
}

void Keyboard::send(std::vector<Action>& out, bool press, std::uint8_t keycode, std::uint32_t m) {
    // Setting a modifier again that the server released within the current
    // batch would be missed: wait until it has read everything before.
    if (m & cleared_ & ~sent_mask_) barrier(out);
    out.push_back({Action::Kind::event, kdrive::key_event(press, m, keycode)});
    sent_mask_ = m;
}

void Keyboard::send_button(std::vector<Action>& out, bool press, std::uint32_t button) {
    const std::uint32_t m = mask();
    if (m & cleared_ & ~sent_mask_) barrier(out);
    out.push_back({Action::Kind::event, kdrive::button_event(press, m, button)});
    sent_mask_ = m;
}

std::vector<Action> Keyboard::key(std::uint32_t sc, bool down, Clock::time_point now) {
    std::vector<Action> out;

    // Pause: 0x21D, then 0x45 (which alone is Num Lock).
    if (sc == sc_pause_prefix) {
        if (down) pause_prefix_ = true;
        return out;
    }
    if (sc == sc_numlock && pause_prefix_ && down) {
        pause_prefix_ = false;
        pause_up_ = true;
        commit_lctrl();
        tap_ = -1;
        send(out, true, kc_pause, mask());
        return out;
    }
    if (sc == sc_numlock && !down && pause_up_) {
        pause_up_ = false;
        return out;
    }
    pause_prefix_ = false;

    int mod = -1;
    switch (sc) {
        case sc_lshift: mod = lshift; break;
        case sc_rshift: mod = rshift; break;
        case sc_lctrl: mod = lctrl; break;
        case sc_rctrl: mod = rctrl; break;
        case sc_lalt: mod = lalt; break;
        case sc_ralt: mod = ralt; break;
        case sc_lwin: mod = lwin; break;
        case sc_rwin: mod = rwin; break;
    }
    if (mod >= 0) {
        const auto m = static_cast<std::size_t>(mod);
        if (down) {
            if (mod == lctrl) {
                // Windows puts a left Ctrl in front of AltGr. Keep it out of
                // the mask until the next event shows whether AltGr follows.
                if (held_[lctrl] || pending_lctrl_) return out;
                pending_lctrl_ = true;
                pending_lctrl_at_ = now;
                tap_ = lctrl;
                return out;
            }
            if (mod == ralt && pending_lctrl_ && now - pending_lctrl_at_ <= std::chrono::milliseconds(50)) {
                pending_lctrl_ = false;
                fake_lctrl_ = true;
            } else {
                commit_lctrl();
            }
            if (held_[m]) return out;
            held_[m] = true;
            tap_ = mod;
            return out;
        }
        if (mod == lctrl) {
            if (fake_lctrl_ && !held_[lctrl] && !pending_lctrl_) {
                fake_lctrl_ = false;
                return out;
            }
            commit_lctrl();
        }
        if (!held_[m]) return out;
        held_[m] = false;
        const std::uint32_t b = bit(mod);
        const std::uint32_t after = mask();
        if (tap_ == mod) {
            tap_ = -1;
            if (!(sent_mask_ & b)) {
                // Pressed and released with nothing in between: a tap, for
                // what the desktop does with a lone Super or Alt.
                static constexpr std::uint8_t own[mod_count] = {50, 62, 37, 105, 64, 108, 133, 134};
                send(out, true, own[m], after);
                return out;
            }
        }
        tap_ = -1;
        if (!(after & b) && (sent_mask_ & b)) {
            // Release the server's key for this bit explicitly, with the
            // mask it already has, so the release is ordered with what
            // follows even inside one batch.
            out.push_back({Action::Kind::event, kdrive::key_event(false, sent_mask_, server_key_for(b))});
            sent_mask_ &= ~b;
            cleared_ |= b;
        }
        return out;
    }

    tap_ = -1;
    commit_lctrl();

    if (sc == sc_caps || sc == sc_numlock) {
        if (!down) {
            locks_down_.erase(sc);
            return out;
        }
        if (!locks_down_.insert(sc).second) return out;  // repeats don't toggle
        const bool caps = sc == sc_caps;
        send(out, true, caps ? kc_caps : kc_numlock, mask());
        barrier(out);
        bool& state = caps ? caps_ : num_;
        state = !state;
        const std::uint32_t lock = caps ? mask_lock : mask_mod2;
        sent_mask_ = state ? (sent_mask_ | lock) : (sent_mask_ & ~lock);
        return out;
    }

    const std::uint8_t keycode = x_keycode(sc);
    if (!keycode) return out;
    if (down) {
        const bool again = !down_.insert(sc).second;
        if (again) {
            casement_repeats_ = true;
            repeat_.reset();
        }
        send(out, true, keycode, mask());
        if (!again && !casement_repeats_) repeat_ = Repeat{sc, keycode, now, now + timing_.delay};
    } else {
        down_.erase(sc);
        if (repeat_ && repeat_->scancode == sc) repeat_.reset();
        send(out, false, keycode, mask());
    }
    return out;
}

std::vector<Action> Keyboard::pointer(std::int32_t x, std::int32_t y, std::uint32_t buttons) {
    std::vector<Action> out;
    const std::int64_t px = std::max(x, 0), py = std::max(y, 0);
    if (px != pointer_x_ || py != pointer_y_) {
        out.push_back({Action::Kind::event,
                       kdrive::motion_event(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py))});
        pointer_x_ = px;
        pointer_y_ = py;
    }
    // Casement: 1 left, 2 middle, 4 right; X: buttons 1, 2, 3.
    static constexpr std::uint32_t bits[3] = {1, 2, 4};
    const std::uint32_t changed = (buttons ^ buttons_) & 7;
    for (std::uint32_t i = 0; i < 3; ++i) {
        if (!(changed & bits[i])) continue;
        commit_lctrl();
        tap_ = -1;
        send_button(out, (buttons & bits[i]) != 0, i + 1);
    }
    buttons_ = buttons & 7;
    return out;
}

std::vector<Action> Keyboard::wheel(std::int32_t delta) {
    std::vector<Action> out;
    if (delta == 0) return out;
    commit_lctrl();
    tap_ = -1;
    // X buttons 4 and 5: up (away from the user) and down.
    const std::uint32_t button = delta > 0 ? 4 : 5;
    const int notches = std::min(std::abs(std::clamp(delta, -50, 50)), 50);
    for (int i = 0; i < notches; ++i) {
        send_button(out, true, button);
        send_button(out, false, button);
    }
    return out;
}

std::vector<Action> Keyboard::tick(Clock::time_point now) {
    std::vector<Action> out;
    if (!repeat_ || casement_repeats_ || now < repeat_->next) return out;
    // A key whose release never arrives must not repeat for ever.
    if (now - repeat_->started > std::chrono::seconds(30)) {
        repeat_.reset();
        return out;
    }
    send(out, true, repeat_->keycode, mask());
    repeat_->next += timing_.interval;
    if (repeat_->next < now) repeat_->next = now + timing_.interval;
    return out;
}

std::optional<Keyboard::Clock::time_point> Keyboard::next_tick() const {
    if (!repeat_ || casement_repeats_) return std::nullopt;
    return repeat_->next;
}

}  // namespace x2go::keys
