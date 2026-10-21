#include "x2go/session.h"

#include <winsock2.h>
#include <windows.h>

#include <lmcons.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "casement/log.h"
#include "casement/text.h"
#include "x2go/clipboard.h"
#include "x2go/cursor.h"
#include "x2go/frames.h"
#include "x2go/image.h"
#include "x2go/kdrive.h"
#include "x2go/keyboard.h"
#include "x2go/known_hosts.h"
#include "x2go/options.h"
#include "x2go/server.h"
#include "x2go/ssh.h"

namespace x2go {

namespace {

namespace log = casement::log;
using casement::json;
using casement::State;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint32_t default_width = 1024;
constexpr std::uint32_t default_height = 768;
/// The reference client waits this long for a window to stop changing
/// size; every GEOMETRY makes the server send the whole screen.
constexpr auto resize_delay = 400ms;
constexpr auto keepalive_interval = 5s;
/// The server sends SRVKEEPALIVE after 10 s of quiet; three missed ones
/// mean the connection is gone.
constexpr auto server_silence = 45s;
constexpr auto barrier_timeout = 1s;

/// A test-only override: read this known_hosts file instead of the
/// user's and the system's.
constexpr wchar_t known_hosts_variable[] = L"CASEMENT_X2GO_KNOWN_HOSTS";

std::pair<std::uint32_t, std::uint32_t> clamp_size(std::int64_t w, std::int64_t h) {
    return {static_cast<std::uint32_t>(std::clamp<std::int64_t>(w, 320, 8192)),
            static_cast<std::uint32_t>(std::clamp<std::int64_t>(h, 200, 8192))};
}

std::wstring environment(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring value(n, L'\0');
    n = GetEnvironmentVariableW(name, value.data(), n);
    value.resize(n);
    return value;
}

std::wstring expand_path(const std::string& path) {
    std::wstring w = casement::widen(path);
    if (w.starts_with(L"~\\") || w.starts_with(L"~/") || w == L"~") w = environment(L"USERPROFILE") + w.substr(1);
    DWORD n = ExpandEnvironmentStringsW(w.c_str(), nullptr, 0);
    if (n == 0) return w;
    std::wstring out(n, L'\0');
    n = ExpandEnvironmentStringsW(w.c_str(), out.data(), n);
    out.resize(n > 0 ? n - 1 : 0);
    return out;
}

std::optional<std::string> read_file(const std::wstring& path, std::size_t limit = std::size_t{4} << 20) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return std::nullopt;
    std::string data;
    char buf[8192];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        data.append(buf, n);
        if (data.size() > limit) {
            std::fclose(f);
            return std::nullopt;
        }
    }
    std::fclose(f);
    return data;
}

std::string windows_user() {
    wchar_t name[UNLEN + 1];
    DWORD n = UNLEN + 1;
    if (!GetUserNameW(name, &n) || n == 0) return {};
    return casement::narrow(std::wstring_view(name, n - 1));
}

std::string random_marker() {
    std::random_device rd;
    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:08x}{:04x}", rd(), rd() & 0xFFFF, rd() & 0xFFFF, rd() & 0xFFFF,
                       rd(), rd() & 0xFFFF);
}

std::string first_line(std::string_view s) {
    while (!s.empty() && (s.front() == '\n' || s.front() == '\r' || s.front() == ' ')) s.remove_prefix(1);
    s = s.substr(0, s.find('\n'));
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.remove_suffix(1);
    return std::string(s);
}

struct KnownHostsFile {
    std::wstring path;
    std::string content;
};

std::vector<KnownHostsFile> known_hosts_files() {
    std::vector<std::wstring> paths;
    std::wstring override_path = environment(known_hosts_variable);
    if (!override_path.empty()) {
        paths.push_back(override_path);
    } else {
        std::wstring home = environment(L"USERPROFILE");
        if (!home.empty()) {
            paths.push_back(home + L"\\.ssh\\known_hosts");
            paths.push_back(home + L"\\.ssh\\known_hosts2");
        }
        std::wstring data = environment(L"ProgramData");
        if (!data.empty()) paths.push_back(data + L"\\ssh\\ssh_known_hosts");
    }
    std::vector<KnownHostsFile> files;
    for (auto& p : paths)
        if (auto c = read_file(p, std::size_t{64} << 20)) files.push_back({p, std::move(*c)});
    return files;
}

template <class... F>
struct overloaded : F... {
    using F::operator()...;
};
template <class... F>
overloaded(F...) -> overloaded<F...>;

