// The SSH connection (libssh2): one TCP connection that carries the exec
// channels for the X2Go scripts and the direct-tcpip channel to the
// session's graphics port.
//
// Setup (handshake, sign-in) runs in blocking mode with a timeout; after
// that the session is non-blocking and every call that would block is
// retried after waiting for the socket, so commands can run while the
// graphics channel stays open.

#pragma once

#include <winsock2.h>

#include <libssh2.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace x2go::ssh {

using Clock = std::chrono::steady_clock;

/// Starts Winsock and libssh2, once per process.
bool init(std::string& error);

struct ExecResult {
    int exit_code = -1;
    std::string out;
    std::string err;
};

class Connection {
public:
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /// Connects to the server and runs the SSH handshake. The host key
    /// algorithms for `key_types` (such as "ssh-ed25519") are offered first,
    /// then, unless `only_these`, the others in OpenSSH's order: Ed25519,
    /// ECDSA, RSA. Host certificates aren't offered. `cancel` is checked
    /// while waiting. On failure `error` is a sentence for the user.
    static std::unique_ptr<Connection> open(const std::string& host, std::uint16_t port,
                                            const std::vector<std::string>& key_types, bool only_these,
                                            const std::atomic<bool>* cancel, std::string& error);

    /// The host key types the plugin can check, in OpenSSH's order.
    static const std::vector<std::string>& host_key_types();

    /// The server's host key blob.
    std::vector<std::uint8_t> host_key() const;

    /// The sign-in methods the server offers. `done` is set when the
    /// server let the user in without any ("none").
    std::vector<std::string> auth_methods(const std::string& user, bool& done);

    /// ok; denied by the server; failed (the connection broke); a key that
    /// couldn't be read (an unknown format, or encrypted and the passphrase
    /// missing or wrong).
    enum class Auth { ok, denied, failed, unreadable };
    Auth auth_key(const std::string& user, const std::string& private_key, const std::string& passphrase,
                  std::string& detail);
    /// Pageant or the Windows OpenSSH agent. `tried` counts the keys offered.
    Auth auth_agent(const std::string& user, int& tried, std::string& detail);
    Auth auth_password(const std::string& user, const std::string& password, std::string& detail);
    /// Keyboard-interactive: every prompt that asks for a password gets
    /// `password`; other prompts (one-time codes) are recorded in `prompts`.
    Auth auth_keyboard(const std::string& user, const std::string& password, std::vector<std::string>& prompts,
                       std::string& detail);
    bool authenticated();

    /// From now on calls don't block; the I/O loop waits on `socket()`.
    void go_nonblocking();
    /// Sends keepalive@openssh.com requests every `seconds` (see `keepalive`).
    void configure_keepalive(int seconds);
    /// Sends a keepalive when one is due; returns seconds until the next.
    int keepalive();

    /// Runs a command in an exec channel and collects its output, giving up
    /// at `deadline`. Nothing, with `error` set, when the channel failed.
    std::optional<ExecResult> exec(const std::string& command, Clock::time_point deadline, std::string& error);

    /// Opens the graphics channel to host:port as seen from the server.
    LIBSSH2_CHANNEL* open_tunnel(const std::string& host, std::uint16_t port, Clock::time_point deadline,
                                 std::string& error);

    /// Waits until the socket can make progress in the direction libssh2
    /// is blocked in, or until the deadline (or a slice of 100 ms, to look
    /// at the cancel flag). False when the deadline passed or cancelled.
    bool wait(Clock::time_point deadline);

    LIBSSH2_SESSION* session() const { return session_; }
    SOCKET socket() const { return socket_; }
    /// libssh2's last error, as text.
    std::string last_error() const;
    bool cancelled() const { return cancel_ && cancel_->load(); }
    /// Replaces the flag waits look at; nullptr: waits only end at their
    /// deadlines.
    void set_cancel(const std::atomic<bool>* cancel) { cancel_ = cancel; }

private:
    Connection() = default;

    SOCKET socket_ = INVALID_SOCKET;
    LIBSSH2_SESSION* session_ = nullptr;
    const std::atomic<bool>* cancel_ = nullptr;
};

}  // namespace x2go::ssh
