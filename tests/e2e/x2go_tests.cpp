// The plugin against a real X2Go server, through the casement-sim harness.
//
// Configured through the environment (tools/e2e/x2go-server.sh env prints
// it for its test server):
//   X2GO_E2E_HOST, X2GO_E2E_PORT, X2GO_E2E_USER, X2GO_E2E_PASSWORD
//   X2GO_E2E_KEY             a private key the server accepts
//   X2GO_E2E_KEY_ENCRYPTED   optional: one protected by
//   X2GO_E2E_KEY_PASSPHRASE
//   X2GO_E2E_KNOWN_HOSTS     known_hosts with the server's keys
//   X2GO_E2E_COMMAND         the desktop: twm and an xterm on #336699
//   X2GO_E2E_RSA_FINGERPRINT optional: the SHA256 fingerprint of the
//                            server's RSA host key
// Without X2GO_E2E_HOST the tests are skipped, unless CASEMENT_E2E=1.
// Unix paths are turned into Wine's Z: paths.

#include "check.h"

#include <windows.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "sim.h"

using namespace casement::sim;
using namespace std::chrono_literals;

namespace {

std::string env(const char* name) {
    char buf[4096];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) ? std::string(buf, n) : std::string();
}

std::string windows_path(std::string p) {
    if (p.empty() || p[0] != '/') return p;
    for (char& c : p)
        if (c == '/') c = '\\';
    return "Z:" + p;
}

std::string encode(std::string_view s) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (char c : s) {
        auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += digits[u >> 4];
            out += digits[u & 15];
        }
    }
    return out;
}

struct Config {
    std::string host, port, user, password, key, key_encrypted, passphrase, known_hosts, command,
        rsa_fingerprint;

    std::string address(std::string_view query) const {
        return "x2go://" + encode(user) + "@" + host + ":" + port + "?command=" + encode(command) + "&layout=us" +
               std::string(query);
    }
};

/// The configuration, or nothing when the tests are to be skipped.
std::optional<Config> configured() {
    Config c;
    c.host = env("X2GO_E2E_HOST");
    if (c.host.empty()) {
        if (env("CASEMENT_E2E") == "1") {
            ::check::fail(__FILE__, __LINE__, "X2GO_E2E_HOST is set", "(CASEMENT_E2E=1 needs a server)");
            throw ::check::Abort();
        }
        std::fprintf(stderr, "skipped: X2GO_E2E_HOST isn't set\n");
        return std::nullopt;
    }
    c.port = env("X2GO_E2E_PORT").empty() ? "22" : env("X2GO_E2E_PORT");
    c.user = env("X2GO_E2E_USER");
    c.password = env("X2GO_E2E_PASSWORD");
    c.key = windows_path(env("X2GO_E2E_KEY"));
    c.key_encrypted = windows_path(env("X2GO_E2E_KEY_ENCRYPTED"));
    c.passphrase = env("X2GO_E2E_KEY_PASSPHRASE");
    c.known_hosts = windows_path(env("X2GO_E2E_KNOWN_HOSTS"));
    c.command = env("X2GO_E2E_COMMAND");
    c.rsa_fingerprint = env("X2GO_E2E_RSA_FINGERPRINT");
    return c;
}

std::wstring plugin_path() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return (std::filesystem::path(self).parent_path() / PLUGIN_EXE).wstring();
}

std::string empty_known_hosts() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::filesystem::path p = std::filesystem::path(dir) / "x2go-e2e-empty-known_hosts";
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") == 0 && f) std::fclose(f);
    return p.string();
}

/// US layout: the set-1 scancode for a character, and whether it needs Shift.
std::optional<std::pair<int, bool>> us_key(char ch) {
    static const std::string lower = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./";
    static const std::string upper = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>?";
    static const int codes[] = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
                                0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
                                0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
                                0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35};
    if (ch == ' ') return std::pair{0x39, false};
    if (ch == '\n') return std::pair{0x1C, false};
    if (auto i = lower.find(ch); i != std::string::npos) return std::pair{codes[i], false};
    if (auto i = upper.find(ch); i != std::string::npos) return std::pair{codes[i], true};
    return std::nullopt;
}