class X2GoSession final : public casement::Session {
public:
    X2GoSession(casement::Host& host, Options options, std::string password)
        : host_(host),
          options_(std::move(options)),
          password_(std::move(password)),
          layout_(options_.layout.empty() ? keys::windows_layout() : options_.layout),
          keyboard_(keys::ralt_is_altgr(layout_), keys::windows_lock_state(), keys::windows_repeat_timing()) {
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        worker_ = std::thread([this] {
            try {
                run();
            } catch (const std::exception& e) {
                finish(State::failed, std::format("Internal error: {}", e.what()));
            }
            SetEvent(done_);
        });
    }

    ~X2GoSession() override {
        // disconnect() has run and the worker is done, or the process is
        // ending anyway.
        if (worker_.joinable()) {
            if (WaitForSingleObject(done_, 0) == WAIT_OBJECT_0) worker_.join();
            else worker_.detach();
        }
    }

    void input(const json& msg) override;
    void disconnect() override;

private:
    struct Target {
        std::string session_id;
        std::string cookie;
        std::uint16_t port = 0;
        /// Started now (the desktop still has to be started) or resumed.
        bool fresh = false;
        server::Started started;
    };

    void run();
    [[noreturn]] void finish(State state, const std::string& text);
    bool stopping() {
        std::lock_guard g(lock_);
        return close_requested_;
    }

    std::unique_ptr<ssh::Connection> connect_ssh();
    std::unique_ptr<ssh::Connection> verify_host_key(std::unique_ptr<ssh::Connection> c);
    void sign_in(ssh::Connection& c);
    Target attach(ssh::Connection& c);
    std::optional<ssh::ExecResult> run_command(ssh::Connection& c, std::string_view command,
                                               Clock::duration timeout, std::string& error);
    void stream(ssh::Connection& c, LIBSSH2_CHANNEL* ch, const Target& t);
    void close_session(ssh::Connection& c, LIBSSH2_CHANNEL* ch, const Target& t, bool connected);
    [[noreturn]] void ended(ssh::Connection& c, const Target& t, bool srv_disconnect);
    std::pair<std::uint32_t, std::uint32_t> wanted_size();

    casement::Host& host_;
    Options options_;
    std::string password_;
    std::string layout_;
    std::thread worker_;
    HANDLE wake_ = nullptr;
    HANDLE done_ = nullptr;
    std::atomic<bool> cancel_{false};

    std::mutex lock_;  // the members below, between input() and the worker
    bool connected_ = false;
    bool close_requested_ = false;
    std::vector<keys::Action> actions_;
    std::optional<std::string> clipboard_out_;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> wanted_;
    Clock::time_point resize_at_{};

    std::mutex keys_lock_;  // taken before lock_ where both are needed
    keys::Keyboard keyboard_;
};

void X2GoSession::finish(State state, const std::string& text) { host_.finish(state, text); }

std::pair<std::uint32_t, std::uint32_t> X2GoSession::wanted_size() {
    std::lock_guard g(lock_);
    return wanted_.value_or(std::pair{default_width, default_height});
}

void X2GoSession::input(const json& msg) {
    const std::string type = casement::get_string(msg, "type");
    if (type == "key" || type == "pointer" || type == "wheel") {
        std::lock_guard k(keys_lock_);
        {
            std::lock_guard g(lock_);
            if (!connected_) return;
        }
        std::vector<keys::Action> acts;
        if (type == "key") {
            acts = keyboard_.key(static_cast<std::uint32_t>(casement::get_int(msg, "scancode")),
                                 casement::get_bool(msg, "down"), Clock::now());
        } else if (type == "pointer") {
            acts = keyboard_.pointer(static_cast<std::int32_t>(casement::get_int(msg, "x")),
                                     static_cast<std::int32_t>(casement::get_int(msg, "y")),
                                     static_cast<std::uint32_t>(casement::get_int(msg, "buttons")));
        } else {
            acts = keyboard_.wheel(static_cast<std::int32_t>(std::clamp<std::int64_t>(casement::get_int(msg, "delta"), -100, 100)));
        }
        if (acts.empty()) return;
        std::lock_guard g(lock_);
        actions_.insert(actions_.end(), acts.begin(), acts.end());
    } else if (type == "resize") {
        const std::int64_t w = casement::get_int(msg, "width"), h = casement::get_int(msg, "height");
        if (w <= 0 || h <= 0) return;
        std::lock_guard g(lock_);
        wanted_ = clamp_size(w, h);
        resize_at_ = Clock::now() + resize_delay;
    } else if (type == "clipboard") {
        std::string text = casement::get_string(msg, "text");
        if (text.size() > max_clipboard) return;
        std::lock_guard g(lock_);
        clipboard_out_ = std::move(text);
    } else {
        return;
    }
    SetEvent(wake_);
}

void X2GoSession::disconnect() {
    {
        std::lock_guard g(lock_);
        close_requested_ = true;
    }
    cancel_ = true;
    SetEvent(wake_);
    // Casement ends the process two seconds after `disconnect`; the session
    // gets suspended or terminated in that time if it can.
    if (WaitForSingleObject(done_, 1800) != WAIT_OBJECT_0) {
        log::warn("closing took too long; leaving");
        ExitProcess(0);
    }
}

