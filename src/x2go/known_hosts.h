// SSH host keys: OpenSSH's known_hosts format (read only) and SHA-256
// fingerprints as ssh and ssh-keygen print them.
//
// A known_hosts line is "[@marker] hosts keytype base64-key [comment]".
// Hosts is a comma-separated list of names and patterns (* and ?, ! to
// exclude), or one hashed name "|1|base64-salt|base64-HMAC-SHA1". A host
// on another port than 22 is written "[host]:port".

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace x2go::hostkeys {

/// The key type an SSH public key blob names ("ssh-ed25519", "ssh-rsa",
/// "ecdsa-sha2-nistp256", ...), or empty.
std::string blob_type(std::span<const std::uint8_t> blob);

/// "SHA256:" and the unpadded base64 of SHA-256 over the key blob.
std::string sha256_fingerprint(std::span<const std::uint8_t> blob);

/// How a host appears in known_hosts.
std::string entry_name(std::string_view host, std::uint16_t port);

/// Whether a known_hosts host field covers `name` (already in entry_name
/// form). Names are compared without regard to case.
bool host_matches(std::string_view field, std::string_view name);

enum class Verdict {
    /// No entry for this host with the server's key type.
    unknown,
    known,
    /// An entry with the server's key type and another key.
    changed,
    /// The key is marked @revoked.
    revoked,
};

struct Check {
    Verdict verdict = Verdict::unknown;
    /// The key types listed for this host.
    std::vector<std::string> types;
    /// The line of the entry that decided (1-based), for messages.
    int line = 0;
};

/// Checks a server key against the content of a known_hosts file.
Check check(std::string_view content, std::string_view host, std::uint16_t port,
            std::span<const std::uint8_t> blob);

/// The key types known for a host, in file order, without duplicates.
std::vector<std::string> known_types(std::string_view content, std::string_view host, std::uint16_t port);

}  // namespace x2go::hostkeys
