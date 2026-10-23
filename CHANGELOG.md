# Changelog

## 0.1.0 (2026-10-23)

- Casement plugin protocol v1 through the shared Casement layer: `hello`,
  `connect`, `state`, shared-memory surfaces with `frame`/`frame_ack`,
  `cursor`, key, pointer and wheel input, clipboard text both ways,
  `resize`. Capabilities: `password`, `clipboard`, `cursor`, `resize`.
- `x2go://[user@]server[:port]` connections with `command`, `layout`,
  `new`, `close`, `hostkey` and `key` (`plugin.json`).
- SSH with libssh2: sign-in with a key file (its passphrase from Casement's
  password), Pageant or the Windows OpenSSH agent, the default keys, then
  password and keyboard-interactive; `auth_failed` for a missing or refused
  password. Host keys checked against a pinned SHA256 fingerprint (any of
  the server's keys) or `known_hosts` (read only; hashed names, patterns,
  `[host]:port`, `@revoked`); unknown and changed keys fail with the
  fingerprint and what to do.
- X2Go KDrive desktop sessions: `x2gostartagent`, `x2golistsessions` and
  `x2goresume-session` for the newest suspended session with the same
  desktop, `x2goruncommand` for the desktop, `x2gosuspend-session` or
  `x2goterminate-session` when the tab closes. A desktop that can't start
  and sessions ended on the server are reported as such.
- The KDrive protocol over a direct-tcpip channel: cookie and version
  handshake, frames with the server's frame cache (main images, screen
  updates, cached frames, region copies, cache hits, DELETED, REINIT,
  recovery from misses), JPEG and PNG through the Windows Imaging
  Component, ARGB and core cursors, clipboard chunks (zlib, CLIPBOARD,
  UTF-8), keepalives, SRVDISCONNECT, GEOMETRY after resizing.
- Keyboard: set-1 scancodes to X keycodes for the server's own XKB layout
  (from `layout=` or the Windows layout), modifiers through the event state
  with explicit releases, Caps Lock and Num Lock as keys followed by a
  round trip, taps of lone modifiers, AltGr with or without Windows' extra
  left Ctrl, Pause, key repeat by the plugin when Casement doesn't send
  repeats.
- Unit tests with messages captured from a real session, end-to-end tests
  against a real X2Go server through `tools/sim`, a script that sets that
  server up (`tools/e2e/x2go-server.sh`), `ci/e2e.sh`, libssh2 in
  `ci/mingw-deps.sh`, vcpkg for MSVC, GitHub Actions for builds, tests and
  releases, an icon of the project's own (`tools/make-icon.py`).