class Run {
public:
    Run(const Config& cfg, const std::string& address, const std::string& password, const std::string& known_hosts) {
        c_ = Casement::spawn(plugin_path(), {}, true,
                             {{"CASEMENT_X2GO_LOG", "debug"}, {"CASEMENT_X2GO_KNOWN_HOSTS", known_hosts}});
        if (!c_ || !c_->hello()) throw std::runtime_error("the plugin didn't start");
        c_->connect(address, password, cfg.user);
    }
    ~Run() {
        if (!closed_) close();
    }

    /// Handles messages as Casement would until `done` or the timeout.
    bool pump(std::chrono::milliseconds timeout, const std::function<bool()>& done) {
        auto end = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            if (std::chrono::steady_clock::now() >= end) return false;
            auto m = c_->recv(100ms);
            if (m) handle(*m);
            else if (c_->wait(0ms)) return done();
        }
        return true;
    }

    bool wait_state(std::chrono::milliseconds timeout) {
        return pump(timeout, [&] { return state.has_value(); });
    }

    std::uint32_t pixel(std::uint32_t x, std::uint32_t y) const {
        if (!surface || x >= surface->width || y >= surface->height) return 0xFFFFFFFF;
        return surface->pixel(x, y) & 0xFFFFFF;
    }

    std::vector<std::uint32_t> region(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) const {
        std::vector<std::uint32_t> out;
        for (std::uint32_t j = y; j < y + h; ++j)
            for (std::uint32_t i = x; i < x + w; ++i) out.push_back(pixel(i, j));
        return out;
    }

    void key(int scancode, bool down) {
        c_->send(json{{"type", "key"}, {"scancode", scancode}, {"down", down}});
    }

    void type(std::string_view text) {
        for (char ch : text) {
            auto k = us_key(ch);
            if (!k) throw std::runtime_error("no key for a character");
            if (k->second) key(0x2A, true);
            key(k->first, true);
            key(k->first, false);
            if (k->second) key(0x2A, false);
        }
    }

    void pointer(int x, int y, int buttons) {
        c_->send(json{{"type", "pointer"}, {"x", x}, {"y", y}, {"buttons", buttons}});
    }

    void send(const json& m) { c_->send(m); }

    bool clipboard_seen(std::string_view text) const {
        for (const auto& c : clipboards)
            if (c.value("text", "") == text) return true;
        return false;
    }

    /// The rest of the first log line containing `prefix`, after it.
    std::string log_after(std::string_view prefix) {
        for (const auto& line : c_->stderr_lines()) {
            auto at = line.find(prefix);
            if (at != std::string::npos) return line.substr(at + prefix.size());
        }
        return {};
    }
    bool log_has(std::string_view text) {
        for (const auto& line : c_->stderr_lines())
            if (line.find(text) != std::string::npos) return true;
        return false;
    }

    /// Sends `disconnect` and returns the exit code.
    std::optional<DWORD> close() {
        closed_ = true;
        c_->send(json{{"type", "disconnect"}});
        c_->close_stdin();
        return c_->wait(5000ms);
    }
    std::optional<DWORD> exit_code(std::chrono::milliseconds timeout) { return c_->wait(timeout); }

    std::optional<std::string> state;
    std::string state_text;
    std::unique_ptr<HostSurface> surface;
    int frames = 0;
    std::vector<json> cursors;
    std::vector<json> clipboards;

private:
    void handle(const json& m) {
        const std::string type = m.value("type", "");
        if (type == "surface_request") {
            auto [s, msg] = c_->create_surface(next_surface_++, m["width"], m["height"]);
            if (!s) throw std::runtime_error("no surface");
            surface = std::move(s);
            c_->send(msg);
        } else if (type == "frame") {
            ++frames;
            c_->send(json{{"type", "frame_ack"}, {"surface", m["surface"]}});
        } else if (type == "state") {
            state = m.value("state", "");
            state_text = m.value("text", "");
        } else if (type == "cursor") {
            cursors.push_back(m);
        } else if (type == "clipboard") {
            clipboards.push_back(m);
        }
    }

    std::unique_ptr<Casement> c_;
    std::uint32_t next_surface_ = 1;
    bool closed_ = false;
};

constexpr std::uint32_t root_colour = 0x336699;
constexpr std::uint32_t white = 0xFFFFFF;

