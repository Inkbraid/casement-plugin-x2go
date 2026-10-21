#include "x2go/server.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <utility>

namespace x2go::server {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        auto nl = text.find('\n');
        out.push_back(text.substr(0, nl));
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
    return out;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    for (;;) {
        auto p = s.find(sep);
        out.push_back(s.substr(0, p));
        if (p == std::string_view::npos) return out;
        s.remove_prefix(p + 1);
    }
}

template <class T>
std::optional<T> number(std::string_view s) {
    s = trim(s);
    T v{};
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || end != s.data() + s.size() || s.empty()) return std::nullopt;
    return v;
}

bool digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool hex32(std::string_view s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
           });
}

/// Characters x2gostartagent keeps in session ids.
bool id_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-' || c == '@';
}

bool valid_session_id(std::string_view id) {
    return !id.empty() && id.size() < 512 && std::all_of(id.begin(), id.end(), id_char);
}

/// The layout as x2gostartagent's kbtype argument: model/layout(variant),
/// with the parentheses escaped for the shell.
std::string kbtype(std::string_view layout) {
    std::string out = "pc105/";
    for (char c : layout) {
        if (c == '(' || c == ')') out += '\\';
        out += c;
    }
    return out;
}

std::string_view base_layout(std::string_view layout) { return layout.substr(0, layout.find('(')); }

// x2goruncommand's desktop names, matched without regard to case. Names
// that are also programs (openbox, icewm) are left as typed.
constexpr std::string_view desktops[] = {"KDE",      "GNOME",   "UNITY",      "CINNAMON",   "MATE",
                                         "XFCE",     "XFCE4",   "LXDE",       "LXQt",       "TRINITY",
                                         "TERMINAL", "OFFICE",  "WWWBROWSER", "MAILCLIENT"};

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

std::string wrap(std::string_view command, std::string_view marker) {
    // As x2goclient does it, but the command's exit status is kept.
    return std::format(
        "bash -l -c 'echo \"X2GODATABEGIN:{0}\"; export TERM=\"dumb\"; {1}; x2go_status=$?; "
        "echo \"X2GODATAEND:{0}\"; exit $x2go_status'",
        marker, command);
}

std::optional<std::string> unwrap(std::string_view output, std::string_view marker) {
    const std::string begin = std::format("X2GODATABEGIN:{}\n", marker);
    const std::string end = std::format("X2GODATAEND:{}\n", marker);
    auto b = output.find(begin);
    if (b == std::string_view::npos) return std::nullopt;
    output.remove_prefix(b + begin.size());
    auto e = output.find(end);
    return std::string(output.substr(0, e));
}