void X2GoSession::run() {
    std::string error;
    if (!ssh::init(error)) finish(State::failed, error);
    auto conn = connect_ssh();
    if (!conn) return;  // cancelled
    conn = verify_host_key(std::move(conn));
    if (!conn || stopping()) return;
    sign_in(*conn);
    if (stopping()) return;
    conn->go_nonblocking();
    conn->configure_keepalive(30);
    // From here on, waits end at their deadlines rather than at a close
    // request: a session being started still has to be dealt with.
    conn->set_cancel(nullptr);

    Target t = attach(*conn);
    if (stopping()) {
        close_session(*conn, nullptr, t, false);
        return;
    }
    // The agent opens its port right away; a few retries cover a slow one.
    LIBSSH2_CHANNEL* ch = nullptr;
    const auto deadline = Clock::now() + 5s;
    for (;;) {
        ch = conn->open_tunnel("localhost", t.port, deadline, error);
        if (ch || Clock::now() >= deadline || stopping()) break;
        log::debug("graphics channel to port {} not open yet: {}", t.port, error);
        std::this_thread::sleep_for(250ms);
    }
    if (!ch) {
        if (stopping()) {
            close_session(*conn, nullptr, t, false);
            return;
        }
        finish(State::failed,
               std::format("The X2Go session's graphics port {} on the server couldn't be reached: {}", t.port, error));
    }
    stream(*conn, ch, t);
}

std::unique_ptr<ssh::Connection> X2GoSession::connect_ssh() {
    std::vector<std::string> types;
    if (options_.hostkey.empty())
        for (const auto& f : known_hosts_files())
            for (auto& type : hostkeys::known_types(f.content, options_.host, options_.port))
                if (std::find(types.begin(), types.end(), type) == types.end()) types.push_back(type);
    std::string error;
    log::info("connecting to {} port {}", options_.host, options_.port);
    auto c = ssh::Connection::open(options_.host, options_.port, types, false, &cancel_, error);
    if (!c) {
        if (stopping()) return nullptr;
        finish(State::failed, error);
    }
    return c;
}

std::unique_ptr<ssh::Connection> X2GoSession::verify_host_key(std::unique_ptr<ssh::Connection> c) {
    const auto blob = c->host_key();
    if (blob.empty()) finish(State::failed, "The server sent no host key.");
    const std::string fp = hostkeys::sha256_fingerprint(blob);
    const std::string type = hostkeys::blob_type(blob);
    if (!options_.hostkey.empty()) {
        if (fp == options_.hostkey) {
            log::info("host key {} {} matches the pinned fingerprint", type, fp);
            return c;
        }
        // The pin may be of another of the server's keys: look at each.
        for (const auto& other : ssh::Connection::host_key_types()) {
            if (other == type || stopping()) continue;
            std::string error;
            auto d = ssh::Connection::open(options_.host, options_.port, {other}, true, &cancel_, error);
            if (!d) continue;
            const auto key = d->host_key();
            if (hostkeys::sha256_fingerprint(key) == options_.hostkey) {
                log::info("host key {} {} matches the pinned fingerprint", other, options_.hostkey);
                return d;
            }
        }
        if (stopping()) return nullptr;
        finish(State::failed,
               std::format("WARNING: none of the server's host keys has the pinned fingerprint {} (its {} key is "
                           "{}). Someone may be intercepting the connection, or the server got new keys. Don't "
                           "connect until you know which.",
                           options_.hostkey, type, fp));
    }
    const std::string name = hostkeys::entry_name(options_.host, options_.port);
    std::vector<std::string> other_types;
    for (const auto& f : known_hosts_files()) {
        auto check = hostkeys::check(f.content, options_.host, options_.port, blob);
        const std::string where = std::format("{}, line {}", casement::narrow(f.path), check.line);
        switch (check.verdict) {
            case hostkeys::Verdict::known:
                log::info("host key {} {} found in {}", type, fp, where);
                return c;
            case hostkeys::Verdict::revoked:
                finish(State::failed, std::format("WARNING: the server's {} host key {} is marked as revoked ({}).",
                                                  type, fp, where));
            case hostkeys::Verdict::changed:
                finish(State::failed,
                       std::format("WARNING: the host key of {} has changed. The server shows the {} key {}, "
                                   "which doesn't match {}. Someone may be intercepting the connection. If the "
                                   "change is expected, remove the old key with ssh-keygen -R \"{}\" and connect "
                                   "once with ssh.exe.",
                                   name, type, fp, where, name));
            case hostkeys::Verdict::unknown:
                for (auto& t : check.types) other_types.push_back(t);
                break;
        }
    }
    std::string also;
    if (!other_types.empty()) also = " (known_hosts only has a key of another type for it)";
    finish(State::failed,
           std::format("{} isn't a known host{}. Its {} host key is {}. To trust it, add hostkey={} to the "
                       "connection, or connect once with ssh.exe to add it to known_hosts.",
                       name, also, type, fp, fp));
}

