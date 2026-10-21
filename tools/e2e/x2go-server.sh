#!/usr/bin/env bash
# An X2Go server for the end-to-end tests, on this machine: the X2Go and
# KDrive packages, a test user with a password and key pairs, a small
# desktop (twm and an xterm), and a private sshd on 127.0.0.1. Run as root
# (sudo); it is idempotent and brings everything back after a restart.
#
#   tools/e2e/x2go-server.sh start    install, create, start sshd, end old sessions
#   tools/e2e/x2go-server.sh env      print the X2GO_E2E_* variables for the tests
#   tools/e2e/x2go-server.sh status   sshd and the test user's sessions
#   tools/e2e/x2go-server.sh clean    end the test user's sessions
#   tools/e2e/x2go-server.sh stop     clean, then stop sshd
#
# Settings (environment):
#   X2GO_E2E_DIR        work folder: keys, known_hosts, sshd (default
#                       /tmp/casement-x2go-e2e)
#   X2GO_E2E_PORT       sshd port (default 2322)
#   X2GO_E2E_USER       test user (default x2gotest)
#   X2GO_E2E_PASSWORD   its password (default: a throwaway test password)
#   X2GO_E2E_PORT_BASE  if set, X2Go session ports start here instead of at
#                       a random port from 30000 (for shared machines)
#
# Nothing outside the work folder is changed except: the packages, the test
# user and its home, /run/sshd, and (with X2GO_E2E_PORT_BASE) the helper
# /usr/lib/x2go/x2gogetrandomport, whose original is kept as .dist.
set -euo pipefail

WORK=${X2GO_E2E_DIR:-/tmp/casement-x2go-e2e}
SSH_PORT=${X2GO_E2E_PORT:-2322}
TEST_USER=${X2GO_E2E_USER:-x2gotest}
TEST_PASS=${X2GO_E2E_PASSWORD:-x2go-e2e-Pw-2322}
KEY_PASS=x2go-e2e-key-passphrase
PORT_BASE=${X2GO_E2E_PORT_BASE:-}
SSHD_DIR=$WORK/sshd
KEYS=$WORK/keys
PACKAGES=(x2goserver x2goserver-x2gokdrive xserver-x2gokdrive twm xterm xsel x11-xserver-utils iproute2
    openssh-server)

log() { printf '[x2go-server] %s\n' "$*" >&2; }

alive() {  # pidfile expected-substring
    local pid
    [ -f "$1" ] || return 1
    pid=$(cat "$1" 2>/dev/null) || return 1
    [ -n "$pid" ] && [ -r "/proc/$pid/cmdline" ] || return 1
    tr '\0' ' ' <"/proc/$pid/cmdline" | grep -q -- "$2"
}

apt_install() {
    # Other jobs may use apt at the same time; the lock serialises them.
    if command -v flock >/dev/null; then
        flock /tmp/apt.lock apt-get install -y --no-install-recommends "$@"
    else
        apt-get install -y --no-install-recommends "$@"
    fi
}

