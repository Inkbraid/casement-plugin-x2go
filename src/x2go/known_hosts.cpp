#include "x2go/known_hosts.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <cctype>
#include <format>

#include "casement/text.h"

namespace x2go::hostkeys {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// OpenSSH's match_pattern: * and ? wildcards.
bool wildcard(std::string_view s, std::string_view p) {
    std::size_t si = 0, pi = 0, star = std::string_view::npos, back = 0;
    while (si < s.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == s[si])) {
            ++si;
            ++pi;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            back = si;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            si = ++back;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

bool hashed_matches(std::string_view field, std::string_view name) {
    // |1|salt|hash
    field.remove_prefix(3);
    auto bar = field.find('|');
    if (bar == std::string_view::npos) return false;
    auto salt = casement::base64_decode(field.substr(0, bar));
    auto hash = casement::base64_decode(field.substr(bar + 1));
    if (!salt || !hash || hash->size() != 20) return false;
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (!HMAC(EVP_sha1(), salt->data(), static_cast<int>(salt->size()),
              reinterpret_cast<const unsigned char*>(name.data()), name.size(), out, &n) ||
        n != 20)
        return false;
    return std::equal(hash->begin(), hash->end(), out);
}

struct Line {
    std::string_view marker;
    std::string_view hosts;
    std::string_view type;
    std::string_view key;
};

bool parse_line(std::string_view l, Line& out) {
    auto token = [&l]() {
        while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
        auto end = l.find_first_of(" \t");
        std::string_view t = l.substr(0, end);
        l.remove_prefix(end == std::string_view::npos ? l.size() : end);
        return t;
    };
    if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    std::string_view first = token();
    if (first.empty() || first.front() == '#') return false;
    out = Line{};
    if (first.front() == '@') {
        out.marker = first;
        first = token();
    }
    out.hosts = first;
    out.type = token();
    out.key = token();
    return !out.hosts.empty() && !out.type.empty() && !out.key.empty();
}

template <class F>
void each_entry(std::string_view content, std::string_view host, std::uint16_t port, F f) {
    const std::string name = entry_name(host, port);
    int number = 0;
    while (!content.empty()) {
        auto nl = content.find('\n');
        std::string_view l = content.substr(0, nl);
        content.remove_prefix(nl == std::string_view::npos ? content.size() : nl + 1);
        ++number;
        Line line;
        if (!parse_line(l, line) || !host_matches(line.hosts, name)) continue;
        f(line, number);
    }
}

}  // namespace

std::string blob_type(std::span<const std::uint8_t> blob) {
    if (blob.size() < 4) return {};
    const std::size_t n = std::size_t{blob[0]} << 24 | std::size_t{blob[1]} << 16 | std::size_t{blob[2]} << 8 | blob[3];
    if (n > 64 || 4 + n > blob.size()) return {};
    return std::string(reinterpret_cast<const char*>(blob.data() + 4), n);
}

std::string sha256_fingerprint(std::span<const std::uint8_t> blob) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (!EVP_Digest(blob.data(), blob.size(), digest, &n, EVP_sha256(), nullptr)) return {};
    std::string b64 = casement::base64_encode(std::span<const std::uint8_t>(digest, n));
    while (!b64.empty() && b64.back() == '=') b64.pop_back();
    return "SHA256:" + b64;
}

std::string entry_name(std::string_view host, std::uint16_t port) {
    std::string h = lower(host);
    return port == 22 ? h : std::format("[{}]:{}", h, port);
}

bool host_matches(std::string_view field, std::string_view name) {
    if (field.starts_with("|1|")) return hashed_matches(field, name);
    const std::string n = lower(name);
    bool positive = false;
    while (!field.empty()) {
        auto comma = field.find(',');
        std::string_view pattern = field.substr(0, comma);
        field.remove_prefix(comma == std::string_view::npos ? field.size() : comma + 1);
        const bool negated = pattern.starts_with('!');
        if (negated) pattern.remove_prefix(1);
        if (pattern.empty() || !wildcard(n, lower(pattern))) continue;
        if (negated) return false;
        positive = true;
    }
    return positive;
}

Check check(std::string_view content, std::string_view host, std::uint16_t port, std::span<const std::uint8_t> blob) {
    Check c;
    const std::string type = blob_type(blob);
    bool known = false, changed = false, revoked = false;
    int known_line = 0, changed_line = 0, revoked_line = 0;
    each_entry(content, host, port, [&](const Line& line, int number) {
        auto key = casement::base64_decode(line.key);
        if (!key) return;
        const bool same = key->size() == blob.size() && std::equal(key->begin(), key->end(), blob.begin());
        if (line.marker == "@revoked") {
            if (same && !revoked) {
                revoked = true;
                revoked_line = number;
            }
            return;
        }
        if (!line.marker.empty()) return;  // @cert-authority: certificates aren't used here
        const std::string line_type = blob_type(*key);
        if (std::find(c.types.begin(), c.types.end(), line_type) == c.types.end()) c.types.push_back(line_type);
        if (line_type != type) return;
        if (same) {
            if (!known) known_line = number;
            known = true;
        } else if (!changed) {
            changed = true;
            changed_line = number;
        }
    });
    if (revoked) {
        c.verdict = Verdict::revoked;
        c.line = revoked_line;
    } else if (known) {
        c.verdict = Verdict::known;
        c.line = known_line;
    } else if (changed) {
        c.verdict = Verdict::changed;
        c.line = changed_line;
    }
    return c;
}

std::vector<std::string> known_types(std::string_view content, std::string_view host, std::uint16_t port) {
    std::vector<std::string> types;
    each_entry(content, host, port, [&](const Line& line, int) {
        if (!line.marker.empty()) return;
        auto key = casement::base64_decode(line.key);
        if (!key) return;
        std::string t = blob_type(*key);
        if (!t.empty() && std::find(types.begin(), types.end(), t) == types.end()) types.push_back(t);
    });
    return types;
}

}  // namespace x2go::hostkeys