/// Waits for the test desktop: the root window's colour and the xterm.
bool desktop_shows(Run& r) {
    return r.pump(40s, [&] {
        return r.surface && r.frames > 0 && r.pixel(r.surface->width - 10, r.surface->height - 10) == root_colour &&
               r.pixel(200, 200) == white;
    });
}

}  // namespace

TEST(password_sign_in_and_desktop) {
    auto cfg = configured();
    if (!cfg) return;
    Run r(*cfg, cfg->address("&new&close=terminate"), cfg->password, cfg->known_hosts);
    REQUIRE(r.wait_state(60s));
    REQUIRE(*r.state == "connected");
    CHECK(r.log_has("signed in as " + cfg->user + " with a password"));
    REQUIRE(desktop_shows(r));
    CHECK_EQ(r.surface->width, 1024u);

    // The pointer over the xterm: its I-beam cursor.
    r.pointer(200, 200, 0);
    REQUIRE(r.pump(10s, [&] { return !r.cursors.empty(); }));
    const json& cursor = r.cursors.back();
    CHECK(cursor.value("width", 0) > 0 && cursor.value("height", 0) > 0);
    CHECK(!cursor.value("rgba", std::string()).empty());

    // Typing into the xterm changes it.
    auto before = r.region(44, 60, 480, 80);
    r.type("echo e2e-typed\n");
    CHECK(r.pump(10s, [&] { return r.region(44, 60, 480, 80) != before; }));

    // Clipboard from the server ...
    r.type("printf e2e-from-server | xsel -bi\n");
    CHECK(r.pump(15s, [&] { return r.clipboard_seen("e2e-from-server"); }));
    // ... and to it: the xterm reads it, changes it and sets it again.
    r.send(json{{"type", "clipboard"}, {"text", "e2e-from-casement"}});
    std::this_thread::sleep_for(500ms);
    r.type("xsel -bo | tr a-z A-Z | xsel -bi\n");
    CHECK(r.pump(15s, [&] { return r.clipboard_seen("E2E-FROM-CASEMENT"); }));

    // A click on the root window opens twm's menu.
    auto root = r.region(650, 450, 200, 200);
    r.pointer(700, 500, 0);
    r.pointer(700, 500, 1);
    CHECK(r.pump(10s, [&] { return r.region(650, 450, 200, 200) != root; }));
    r.pointer(700, 500, 0);

    // Resizing the view resizes the remote screen.
    r.send(json{{"type", "resize"}, {"width", 800}, {"height", 600}});
    CHECK(r.pump(15s, [&] {
        return r.surface && r.surface->width == 800 && r.surface->height == 600 &&
               r.pixel(790, 590) == root_colour;
    }));
    CHECK(r.log_has("resizing to 800x600"));

    auto code = r.close();
    REQUIRE(code);
    CHECK_EQ(*code, 0ul);
}

TEST(key_sign_in) {
    auto cfg = configured();
    if (!cfg) return;
    {
        Run r(*cfg, cfg->address("&new&close=terminate&key=" + encode(cfg->key)), "", cfg->known_hosts);
        REQUIRE(r.wait_state(60s));
        REQUIRE(*r.state == "connected");
        CHECK(r.log_has("with the key"));
        CHECK(r.pump(20s, [&] { return r.frames > 0; }));
        auto code = r.close();
        CHECK(code && *code == 0);
    }
    if (cfg->key_encrypted.empty()) return;
    // Casement's password opens a key protected by a passphrase.
    Run r(*cfg, cfg->address("&new&close=terminate&key=" + encode(cfg->key_encrypted)), cfg->passphrase,
          cfg->known_hosts);
    REQUIRE(r.wait_state(60s));
    REQUIRE(*r.state == "connected");
    CHECK(r.log_has("with the key"));
    CHECK(r.pump(20s, [&] { return r.frames > 0; }));
}

TEST(wrong_password_is_auth_failed) {
    auto cfg = configured();
    if (!cfg) return;
    Run r(*cfg, cfg->address("&new&close=terminate"), "not-the-password", cfg->known_hosts);
    REQUIRE(r.wait_state(60s));
    CHECK_EQ(*r.state, std::string("auth_failed"));
    auto code = r.exit_code(5000ms);
    CHECK(code && *code == 0);

    Run none(*cfg, cfg->address("&new&close=terminate"), "", cfg->known_hosts);
    REQUIRE(none.wait_state(60s));
    CHECK_EQ(*none.state, std::string("auth_failed"));
}