void X2GoSession::sign_in(ssh::Connection& c) {
    std::string user = options_.user.empty() ? windows_user() : options_.user;
    if (user.empty()) finish(State::failed, "The connection has no user name.");
    bool done = false;
    auto methods = c.auth_methods(user, done);
    if (done) {
        log::info("signed in as {} without credentials", user);
        return;
    }
    if (methods.empty()) finish(State::failed, std::format("The server offered no way to sign in: {}", c.last_error()));
    auto offers = [&](std::string_view m) { return std::find(methods.begin(), methods.end(), m) != methods.end(); };
    const bool by_key = offers("publickey"), by_password = offers("password"),
               by_keyboard = offers("keyboard-interactive");
    std::vector<std::string> notes;
    std::string detail;

    // Servers allow six attempts by default and count every refused key,
    // so leave room for the password methods when there is a password.
    int budget = password_.empty() ? 6 : 4;
    auto fail_hard = [&](ssh::Connection::Auth r) {
        if (r == ssh::Connection::Auth::failed)
            finish(State::failed, std::format("Signing in failed: {}", detail.empty() ? c.last_error() : detail));
    };
    auto try_key = [&](const std::string& content, const std::string& label) -> bool {
        if (budget <= 0) return false;
        auto r = c.auth_key(user, content, {}, detail);
        if (r == ssh::Connection::Auth::unreadable && !password_.empty()) {
            // Encrypted: Casement's password may be its passphrase.
            r = c.auth_key(user, content, password_, detail);
            if (r == ssh::Connection::Auth::unreadable) log::info("the password isn't the passphrase of {}", label);
        }
        switch (r) {
            case ssh::Connection::Auth::ok:
                log::info("signed in as {} with the key {}", user, label);
                return true;
            case ssh::Connection::Auth::unreadable:
                notes.push_back(password_.empty()
                                    ? std::format("The key {} needs a passphrase, or isn't a key libssh2 reads.", label)
                                    : std::format("The key {} didn't open with the password as its passphrase.", label));
                return false;
            case ssh::Connection::Auth::denied:
                --budget;
                log::info("the server refused the key {}", label);
                return false;
            case ssh::Connection::Auth::failed: break;
        }
        fail_hard(r);
        return false;
    };

    if (by_key) {
        if (!options_.key.empty()) {
            const std::wstring path = expand_path(options_.key);
            if (auto content = read_file(path)) {
                if (try_key(*content, options_.key)) return;
            } else {
                notes.push_back(std::format("The key file “{}” couldn't be read.", options_.key));
            }
        }
        if (budget > 0) {
            int tried = 0;
            auto r = c.auth_agent(user, tried, detail);
            if (r == ssh::Connection::Auth::ok) {
                log::info("signed in as {} with a key from the SSH agent", user);
                return;
            }
            fail_hard(r);
            budget -= tried;
            if (tried) log::info("the server refused {} key(s) from the SSH agent", tried);
        }
        const std::wstring home = environment(L"USERPROFILE");
        for (const wchar_t* name : {L"id_ed25519", L"id_ecdsa", L"id_rsa"}) {
            if (home.empty() || budget <= 0) break;
            const std::wstring path = home + L"\\.ssh\\" + name;
            if (auto content = read_file(path))
                if (try_key(*content, casement::narrow(path))) return;
        }
    }

    std::vector<std::string> prompts;
    if (!password_.empty()) {
        if (by_password) {
            auto r = c.auth_password(user, password_, detail);
            if (r == ssh::Connection::Auth::ok) {
                log::info("signed in as {} with a password", user);
                return;
            }
            fail_hard(r);
        }
        if (by_keyboard) {
            auto r = c.auth_keyboard(user, password_, prompts, detail);
            if (r == ssh::Connection::Auth::ok) {
                log::info("signed in as {} with keyboard-interactive", user);
                return;
            }
            fail_hard(r);
        }
    }

    std::string extra;
    for (const auto& n : notes) extra += " " + n;
    if (!prompts.empty())
        finish(State::failed, std::format("The server also asked “{}”, which Casement can't answer yet (two-factor "
                                          "sign-in). Use a key instead.{}",
                                          first_line(prompts.front()), extra));
    if (!by_password && !by_keyboard)
        finish(State::failed, std::format("The server only accepts keys for {}, and none of the keys was accepted.{}",
                                          user, extra));
    if (password_.empty()) finish(State::auth_failed, std::format("A password is needed for {}.{}", user, extra));
    finish(State::auth_failed, std::format("The server didn't accept the password for {}.{}", user, extra));
}