std::optional<std::string> normalize_command(std::string_view command, std::string& error) {
    command = trim(command);
    if (command.empty()) {
        error = "The command is empty.";
        return std::nullopt;
    }
    if (command.size() > 256) {
        error = "The command is longer than 256 characters.";
        return std::nullopt;
    }
    for (char c : command) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == ' ' ||
                        std::string_view("._/+=:,@%-").find(c) != std::string_view::npos;
        if (!ok) {
            error = std::format(
                "The command “{}” has a character the plugin doesn't pass on (“{}”). Use letters, digits, "
                "spaces and . _ / + = : , @ % -, or put the command in a script on the server.",
                command, c);
            return std::nullopt;
        }
    }
    for (std::string_view d : desktops)
        if (iequals(command, d)) return std::string(d);
    // Runs of spaces become one: x2goruncommand's unquoted echo would
    // collapse them anyway.
    std::string out;
    bool space = false;
    for (char c : command) {
        if (c == ' ') {
            space = true;
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::string session_token(std::string_view command) {
    std::string_view word = command.substr(0, command.find(' '));
    if (auto slash = word.rfind('/'); slash != std::string_view::npos) word.remove_prefix(slash + 1);
    std::string out;
    for (char c : word) {
        if (c == ':') out += "PP";
        else if (id_char(c)) out += c;
    }
    return out.empty() ? std::string("desktop") : out;
}

std::string startagent_command(Geometry g, std::string_view layout, std::string_view command) {
    return std::format("X2GO_XINERAMA=no x2gostartagent {}x{} lan 16m-jpeg-9 unix-kde-depth_24 {} {} 1 K {} both",
                       g.width, g.height, base_layout(layout), kbtype(layout), session_token(command));
}

std::optional<Started> parse_startagent(std::string_view output) {
    std::vector<std::string_view> values;
    for (auto l : lines(output)) {
        l = trim(l);
        if (!l.empty()) values.push_back(l);
    }
    if (values.size() < 7) return std::nullopt;
    Started s;
    s.display = std::string(values[0]);
    s.cookie = std::string(values[1]);
    auto pid = number<std::uint32_t>(values[2]);
    s.session_id = std::string(values[3]);
    auto gr = number<std::uint16_t>(values[4]);
    auto snd = number<std::uint16_t>(values[5]);
    auto fs = number<std::uint16_t>(values[6]);
    if (!digits(s.display) || !hex32(s.cookie) || !pid || !valid_session_id(s.session_id) || !gr || !*gr || !snd ||
        !fs)
        return std::nullopt;
    s.agent_pid = *pid;
    s.graphics_port = *gr;
    s.sound_port = *snd;
    s.files_port = *fs;
    return s;
}

std::string explain_startagent(int code, std::string_view stderr_text) {
    std::string_view detail = trim(stderr_text);
    if (auto nl = detail.find('\n'); nl != std::string_view::npos) detail = detail.substr(0, nl);
    const bool missing = detail.find("not found") != std::string_view::npos ||
                         detail.find("No such file") != std::string_view::npos;
    if (code == 127 || (missing && detail.find("x2gostartagent") != std::string_view::npos))
        return "X2Go isn't installed on the server: x2gostartagent wasn't found.";
    if (missing && detail.find("x2gokdrive") != std::string_view::npos)
        return "X2Go's KDrive server isn't installed: install xserver-x2gokdrive on the server.";
    switch (code) {
        case 1:
        case 11: return "The X2Go server found no free port for the session.";
        case 3:
        case 4:
        case 7:
            return "The server's login environment lacks USER, HOME or SSH_CLIENT, which the X2Go server needs.";
        case 5: return "The path of the home directory has characters the X2Go server can't handle.";
        case 6: return "The server has no usable host name for X2Go (it must not be “localhost”).";
        case 9:
            return detail.empty() ? std::string("The X2Go session limit for this user has been reached.")
                                  : std::format("The X2Go session limit has been reached: {}", detail);
        case 10: return "The X2Go server couldn't register the session: no free display, or its database failed.";
        case 12:
            return "The X2Go KDrive server didn't start. Is xserver-x2gokdrive (x2gokdrive) installed on the server?";
    }
    if (detail.empty()) return std::format("x2gostartagent failed (exit code {}).", code);
    return std::format("x2gostartagent failed (exit code {}): {}", code, detail);
}

std::vector<Listed> parse_listsessions(std::string_view output) {
    std::vector<Listed> out;
    for (auto l : lines(output)) {
        l = trim(l);
        auto f = split(l, '|');
        // Perl warnings can end up on stdout; x2goclient also skips short lines.
        if (f.size() < 10) continue;
        Listed s;
        auto pid = number<std::uint32_t>(f[0]);
        auto gr = number<std::uint16_t>(f[8]);
        auto snd = number<std::uint16_t>(f[9]);
        if (!pid || !gr || !snd || f[4].size() != 1 || !valid_session_id(f[1]) || !hex32(f[6])) continue;
        s.agent_pid = *pid;
        s.session_id = std::string(f[1]);
        s.display = std::string(f[2]);
        s.host = std::string(f[3]);
        s.status = f[4][0];
        s.created = std::string(f[5]);
        s.cookie = std::string(f[6]);
        s.graphics_port = *gr;
        s.sound_port = *snd;
        out.push_back(std::move(s));
    }
    return out;
}

std::optional<SessionId> parse_session_id(std::string_view id) {
    // The user name may contain anything, "_st" included, so look for the
    // first "-<display>-<time>_st".
    for (std::size_t at = id.find("_st"); at != std::string_view::npos; at = id.find("_st", at + 1)) {
        std::string_view head = id.substr(0, at);
        auto t = head.rfind('-');
        if (t == std::string_view::npos || t == 0) continue;
        auto d = head.rfind('-', t - 1);
        if (d == std::string_view::npos) continue;
        std::string_view display = head.substr(d + 1, t - d - 1);
        std::string_view time = head.substr(t + 1);
        auto seconds = number<std::uint64_t>(time);
        if (!digits(display) || !seconds || at + 3 >= id.size()) continue;
        std::string_view rest = id.substr(at + 4);
        auto dp = rest.rfind("_dp");
        if (dp == std::string_view::npos) continue;
        SessionId s;
        s.user = std::string(head.substr(0, d));
        s.display = std::string(display);
        s.time = *seconds;
        s.type = id[at + 3];
        s.command = std::string(rest.substr(0, dp));
        return s;
    }
    return std::nullopt;
}

std::optional<Listed> pick_resumable(const std::vector<Listed>& sessions, std::string_view command) {
    const std::string token = session_token(command);
    std::optional<Listed> best;
    std::uint64_t best_time = 0;
    for (const auto& s : sessions) {
        if (s.status != 'S') continue;
        auto id = parse_session_id(s.session_id);
        if (!id || id->type != 'K' || id->command != token) continue;
        if (!best || id->time > best_time || (id->time == best_time && s.created > best->created)) {
            best = s;
            best_time = id->time;
        }
    }
    return best;
}

std::string resume_command(std::string_view session_id, Geometry g, std::string_view layout) {
    return std::format("x2goresume-session {} {}x{} lan 16m-jpeg-9 {} {} 1 both no", session_id, g.width, g.height,
                       base_layout(layout), kbtype(layout));
}

std::optional<Resumed> parse_resume(std::string_view output) {
    Resumed r;
    for (auto l : lines(output)) {
        l = trim(l);
        auto eq = l.find('=');
        if (eq == std::string_view::npos) continue;
        auto key = l.substr(0, eq);
        auto value = number<std::uint16_t>(l.substr(eq + 1));
        if (!value) continue;
        if (key == "gr_port") r.graphics_port = *value;
        else if (key == "sound_port") r.sound_port = *value;
        else if (key == "fs_port") r.files_port = *value;
    }
    if (!r.graphics_port) return std::nullopt;
    return r;
}

std::string runcommand_command(const Started& s, std::string_view command) {
    std::string cmd;
    for (char c : command) {
        if (c == ' ') cmd += "X2GO_SPACE_CHAR";
        else cmd += c;
    }
    // In the background of a subshell with every stream closed, so the
    // channel ends now while the desktop runs on; "D" is what x2goclient
    // passes for desktop sessions, KDrive included.
    return std::format("(setsid x2goruncommand {} {} {} {} {} nosnd D </dev/null >/dev/null 2>&1 &)", s.display,
                       s.agent_pid, s.session_id, s.sound_port, cmd);
}

std::string suspend_command(std::string_view session_id) {
    return std::format("x2gosuspend-session {}", session_id);
}

std::string terminate_command(std::string_view session_id) {
    return std::format("x2goterminate-session {}", session_id);
}

std::string state_command(std::string_view session_id) {
    return std::format(
        "cat \"/tmp/.x2go-${{USER}}/C-{0}/state\" 2>/dev/null; echo; tail -n 5 \"${{HOME}}/.x2go/C-{0}/cmdoutput\" "
        "2>/dev/null",
        session_id);
}

State parse_state(std::string_view output) {
    State s;
    auto ls = lines(output);
    if (!ls.empty()) s.agent = std::string(trim(ls[0]));
    constexpr std::string_view noexec = "X2GORUNCOMMAND ERR NOEXEC:";
    for (std::size_t i = 1; i < ls.size(); ++i) {
        auto l = trim(ls[i]);
        if (l.starts_with(noexec)) s.not_found = std::string(l.substr(noexec.size()));
    }
    return s;
}

}  // namespace x2go::server