TEST(unknown_host_key_fails_and_a_pinned_key_works) {
    auto cfg = configured();
    if (!cfg) return;
    const std::string empty = empty_known_hosts();
    std::string fingerprint;
    {
        Run r(*cfg, cfg->address("&new&close=terminate"), cfg->password, empty);
        REQUIRE(r.wait_state(60s));
        CHECK_EQ(*r.state, std::string("failed"));
        auto at = r.state_text.find("hostkey=SHA256:");
        REQUIRE(at != std::string::npos);
        fingerprint = r.state_text.substr(at + 8, r.state_text.find(' ', at) - at - 8);
        CHECK_EQ(fingerprint.size(), 50u);
        auto code = r.exit_code(5000ms);
        CHECK(code && *code == 1);
    }
    {
        Run r(*cfg, cfg->address("&new&close=terminate&hostkey=" + encode(fingerprint)), cfg->password, empty);
        REQUIRE(r.wait_state(60s));
        CHECK_EQ(*r.state, std::string("connected"));
        CHECK(r.log_has("matches the pinned fingerprint"));
    }
    if (!cfg->rsa_fingerprint.empty()) {
        // A pin of another of the server's keys than the one it shows first.
        Run r(*cfg, cfg->address("&new&close=terminate&hostkey=" + encode(cfg->rsa_fingerprint)), cfg->password, empty);
        REQUIRE(r.wait_state(60s));
        CHECK_EQ(*r.state, std::string("connected"));
        CHECK(r.log_has("host key ssh-rsa " + cfg->rsa_fingerprint + " matches the pinned fingerprint"));
    }
    std::string wrong = fingerprint;
    wrong[10] = wrong[10] == 'A' ? 'B' : 'A';
    Run r(*cfg, cfg->address("&new&close=terminate&hostkey=" + encode(wrong)), cfg->password, cfg->known_hosts);
    REQUIRE(r.wait_state(60s));
    CHECK_EQ(*r.state, std::string("failed"));
    CHECK(r.state_text.find("WARNING") != std::string::npos);
}

TEST(disconnect_then_reconnect_resumes) {
    auto cfg = configured();
    if (!cfg) return;
    std::string session;
    {
        Run r(*cfg, cfg->address("&new"), cfg->password, cfg->known_hosts);
        REQUIRE(r.wait_state(60s));
        REQUIRE(*r.state == "connected");
        REQUIRE(desktop_shows(r));
        session = r.log_after("started session ");
        REQUIRE(!session.empty());
        // Something to recognise the session by.
        r.pointer(200, 200, 0);
        r.type("echo resumed-session-marker\n");
        std::this_thread::sleep_for(1s);
        auto code = r.close();  // suspends
        REQUIRE(code);
        CHECK_EQ(*code, 0ul);
        CHECK(r.log_has("suspending session " + session));
    }
    Run r(*cfg, cfg->address("&close=terminate"), cfg->password, cfg->known_hosts);
    REQUIRE(r.wait_state(60s));
    REQUIRE(*r.state == "connected");
    CHECK(r.log_has("resumed session " + session));
    CHECK(!r.log_has("started session"));
    CHECK(r.pump(20s, [&] { return r.frames > 0 && r.pixel(200, 200) == white; }));
}

TEST(close_terminate_ends_the_session) {
    auto cfg = configured();
    if (!cfg) return;
    std::string first;
    {
        Run r(*cfg, cfg->address("&close=terminate"), cfg->password, cfg->known_hosts);
        REQUIRE(r.wait_state(60s));
        REQUIRE(*r.state == "connected");
        CHECK(r.pump(20s, [&] { return r.frames > 0; }));
        first = r.log_after("started session ");
        if (first.empty()) first = r.log_after("resumed session ");
        REQUIRE(!first.empty());
        auto code = r.close();
        CHECK(code && *code == 0);
        CHECK(r.log_has("terminating session " + first));
    }
    // Nothing to resume: the session is gone, so a new one starts.
    Run r(*cfg, cfg->address("&close=terminate"), cfg->password, cfg->known_hosts);
    REQUIRE(r.wait_state(60s));
    REQUIRE(*r.state == "connected");
    CHECK(r.log_has("no suspended session"));
    CHECK(!r.log_has(first));
}

CHECK_MAIN