std::optional<ssh::ExecResult> X2GoSession::run_command(ssh::Connection& c, std::string_view command,
                                                        Clock::duration timeout, std::string& error) {
    const std::string marker = random_marker();
    log::debug("running: {}", command);
    auto r = c.exec(server::wrap(command, marker), Clock::now() + timeout, error);
    if (!r) return std::nullopt;
    auto out = server::unwrap(r->out, marker);
    if (!out) {
        error = std::format("the server's login shell didn't run it{}", r->err.empty() ? "" : ": " + first_line(r->err));
        if (r->exit_code == 0) r->exit_code = 1;
        r->out.clear();
        return r;
    }
    r->out = std::move(*out);
    if (!r->err.empty()) log::debug("stderr: {}", r->err);
    return r;
}

X2GoSession::Target X2GoSession::attach(ssh::Connection& c) {
    auto [w, h] = wanted_size();
    const server::Geometry g{w, h};
    std::string error;
    log::info("desktop command {}, keyboard layout {}", options_.command, layout_);

    if (options_.resume) {
        auto r = run_command(c, "x2golistsessions", 30s, error);
        if (!r) finish(State::failed, std::format("Listing the X2Go sessions failed: {}", error));
        if (r->exit_code == 127)
            finish(State::failed, "X2Go isn't installed on the server: x2golistsessions wasn't found.");
        auto sessions = server::parse_listsessions(r->out);
        if (auto pick = server::pick_resumable(sessions, options_.command)) {
            log::info("resuming session {}", pick->session_id);
            auto rr = run_command(c, server::resume_command(pick->session_id, g, layout_), 75s, error);
            if (rr && rr->exit_code == 0) {
                if (auto res = server::parse_resume(rr->out)) {
                    log::info("resumed session {}", pick->session_id);
                    Target t;
                    t.session_id = pick->session_id;
                    t.cookie = pick->cookie;
                    t.port = res->graphics_port;
                    return t;
                }
            }
            log::warn("resuming {} failed ({}); starting a new session", pick->session_id,
                      rr ? first_line(rr->err.empty() ? rr->out : rr->err) : error);
        } else {
            log::info("no suspended session of {} to resume", options_.command);
        }
        if (stopping()) return {};
    }

    auto r = run_command(c, server::startagent_command(g, layout_, options_.command), 75s, error);
    if (!r) finish(State::failed, std::format("Starting the X2Go session failed: {}", error));
    if (r->exit_code != 0) finish(State::failed, server::explain_startagent(r->exit_code, r->err));
    auto s = server::parse_startagent(r->out);
    if (!s)
        finish(State::failed,
               std::format("x2gostartagent's answer wasn't understood: {}", first_line(r->out.empty() ? r->err : r->out)));
    log::info("started session {}", s->session_id);
    Target t;
    t.session_id = s->session_id;
    t.cookie = s->cookie;
    t.port = s->graphics_port;
    t.fresh = true;
    t.started = *s;
    return t;
}

void X2GoSession::close_session(ssh::Connection& c, LIBSSH2_CHANNEL* ch, const Target& t, bool connected) {
    if (t.session_id.empty()) return;
    std::string error;
    if (options_.terminate_on_close) {
        log::info("terminating session {}", t.session_id);
        auto r = run_command(c, server::terminate_command(t.session_id), 1400ms, error);
        if (!r || r->exit_code != 0) log::warn("terminating failed: {}", r ? first_line(r->err) : error);
        return;
    }
    if (!connected) return;  // a session nobody connects to suspends itself after 30 s
    log::info("suspending session {}", t.session_id);
    auto r = run_command(c, server::suspend_command(t.session_id), 1400ms, error);
    if (r && r->exit_code == 0) return;
    log::warn("x2gosuspend-session failed ({}); disconnecting the client instead", r ? first_line(r->err) : error);
    // DISCONNECTCLIENT suspends the session too, though the server's list
    // only shows it the next time it is looked at.
    const auto e = kdrive::plain_event(kdrive::Ev::disconnect_client);
    const auto deadline = Clock::now() + 200ms;
    std::size_t sent = 0;
    while (ch && sent < e.size()) {
        auto n = libssh2_channel_write(ch, reinterpret_cast<const char*>(e.data()) + sent, e.size() - sent);
        if (n > 0) sent += static_cast<std::size_t>(n);
        else if (n != LIBSSH2_ERROR_EAGAIN || !c.wait(deadline)) break;
    }
}

