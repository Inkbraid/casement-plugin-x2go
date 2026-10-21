#include "x2go/options.h"

#include <format>

#include "casement/address.h"
#include "casement/text.h"
#include "x2go/keyboard.h"
#include "x2go/server.h"

namespace x2go {

std::optional<std::string> normalize_fingerprint(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '=')) text.remove_suffix(1);
    if (text.size() > 7 && (text.substr(0, 7) == "SHA256:" || text.substr(0, 7) == "sha256:")) text.remove_prefix(7);
    // 32 bytes are 43 base64 characters without padding.
    if (text.size() != 43) return std::nullopt;
    std::string padded(text);
    padded += '=';
    auto raw = casement::base64_decode(padded);
    if (!raw || raw->size() != 32) return std::nullopt;
    // Re-encoded, so URL-safe base64 ends up in the usual alphabet.
    std::string canonical = casement::base64_encode(*raw);
    while (!canonical.empty() && canonical.back() == '=') canonical.pop_back();
    return "SHA256:" + canonical;
}

std::optional<Options> parse_options(std::string_view address, std::string_view username, std::string& error) {
    auto a = casement::parse_address(address, error);
    if (!a) return std::nullopt;
    if (a->scheme != "x2go") {
        error = std::format("“{}” isn't an X2Go address.", address);
        return std::nullopt;
    }
    Options o;
    o.host = a->host;
    if (a->port) o.port = *a->port;
    o.user = !username.empty() ? std::string(username) : a->user;

    if (const std::string* c = a->param("command"); c && !c->empty()) {
        auto cmd = server::normalize_command(*c, error);
        if (!cmd) return std::nullopt;
        o.command = *cmd;
    }
    if (const std::string* l = a->param("layout"); l && !l->empty()) {
        if (!keys::valid_layout(*l)) {
            error = std::format("“{}” isn't a keyboard layout name such as de or de(nodeadkeys).", *l);
            return std::nullopt;
        }
        o.layout = *l;
    }
    o.resume = a->flag("resume");
    if (const std::string* c = a->param("close"); c && !c->empty()) {
        if (*c == "terminate") {
            o.terminate_on_close = true;
        } else if (*c != "suspend") {
            error = std::format("“close” must be suspend or terminate, not “{}”.", *c);
            return std::nullopt;
        }
    }
    if (const std::string* h = a->param("hostkey"); h && !h->empty()) {
        auto fp = normalize_fingerprint(*h);
        if (!fp) {
            error = std::format("“{}” isn't a SHA256 host key fingerprint such as ssh-keygen -lf shows.", *h);
            return std::nullopt;
        }
        o.hostkey = *fp;
    }
    if (const std::string* k = a->param("key"); k && !k->empty()) o.key = *k;
    return o;
}

}  // namespace x2go
