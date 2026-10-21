// A connection's settings, from its address:
//   x2go://[user@]host[:port]?command=XFCE&layout=de&resume&close=suspend&hostkey=SHA256:...&key=C:\...

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace x2go {

struct Options {
    std::string host;
    std::uint16_t port = 22;
    /// Empty: the Windows user name, as ssh does.
    std::string user;
    /// For x2goruncommand: a desktop name such as XFCE, or a program.
    std::string command = "XFCE";
    /// XKB layout with optional variant; empty: from the Windows layout.
    std::string layout;
    /// Resume the newest suspended session started with the same command.
    bool resume = false;
    /// What closing the tab does to the session.
    bool terminate_on_close = false;
    /// A pinned host key: "SHA256:" and the unpadded base64 of the hash.
    std::string hostkey;
    /// A private key file (UTF-8; environment variables are expanded).
    std::string key;
};

/// Reads the settings from `connect`'s address and user name. On failure
/// `error` is a sentence for the user.
std::optional<Options> parse_options(std::string_view address, std::string_view username, std::string& error);

/// A fingerprint as the user may give it ("SHA256:abc…", with or without
/// the prefix and padding) in the form `sha256_fingerprint` produces, or
/// nothing when it isn't one.
std::optional<std::string> normalize_fingerprint(std::string_view text);

}  // namespace x2go
