#include "x2go/ssh.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>

#include "casement/log.h"
#include "casement/text.h"

namespace x2go::ssh {

namespace log = casement::log;

namespace {

constexpr int setup_timeout_ms = 20000;

std::string socket_error(int code) { return casement::system_message(static_cast<unsigned long>(code)); }

/// The host key algorithms (method names) for a key type in known_hosts.
std::vector<std::string> algorithms_for(const std::string& type) {
    if (type == "ssh-rsa") return {"rsa-sha2-512", "rsa-sha2-256", "ssh-rsa"};
    return {type};
}

/// Keyboard-interactive answers: the session's abstract pointer leads here.
struct KbdContext {
    const std::string* password = nullptr;
    std::vector<std::string>* prompts = nullptr;
    bool answered_password = false;
};

bool looks_like_password_prompt(std::string_view prompt) {
    std::string p(prompt);
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return p.find("password") != std::string::npos || p.find("passphrase") != std::string::npos ||
           p.find("passwort") != std::string::npos || p.find("mot de passe") != std::string::npos;
}

char* duplicate(const std::string& s) {
    // libssh2 frees responses with its allocator, which is malloc here.
    auto* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

void kbd_callback(const char*, int, const char*, int, int num_prompts, const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
                  LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void** abstract) {
    auto* ctx = static_cast<KbdContext*>(*abstract);
    for (int i = 0; i < num_prompts; ++i) {
        std::string_view text(reinterpret_cast<const char*>(prompts[i].text), prompts[i].length);
        std::string answer;
        if (ctx && ctx->password && !prompts[i].echo && looks_like_password_prompt(text)) {
            answer = *ctx->password;
            ctx->answered_password = true;
        } else if (ctx && ctx->prompts) {
            ctx->prompts->emplace_back(text);
        }
        responses[i].text = duplicate(answer);
        responses[i].length = responses[i].text ? static_cast<unsigned int>(answer.size()) : 0;
    }
}

}  // namespace

bool init(std::string& error) {
    static std::once_flag once;
    static int result = 0;
    static std::string message;
    std::call_once(once, [] {
        WSADATA data;
        if (int rc = WSAStartup(MAKEWORD(2, 2), &data); rc != 0) {
            result = rc;
            message = "Windows sockets didn't start: " + socket_error(rc);
            return;
        }
        if (int rc = libssh2_init(0); rc != 0) {
            result = rc;
            message = "The SSH library didn't start.";
        }
    });
    if (result != 0) error = message;
    return result == 0;
}

Connection::~Connection() {
    if (session_) {
        libssh2_session_set_blocking(session_, 0);
        libssh2_session_disconnect(session_, "Closing");
        libssh2_session_free(session_);
    }
    if (socket_ != INVALID_SOCKET) closesocket(socket_);
}

std::string Connection::last_error() const {
    if (!session_) return {};
    char* msg = nullptr;
    int len = 0;
    libssh2_session_last_error(session_, &msg, &len, 0);
    return msg ? std::string(msg, static_cast<std::size_t>(len)) : std::string();
}

bool Connection::wait(Clock::time_point deadline) {
    for (;;) {
        if (cancelled()) return false;
        auto now = Clock::now();
        if (now >= deadline) return false;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto slice = std::min<std::chrono::milliseconds>(left, std::chrono::milliseconds(100));
        const int dir = session_ ? libssh2_session_block_directions(session_) : 0;
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        if (dir == 0 || (dir & LIBSSH2_SESSION_BLOCK_INBOUND)) FD_SET(socket_, &rd);
        if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(socket_, &wr);
        timeval tv{static_cast<long>(slice.count() / 1000), static_cast<long>((slice.count() % 1000) * 1000)};
        int rc = select(0, &rd, &wr, nullptr, &tv);
        if (rc > 0) return true;
        if (rc < 0) return false;
    }
}

const std::vector<std::string>& Connection::host_key_types() {
    static const std::vector<std::string> types{"ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
                                                "ecdsa-sha2-nistp521", "ssh-rsa"};
    return types;
}

std::unique_ptr<Connection> Connection::open(const std::string& host, std::uint16_t port,
                                             const std::vector<std::string>& key_types, bool only_these,
                                             const std::atomic<bool>* cancel, std::string& error) {
    std::unique_ptr<Connection> c(new Connection());
    c->cancel_ = cancel;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    const std::string service = std::to_string(port);
    if (int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &found); rc != 0) {
        error = std::format("The server “{}” wasn't found: {}", host, socket_error(rc));
        return nullptr;
    }
    std::string last = "no address";
    for (addrinfo* a = found; a && c->socket_ == INVALID_SOCKET; a = a->ai_next) {
        SOCKET s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) {
            last = socket_error(WSAGetLastError());
            continue;
        }
        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        bool ok = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            auto deadline = Clock::now() + std::chrono::seconds(15);
            while (!ok && Clock::now() < deadline && !(cancel && cancel->load())) {
                fd_set wr, ex;
                FD_ZERO(&wr);
                FD_ZERO(&ex);
                FD_SET(s, &wr);
                FD_SET(s, &ex);
                timeval tv{0, 100000};
                int rc = select(0, nullptr, &wr, &ex, &tv);
                if (rc < 0) break;
                if (rc == 0) continue;
                int so = 0;
                int len = sizeof(so);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &len);
                if (FD_ISSET(s, &ex) || so != 0) {
                    last = socket_error(so ? so : WSAECONNREFUSED);
                    break;
                }
                ok = true;
            }
            if (!ok && last == "no address") last = "the connection timed out";
        } else if (!ok) {
            last = socket_error(WSAGetLastError());
        }
        if (!ok) {
            closesocket(s);
            continue;
        }
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        c->socket_ = s;
    }
    freeaddrinfo(found);
    if (c->socket_ == INVALID_SOCKET) {
        if (c->cancelled()) error = "Cancelled.";
        else error = std::format("Can't connect to {} port {}: {}.", host, port, last);
        return nullptr;
    }

    c->session_ = libssh2_session_init_ex(nullptr, nullptr, nullptr, nullptr);
    if (!c->session_) {
        error = "The SSH library couldn't start a session.";
        return nullptr;
    }
    libssh2_session_set_blocking(c->session_, 1);
    libssh2_session_set_timeout(c->session_, setup_timeout_ms);

    // The host key algorithms: the requested types first (the ones
    // known_hosts has keys for, as OpenSSH does it), then OpenSSH's order.
    // libssh2 puts ECDSA first; a fingerprint a user copied from ssh is
    // usually the Ed25519 key's.
    {
        const char** algs = nullptr;
        int n = libssh2_session_supported_algs(c->session_, LIBSSH2_METHOD_HOSTKEY, &algs);
        std::vector<std::string> supported;
        if (n > 0 && algs) {
            supported.assign(algs, algs + n);
            libssh2_free(c->session_, static_cast<void*>(algs));
        }
        std::vector<std::string> order;
        auto add = [&](const std::string& type) {
            for (const auto& alg : algorithms_for(type))
                if (std::find(supported.begin(), supported.end(), alg) != supported.end() &&
                    std::find(order.begin(), order.end(), alg) == order.end())
                    order.push_back(alg);
        };
        for (const auto& t : key_types) add(t);
        if (!only_these)
            for (const auto& t : host_key_types()) add(t);
        std::string pref;
        for (const auto& alg : order) pref += (pref.empty() ? "" : ",") + alg;
        if (pref.empty() || libssh2_session_method_pref(c->session_, LIBSSH2_METHOD_HOSTKEY, pref.c_str()) != 0) {
            error = std::format("No usable host key algorithm for {}.", host);
            return nullptr;
        }
    }

    if (libssh2_session_handshake(c->session_, c->socket_) != 0) {
        error = std::format("The SSH handshake with {} failed: {}.", host, c->last_error());
        return nullptr;
    }
    return c;
}

