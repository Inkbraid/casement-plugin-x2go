#!/usr/bin/env bash
# Starts the X2Go test server on this machine and runs the end-to-end tests
# of a build directory (a MinGW cross build; the tests run under Wine):
#
#   ci/e2e.sh [build-dir]
#
# The server script needs root; without it, sudo is used. The X2GO_E2E_*
# settings tools/e2e/x2go-server.sh reads are passed through.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
BUILD=${1:-build}

run_root() {
    if [ "$(id -u)" = 0 ]; then
        "$@"
    else
        sudo --preserve-env=X2GO_E2E_DIR,X2GO_E2E_PORT,X2GO_E2E_USER,X2GO_E2E_PASSWORD,X2GO_E2E_PORT_BASE "$@"
    fi
}

run_root tools/e2e/x2go-server.sh start
eval "$(run_root tools/e2e/x2go-server.sh env)"
export WINEDEBUG=${WINEDEBUG:--all}

status=0
ctest --test-dir "$BUILD" --output-on-failure -L e2e || status=$?
# Leave no sessions behind, whatever the tests did.
run_root tools/e2e/x2go-server.sh clean || true
exit "$status"