ensure_packages() {
    local missing=()
    for p in "${PACKAGES[@]}"; do
        dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
    done
    [ ${#missing[@]} -eq 0 ] && return
    log "installing: ${missing[*]}"
    export DEBIAN_FRONTEND=noninteractive
    if ! apt_install "${missing[@]}"; then
        log "updating the package lists and trying again"
        apt-get update
        apt_install "${missing[@]}"
    fi
}

# x2gostartagent takes its ports from x2gogetrandomport's start value
# (30000 + $RANDOM). On a machine where other jobs own port ranges, pin it.
pin_agent_ports() {
    [ -n "$PORT_BASE" ] || return 0
    local f=/usr/lib/x2go/x2gogetrandomport
    if ! grep -q "casement-x2go-test $PORT_BASE" "$f"; then
        [ -f "$f.dist" ] || cp -p "$f" "$f.dist"
        cat >"$f" <<EOF
#!/bin/bash
# casement-x2go-test $PORT_BASE: start port pinned for the end-to-end tests
# (original in x2gogetrandomport.dist).
printf '%d\n' $PORT_BASE
EOF
        chmod 755 "$f"
        log "X2Go session ports now start at $PORT_BASE"
    fi
}

ensure_user() {
    if ! id "$TEST_USER" >/dev/null 2>&1; then
        useradd -m -s /bin/bash "$TEST_USER"
        log "created user $TEST_USER"
    fi
    echo "$TEST_USER:$TEST_PASS" | chpasswd
    mkdir -p "$KEYS"
    [ -f "$KEYS/id_ed25519" ] || ssh-keygen -q -t ed25519 -N '' -C "$TEST_USER-e2e" -f "$KEYS/id_ed25519"
    [ -f "$KEYS/id_ed25519_pass" ] ||
        ssh-keygen -q -t ed25519 -N "$KEY_PASS" -C "$TEST_USER-e2e-passphrase" -f "$KEYS/id_ed25519_pass"
    local home
    home=$(getent passwd "$TEST_USER" | cut -d: -f6)
    install -d -m 700 -o "$TEST_USER" -g "$TEST_USER" "$home/.ssh"
    cat "$KEYS/id_ed25519.pub" "$KEYS/id_ed25519_pass.pub" >"$home/.ssh/authorized_keys"
    chown "$TEST_USER:$TEST_USER" "$home/.ssh/authorized_keys"
    chmod 600 "$home/.ssh/authorized_keys"
    # The test desktop: a window manager and one terminal on a known
    # background colour, started by x2goruncommand as a custom command.
    install -d -m 755 -o "$TEST_USER" -g "$TEST_USER" "$home/bin"
    cat >"$home/bin/x2go-test-desktop" <<'EOF'
#!/bin/sh
# Desktop for the KDrive test sessions: a window manager and one terminal.
xsetroot -solid '#336699' 2>/dev/null || true
xterm -geometry 80x24+40+40 -title x2go-test-xterm &
exec twm 2>>"$HOME/twm.log"
EOF
    chmod 755 "$home/bin/x2go-test-desktop"
    chown "$TEST_USER:$TEST_USER" "$home/bin/x2go-test-desktop"
    # x2gokdrive only has the built-in "fixed" and "cursor" fonts, and twm
    # exits when its Helvetica fontset is missing: use fixed everywhere.
    sed -e 's/^\(TitleFont\|ResizeFont\|MenuFont\|IconFont\|IconManagerFont\) .*/\1 "fixed"/' \
        /etc/X11/twm/system.twmrc >"$home/.twmrc"
    chown "$TEST_USER:$TEST_USER" "$home/.twmrc"
}

ensure_db() {
    # The x2goserver postinst creates the session database; make sure.
    if ! python3 -I - <<'EOF'
import sqlite3, sys
try:
    c = sqlite3.connect("file:/var/lib/x2go/x2go_sessions?mode=ro", uri=True)
    c.execute("select count(*) from sessions").fetchone()
except Exception:
    sys.exit(1)
EOF
    then
        log "creating the X2Go session database"
        x2godbadmin --createdb
    fi
}

write_sshd_config() {
    mkdir -p "$SSHD_DIR"
    chmod 700 "$SSHD_DIR"
    for t in ed25519 rsa ecdsa; do
        [ -f "$SSHD_DIR/ssh_host_${t}_key" ] || ssh-keygen -q -t "$t" -N '' -f "$SSHD_DIR/ssh_host_${t}_key"
    done
    cat >"$SSHD_DIR/sshd_config" <<EOF
# Private sshd for the X2Go plugin's end-to-end tests.
Port $SSH_PORT
ListenAddress 127.0.0.1
HostKey $SSHD_DIR/ssh_host_ed25519_key
HostKey $SSHD_DIR/ssh_host_rsa_key
HostKey $SSHD_DIR/ssh_host_ecdsa_key
PidFile $SSHD_DIR/sshd.pid
AllowUsers $TEST_USER
PermitRootLogin no
AuthorizedKeysFile .ssh/authorized_keys
PubkeyAuthentication yes
PasswordAuthentication yes
KbdInteractiveAuthentication yes
UsePAM yes
AllowTcpForwarding yes
X11Forwarding no
PrintMotd no
LogLevel VERBOSE
Subsystem sftp internal-sftp
EOF
    # known_hosts for the tests, with every host key, as ssh writes it for
    # a server on another port than 22.
    : >"$KEYS/known_hosts"
    for t in ed25519 rsa ecdsa; do
        local k
        k=$(cut -d' ' -f1,2 "$SSHD_DIR/ssh_host_${t}_key.pub")
        printf '[127.0.0.1]:%s %s\n[localhost]:%s %s\n' "$SSH_PORT" "$k" "$SSH_PORT" "$k" >>"$KEYS/known_hosts"
    done
    # The tests run as the user who called sudo.
    if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
        chown -R "$SUDO_USER" "$KEYS"
    fi
}

start_sshd() {
    write_sshd_config
    if alive "$SSHD_DIR/sshd.pid" "$SSHD_DIR/sshd_config"; then
        log "sshd already running (pid $(cat "$SSHD_DIR/sshd.pid"))"
        return
    fi
    rm -f "$SSHD_DIR/sshd.pid"
    mkdir -p /run/sshd
    chmod 755 /run/sshd
    /usr/sbin/sshd -t -f "$SSHD_DIR/sshd_config"
    /usr/sbin/sshd -f "$SSHD_DIR/sshd_config" -E "$SSHD_DIR/sshd.log"
    for _ in $(seq 50); do
        alive "$SSHD_DIR/sshd.pid" "$SSHD_DIR/sshd_config" && break
        sleep 0.1
    done
    alive "$SSHD_DIR/sshd.pid" "$SSHD_DIR/sshd_config" || {
        log "sshd didn't start; see $SSHD_DIR/sshd.log"
        exit 1
    }
    log "sshd started on 127.0.0.1:$SSH_PORT (pid $(cat "$SSHD_DIR/sshd.pid"))"
}

ssh_test() {  # command...
    ssh -p "$SSH_PORT" -i "$KEYS/id_ed25519" -o IdentitiesOnly=yes -o BatchMode=yes \
        -o UserKnownHostsFile="$KEYS/known_hosts" -o StrictHostKeyChecking=yes \
        "$TEST_USER@127.0.0.1" "$@"
}

# Ends the test user's sessions and removes what finished sessions leave:
# their database rows (x2gocleansessions' job, if it isn't running) and
# their folders.
clean_sessions() {
    local line sid
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        sid=$(cut -d'|' -f2 <<<"$line")
        log "terminating session $sid"
        su - "$TEST_USER" -c "x2goterminate-session '$sid'" >/dev/null 2>&1 || true
    done < <(su - "$TEST_USER" -c x2golistsessions 2>/dev/null || true)
    # x2golistsessions_sql purges finished sessions while it prints, so its
    # whole output is read; a second pass confirms.
    local listing
    for _ in 1 2 3 4 5; do
        listing=$(/usr/lib/x2go/x2golistsessions_sql "$(hostname)" 2>/dev/null || true)
        grep -q '|F|' <<<"$listing" || break
        sleep 0.5
    done
    local live dir home
    live=$(su - "$TEST_USER" -c x2golistsessions 2>/dev/null | cut -d'|' -f2 || true)
    home=$(getent passwd "$TEST_USER" | cut -d: -f6)
    for dir in /tmp/.x2go-"$TEST_USER"/C-* "$home"/.x2go/C-*; do
        [ -e "$dir" ] || [ -L "$dir" ] || continue
        grep -qxF -- "${dir##*/C-}" <<<"$live" || rm -rf -- "$dir"
    done
}

status() {
    if alive "$SSHD_DIR/sshd.pid" "$SSHD_DIR/sshd_config"; then
        echo "sshd: running, pid $(cat "$SSHD_DIR/sshd.pid"), 127.0.0.1:$SSH_PORT"
    else
        echo "sshd: stopped"
    fi
    echo "sessions of $TEST_USER:"
    su - "$TEST_USER" -c x2golistsessions 2>/dev/null | sed 's/^/  /' || true
}

print_env() {
    local home
    home=$(getent passwd "$TEST_USER" | cut -d: -f6)
    cat <<EOF
export X2GO_E2E_HOST=127.0.0.1
export X2GO_E2E_PORT=$SSH_PORT
export X2GO_E2E_USER=$TEST_USER
export X2GO_E2E_PASSWORD=$TEST_PASS
export X2GO_E2E_KEY=$KEYS/id_ed25519
export X2GO_E2E_KEY_ENCRYPTED=$KEYS/id_ed25519_pass
export X2GO_E2E_KEY_PASSPHRASE=$KEY_PASS
export X2GO_E2E_KNOWN_HOSTS=$KEYS/known_hosts
export X2GO_E2E_COMMAND=$home/bin/x2go-test-desktop
export X2GO_E2E_RSA_FINGERPRINT=$(ssh-keygen -lf "$SSHD_DIR/ssh_host_rsa_key.pub" | cut -d' ' -f2)
EOF
}

cmd=${1:-start}
case $cmd in
    start)
        [ "$(id -u)" = 0 ] || { log "run as root (sudo)"; exit 1; }
        ensure_packages
        pin_agent_ports
        ensure_db
        ensure_user
        start_sshd
        ssh_test 'echo "ssh sign-in works for $USER"' >&2
        clean_sessions
        ;;
    env) print_env ;;
    status) status ;;
    clean) clean_sessions ;;
    stop)
        clean_sessions
        if alive "$SSHD_DIR/sshd.pid" "$SSHD_DIR/sshd_config"; then
            kill "$(cat "$SSHD_DIR/sshd.pid")"
            log "sshd stopped"
        fi
        ;;
    *)
        echo "usage: $0 start|env|status|clean|stop" >&2
        exit 2
        ;;
esac