std::vector<std::uint8_t> Connection::host_key() const {
    std::size_t len = 0;
    int type = 0;
    const char* key = libssh2_session_hostkey(session_, &len, &type);
    if (!key) return {};
    return std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(key),
                                     reinterpret_cast<const std::uint8_t*>(key) + len);
}

std::vector<std::string> Connection::auth_methods(const std::string& user, bool& done) {
    done = false;
    char* list = libssh2_userauth_list(session_, user.c_str(), static_cast<unsigned int>(user.size()));
    std::vector<std::string> out;
    if (!list) {
        done = libssh2_userauth_authenticated(session_) != 0;
        return out;
    }
    std::string_view s(list);
    while (!s.empty()) {
        auto comma = s.find(',');
        out.emplace_back(s.substr(0, comma));
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    return out;
}

bool Connection::authenticated() { return libssh2_userauth_authenticated(session_) != 0; }

Connection::Auth Connection::auth_key(const std::string& user, const std::string& key, const std::string& passphrase,
                                      std::string& detail) {
    int rc = libssh2_userauth_publickey_frommemory(session_, user.c_str(), user.size(), nullptr, 0, key.data(),
                                                   key.size(), passphrase.empty() ? nullptr : passphrase.c_str());
    if (rc == 0) return Auth::ok;
    detail = last_error();
    if (rc == LIBSSH2_ERROR_FILE) return Auth::unreadable;
    if (rc == LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED || rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED ||
        rc == LIBSSH2_ERROR_METHOD_NOT_SUPPORTED)
        return Auth::denied;
    return Auth::failed;
}

Connection::Auth Connection::auth_agent(const std::string& user, int& tried, std::string& detail) {
    tried = 0;
    LIBSSH2_AGENT* agent = libssh2_agent_init(session_);
    if (!agent) return Auth::denied;
    Auth result = Auth::denied;
    if (libssh2_agent_connect(agent) == 0) {
        if (libssh2_agent_list_identities(agent) == 0) {
            libssh2_agent_publickey* identity = nullptr;
            libssh2_agent_publickey* previous = nullptr;
            // Five keys at most: servers count every refused key towards
            // MaxAuthTries (6 by default) and the password still has to fit.
            while (tried < 5 && libssh2_agent_get_identity(agent, &identity, previous) == 0) {
                previous = identity;
                ++tried;
                int rc = libssh2_agent_userauth(agent, user.c_str(), identity);
                if (rc == 0) {
                    result = Auth::ok;
                    break;
                }
                if (rc != LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED && rc != LIBSSH2_ERROR_AUTHENTICATION_FAILED &&
                    rc != LIBSSH2_ERROR_AGENT_PROTOCOL) {
                    detail = last_error();
                    result = Auth::failed;
                    break;
                }
            }
        }
        libssh2_agent_disconnect(agent);
    }
    libssh2_agent_free(agent);
    return result;
}

Connection::Auth Connection::auth_password(const std::string& user, const std::string& password, std::string& detail) {
    int rc = libssh2_userauth_password_ex(session_, user.c_str(), static_cast<unsigned int>(user.size()),
                                          password.c_str(), static_cast<unsigned int>(password.size()), nullptr);
    if (rc == 0) return Auth::ok;
    detail = last_error();
    if (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED || rc == LIBSSH2_ERROR_PASSWORD_EXPIRED ||
        rc == LIBSSH2_ERROR_METHOD_NOT_SUPPORTED)
        return Auth::denied;
    return Auth::failed;
}

Connection::Auth Connection::auth_keyboard(const std::string& user, const std::string& password,
                                           std::vector<std::string>& prompts, std::string& detail) {
    KbdContext ctx{&password, &prompts, false};
    void** abstract = libssh2_session_abstract(session_);
    void* saved = *abstract;
    *abstract = &ctx;
    int rc = libssh2_userauth_keyboard_interactive_ex(session_, user.c_str(), static_cast<unsigned int>(user.size()),
                                                      kbd_callback);
    *abstract = saved;
    if (rc == 0) return Auth::ok;
    detail = last_error();
    if (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED || rc == LIBSSH2_ERROR_METHOD_NOT_SUPPORTED) return Auth::denied;
    return Auth::failed;
}

void Connection::go_nonblocking() { libssh2_session_set_blocking(session_, 0); }

void Connection::configure_keepalive(int seconds) { libssh2_keepalive_config(session_, 1, static_cast<unsigned>(seconds)); }

int Connection::keepalive() {
    int next = 0;
    if (libssh2_keepalive_send(session_, &next) != 0) return 1;
    return next > 0 ? next : 1;
}

std::optional<ExecResult> Connection::exec(const std::string& command, Clock::time_point deadline,
                                           std::string& error) {
    LIBSSH2_CHANNEL* ch = nullptr;
    while (!(ch = libssh2_channel_open_session(session_))) {
        if (libssh2_session_last_errno(session_) != LIBSSH2_ERROR_EAGAIN) {
            error = "the server refused a session channel: " + last_error();
            return std::nullopt;
        }
        if (!wait(deadline)) {
            error = cancelled() ? "cancelled" : "the server didn't open a session channel in time";
            return std::nullopt;
        }
    }
    ExecResult r;
    auto finish = [&](bool ok) -> std::optional<ExecResult> {
        // Close politely if there is time; the channel goes either way.
        int rc;
        while ((rc = libssh2_channel_close(ch)) == LIBSSH2_ERROR_EAGAIN && wait(deadline)) {
        }
        if (rc == 0)
            while (libssh2_channel_wait_closed(ch) == LIBSSH2_ERROR_EAGAIN && wait(deadline)) {
            }
        if (ok) r.exit_code = libssh2_channel_get_exit_status(ch);
        libssh2_channel_free(ch);
        if (!ok) return std::nullopt;
        return r;
    };
    int rc;
    while ((rc = libssh2_channel_exec(ch, command.c_str())) == LIBSSH2_ERROR_EAGAIN) {
        if (!wait(deadline)) {
            error = "the command didn't start in time";
            return finish(false);
        }
    }
    if (rc != 0) {
        error = "the server didn't run the command: " + last_error();
        return finish(false);
    }
    char buf[16384];
    for (;;) {
        ssize_t n = libssh2_channel_read(ch, buf, sizeof(buf));
        if (n > 0) {
            r.out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        ssize_t m = libssh2_channel_read_stderr(ch, buf, sizeof(buf));
        if (m > 0) {
            r.err.append(buf, static_cast<std::size_t>(m));
            continue;
        }
        if (libssh2_channel_eof(ch)) break;
        if ((n < 0 && n != LIBSSH2_ERROR_EAGAIN) || (m < 0 && m != LIBSSH2_ERROR_EAGAIN)) {
            error = "the command's channel failed: " + last_error();
            return finish(false);
        }
        if (n == 0 && m == 0) {
            // Nothing buffered and no EOF yet: wait for the socket.
        }
        if (!wait(deadline)) {
            error = cancelled() ? "cancelled" : "the command didn't finish in time";
            return finish(false);
        }
    }
    return finish(true);
}

LIBSSH2_CHANNEL* Connection::open_tunnel(const std::string& host, std::uint16_t port, Clock::time_point deadline,
                                         std::string& error) {
    for (;;) {
        LIBSSH2_CHANNEL* ch = libssh2_channel_direct_tcpip_ex(session_, host.c_str(), port, "127.0.0.1", 22);
        if (ch) return ch;
        const int rc = libssh2_session_last_errno(session_);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (!wait(deadline)) {
                error = cancelled() ? "cancelled" : "the server didn't open the graphics channel in time";
                return nullptr;
            }
            continue;
        }
        error = last_error();
        return nullptr;
    }
}

}  // namespace x2go::ssh