void X2GoSession::ended(ssh::Connection& c, const Target& t, bool srv_disconnect) {
    std::string error;
    auto r = run_command(c, server::state_command(t.session_id), 3s, error);
    if (!r) {
        finish(State::closed, "The connection to the server was lost. (Resuming the session from somewhere else "
                              "ends this connection.)");
    }
    const server::State s = server::parse_state(r->out);
    log::info("the session ended: agent state {}{}", s.agent.empty() ? "gone" : s.agent,
              srv_disconnect ? " (the server said so)" : "");
    if (s.not_found)
        finish(State::failed, std::format("The desktop “{}” couldn't start on the server: {} wasn't found there.",
                                          options_.command, *s.not_found));
    if (s.agent == "SUSPENDED")
        finish(State::closed, "The X2Go session was suspended on the server; it can be resumed.");
    finish(State::closed, "The X2Go session ended.");
}

void X2GoSession::stream(ssh::Connection& c, LIBSSH2_CHANNEL* ch, const Target& t) {
    std::string error;
    WicDecoder decoder(error);
    if (!error.empty()) finish(State::failed, error);
    FrameCache frames([&decoder](std::span<const std::uint8_t> data, Image& out) { return decoder.decode(data, out); });
    CursorCache cursors;
    SelectionReceiver selections;
    kdrive::Parser parser;

    std::vector<std::uint8_t> out;
    std::size_t out_pos = 0;
    auto append = [&](std::span<const std::uint8_t> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); };
    auto append_event = [&](const kdrive::Event& e) { append(e); };

    // The client speaks first: the cookie, then its version.
    append(std::span(reinterpret_cast<const std::uint8_t*>(t.cookie.data()), t.cookie.size()));
    append_event(kdrive::client_version_event());

    WSAEVENT socket_event = WSACreateEvent();
    WSAEventSelect(c.socket(), socket_event, FD_READ | FD_WRITE | FD_CLOSE);

    bool running = false;  // SERVERVERSION came, GEOMETRY sent
    bool srv_disconnect = false;
    std::pair<std::uint32_t, std::uint32_t> geometry{0, 0};
    auto now = Clock::now();
    const auto hello_deadline = now + 20s;
    auto last_rx = now;
    auto next_keepalive = now + keepalive_interval;
    auto next_ssh_keepalive = now + 30s;
    bool barrier = false;
    auto barrier_deadline = now;
    std::deque<keys::Action> held;
    std::optional<std::string> from_casement;  // last clipboard text sent to the server
    std::optional<std::string> from_server;    // last clipboard text sent to Casement
    // The cursor Casement shows: a serial, or none_shown / default_shown.
    constexpr std::uint64_t none_shown = ~std::uint64_t{0}, default_shown = none_shown - 1;
    std::uint64_t shown_cursor = none_shown;
    Clock::time_point last_rebuild{};
    bool desktop_started = false;

    auto push_action = [&](const keys::Action& a) {
        if (barrier) {
            held.push_back(a);
        } else if (a.kind == keys::Action::Kind::barrier) {
            // UPDATE of one pixel: its answer shows the server has read
            // everything sent before it.
            append_event(kdrive::update_event(1, 1, 0, 0));
            barrier = true;
            barrier_deadline = Clock::now() + barrier_timeout;
        } else {
            append_event(a.event);
        }
    };
    auto release_held = [&] {
        barrier = false;
        while (!held.empty() && !barrier) {
            keys::Action a = held.front();
            held.pop_front();
            push_action(a);
        }
    };
    auto rebuild = [&](const std::string& why) {
        // A picture that can't be trusted: drop both caches and ask for the
        // whole screen again.
        auto n = Clock::now();
        if (n - last_rebuild < 2s) return;
        last_rebuild = n;
        log::warn("{}; rebuilding the caches", why);
        append_event(kdrive::plain_event(kdrive::Ev::cache_rebuild));
        if (running) append_event(kdrive::geometry_event(static_cast<std::uint16_t>(geometry.first),
                                                         static_cast<std::uint16_t>(geometry.second)));
    };
    auto connection_lost = [&](const std::string& why) {
        log::warn("graphics channel: {}", why);
        if (running)
            finish(State::closed, "The connection to the server was lost. (Resuming the session from somewhere else "
                                  "ends this connection.)");
        finish(State::failed, std::format("The connection to the X2Go session failed: {}", why));
    };

    std::vector<std::uint8_t> buf(std::size_t{1} << 16);
    for (;;) {
        now = Clock::now();
        if (stopping()) {
            close_session(c, ch, t, running);
            WSACloseEvent(socket_event);
            return;
        }

        // What the server has sent so far, a few megabytes at a time so the
        // picture keeps up with a busy stream.
        bool eof = false;
        bool more = false;
        for (std::size_t got = 0;;) {
            if (got >= (std::size_t{4} << 20)) {
                more = true;
                break;
            }
            auto n = libssh2_channel_read(ch, reinterpret_cast<char*>(buf.data()), buf.size());
            if (n > 0) {
                parser.feed(std::span(buf.data(), static_cast<std::size_t>(n)));
                got += static_cast<std::size_t>(n);
                last_rx = now;
                continue;
            }
            if (n == LIBSSH2_ERROR_EAGAIN || (n == 0 && !libssh2_channel_eof(ch))) break;
            if (n == 0 || n == LIBSSH2_ERROR_CHANNEL_CLOSED || n == LIBSSH2_ERROR_CHANNEL_EOF_SENT) {
                eof = true;
                break;
            }
            connection_lost(c.last_error());
        }

        std::vector<Blit> blits;
        try {
            while (auto m = parser.next()) {
                std::visit(
                    overloaded{
                        [&](kdrive::Frame& f) {
                            if (barrier && f.crc == 0 && f.x == 0 && f.y == 0 && f.width == 1 && f.height == 1)
                                release_held();
                            Blit b;
                            std::string why;
                            switch (frames.apply(f, b, why)) {
                                case FrameCache::Result::ok: blits.push_back(std::move(b)); break;
                                case FrameCache::Result::miss:
                                case FrameCache::Result::bad: rebuild(why); break;
                            }
                        },
                        [&](kdrive::Cursor& cur) {
                            const CursorImage* img = cursors.apply(cur);
                            if (!img) {
                                if (!cur.data.empty())
                                    log::debug("cursor {} ({}x{}, {} bytes) not understood", cur.serial, cur.width,
                                               cur.height, cur.data.size());
                                if (shown_cursor != default_shown) host_.send(casement::default_cursor_message());
                                shown_cursor = default_shown;
                                return;
                            }
                            if (shown_cursor == cur.serial) return;
                            shown_cursor = cur.serial;
                            host_.send(casement::cursor_message(img->width, img->height, img->hot_x, img->hot_y,
                                                                img->rgba));
                        },
                        [&](kdrive::SelectionChunk& s) {
                            if (s.notification()) {
                                if (s.selection == kdrive::sel_clipboard)
                                    append_event(kdrive::demand_selection_event(kdrive::sel_clipboard));
                                return;
                            }
                            std::string why;
                            auto done = selections.add(s, why);
                            if (!why.empty()) log::warn("clipboard from the server: {}", why);
                            if (!done || done->selection != kdrive::sel_clipboard) return;
                            auto text = selection_text(done->format, done->data);
                            if (!text) return;
                            // The server repeating what Casement gave it isn't news.
                            if (*text == from_casement) return;
                            from_server = *text;
                            from_casement.reset();
                            host_.send(casement::clipboard_message(*text));
                        },
                        [&](kdrive::Deleted& d) { frames.erase(d.crcs); },
                        [&](kdrive::DeletedCursors& d) {
                            for (auto s : d.serials) cursors.erase(s);
                        },
                        [&](kdrive::ServerVersion& v) {
                            if (running) return;
                            geometry = wanted_size();
                            append_event(kdrive::geometry_event(static_cast<std::uint16_t>(geometry.first),
                                                                static_cast<std::uint16_t>(geometry.second)));
                            running = true;
                            {
                                std::lock_guard g(lock_);
                                connected_ = true;
                            }
                            log::info("connected to session {} (server feature version {}), {}x{}", t.session_id,
                                      v.version, geometry.first, geometry.second);
                            if (v.version < 7) log::warn("this server is older than the plugin expects");
                            host_.send(casement::state_message(State::connected));
                        },
                        [&](kdrive::DemandClientSelection& d) {
                            if (d.selection == kdrive::sel_clipboard && from_casement) {
                                auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(from_casement->data()),
                                                       from_casement->size());
                                append(selection_events(bytes, kdrive::sel_clipboard, kdrive::fmt_utf8));
                            }
                        },
                        [&](kdrive::Signal& s) {
                            switch (s.type) {
                                case kdrive::Msg::reinit:
                                    frames.clear();
                                    cursors.clear();
                                    shown_cursor = none_shown;
                                    break;
                                case kdrive::Msg::srv_disconnect: srv_disconnect = true; break;
                                default: break;
                            }
                        },
                    },
                    *m);
            }
        } catch (const kdrive::ProtocolError& e) {
            finish(State::failed, std::format("The X2Go server sent something the plugin doesn't understand: {}", e.what()));
        }

        if (!blits.empty()) {
            auto d = host_.display();
            for (const auto& b : blits) {
                if (b.resize) d->set_size(b.resize->first, b.resize->second);
                Rect r = draw(*b.image, b.x, b.y, d->buffer(), d->stride(), d->width(), d->height());
                if (r.w > 0) d->damage(r.x, r.y, r.w, r.h);
            }
            d->flush();
        }

        if (eof) {
            WSACloseEvent(socket_event);
            if (!running)
                finish(State::failed, "The X2Go session closed the graphics connection before the handshake. "
                                      "Another client may have connected first.");
            ended(c, t, srv_disconnect);
        }

        now = Clock::now();
        if (!running && now > hello_deadline)
            finish(State::failed, "The X2Go KDrive server didn't answer the handshake. It may be older than "
                                  "x2gokdrive 0.0.0.2, or run on a big-endian machine.");
        if (running && now - last_rx > server_silence) finish(State::closed, "The server stopped answering.");

        if (running) {
            // The desktop of a new session starts once the client is in.
            if (t.fresh && t.started.agent_pid) {
                if (!desktop_started) {
                    desktop_started = true;
                    // Send what is queued first, so the screen arrives while
                    // the command runs.
                    while (out_pos < out.size()) {
                        auto n = libssh2_channel_write(ch, reinterpret_cast<const char*>(out.data()) + out_pos,
                                                       out.size() - out_pos);
                        if (n > 0) out_pos += static_cast<std::size_t>(n);
                        else if (n != LIBSSH2_ERROR_EAGAIN || !c.wait(Clock::now() + 2s)) break;
                    }
                    auto r = run_command(c, server::runcommand_command(t.started, options_.command), 15s, error);
                    if (!r || r->exit_code != 0)
                        log::warn("x2goruncommand didn't start: {}", r ? first_line(r->err) : error);
                }
            }

            std::vector<keys::Action> acts;
            std::optional<std::string> clip;
            std::optional<std::pair<std::uint32_t, std::uint32_t>> size;
            {
                std::lock_guard k(keys_lock_);
                {
                    std::lock_guard g(lock_);
                    acts.swap(actions_);
                    clip.swap(clipboard_out_);
                    if (wanted_ && *wanted_ != geometry && now >= resize_at_) size = wanted_;
                }
                auto repeats = keyboard_.tick(now);
                acts.insert(acts.end(), repeats.begin(), repeats.end());
            }
            for (const auto& a : acts) push_action(a);
            if (barrier && now >= barrier_deadline) {
                log::debug("barrier timed out");
                release_held();
            }
            // Casement repeating what the server gave it isn't news either.
            if (clip && !clip->empty() && clip != from_server) {
                from_casement = *clip;
                from_server.reset();
                auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(clip->data()), clip->size());
                append(selection_events(bytes, kdrive::sel_clipboard, kdrive::fmt_utf8));
            }
            if (size) {
                geometry = *size;
                log::info("resizing to {}x{}", geometry.first, geometry.second);
                append_event(kdrive::geometry_event(static_cast<std::uint16_t>(geometry.first),
                                                    static_cast<std::uint16_t>(geometry.second)));
            }
            if (now >= next_keepalive) {
                append_event(kdrive::plain_event(kdrive::Ev::keepalive));
                next_keepalive = now + keepalive_interval;
            }
        }
        if (now >= next_ssh_keepalive) next_ssh_keepalive = now + std::chrono::seconds(c.keepalive());

        while (out_pos < out.size()) {
            auto n = libssh2_channel_write(ch, reinterpret_cast<const char*>(out.data()) + out_pos, out.size() - out_pos);
            if (n > 0) {
                out_pos += static_cast<std::size_t>(n);
                continue;
            }
            if (n == LIBSSH2_ERROR_EAGAIN) break;
            connection_lost(c.last_error());
        }
        if (out_pos == out.size()) {
            out.clear();
            out_pos = 0;
        }

        // Sleep until the socket, Casement or a timer needs attention.
        auto next = now + 250ms;
        if (running) {
            next = std::min(next, next_keepalive);
            if (barrier) next = std::min(next, barrier_deadline);
            {
                std::lock_guard k(keys_lock_);
                if (auto tick = keyboard_.next_tick()) next = std::min(next, *tick);
            }
            {
                std::lock_guard g(lock_);
                if (wanted_ && *wanted_ != geometry) next = std::min(next, resize_at_);
            }
        }
        auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count();
        if (wait_ms > 0 && !more) {
            HANDLE handles[2] = {socket_event, wake_};
            DWORD r = WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(wait_ms));
            if (r == WAIT_OBJECT_0) {
                WSANETWORKEVENTS ne;
                WSAEnumNetworkEvents(c.socket(), socket_event, &ne);
            }
        }
    }
}

}  // namespace

std::unique_ptr<casement::Session> start_session(casement::Host& host, const casement::Connect& connect) {
    std::string error;
    auto options = parse_options(connect.address, connect.username, error);
    if (!options) host.finish(State::failed, error);
    return std::make_unique<X2GoSession>(host, std::move(*options), connect.password);
}

}  // namespace x2go
