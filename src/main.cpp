// casement-x2go: X2Go KDrive sessions in Casement.

#include "casement/plugin.h"
#include "x2go/session.h"

int main() {
    casement::Info info{"X2GO", "X2Go", X2GO_PLUGIN_VERSION, {"password", "clipboard", "cursor", "resize"}, 1, 1};
    return casement::run(info, x2go::start_session);
}
