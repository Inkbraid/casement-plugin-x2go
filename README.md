# X2Go support for Casement

A protocol plugin for Casement that opens desktop sessions on X2Go servers.
It signs in over SSH, starts or resumes a session that uses X2Go's KDrive
graphical backend, and shows that session's screen in a Casement tab, with
keyboard, mouse and clipboard, all through the same SSH connection.

This is an independent project. It is not made or endorsed by the X2Go
project; "X2Go" names the software it works with.

## Status

| | |
|---|---|
| Casement plugin protocol v1: start, states, surfaces and frame flow control, input, cursor, clipboard, resize | done |
| SSH sign-in: key file (passphrase from Casement's password), Pageant and the Windows OpenSSH agent, default keys, password, keyboard-interactive | done |
| Host keys: `known_hosts` (read only, hashed names, `[host]:port`), pinned SHA256 fingerprints | done |
| KDrive desktop sessions: start, resume the newest suspended one, suspend or end on close | done |
| Picture: JPEG and PNG frames, the frame cache, recovery from cache misses | done |
| Keyboard on the server's own layout, mouse buttons and wheel, cursor shapes | done |
| Clipboard text both ways | done |
| The remote screen follows the tab's size | done |
| Sign-in through Pageant or the Windows OpenSSH agent | implemented; not tested with a real agent yet |
| Sound, file sharing, printing, single applications (rootless), several monitors | not yet |
| NX sessions (`x2goagent`, x2goclient's default) | out of scope |
| Accepting a new host key, two-factor codes, choosing among suspended sessions, progress while a session starts | needs protocol v2 (P2, P1) |

See [CHANGELOG.md](CHANGELOG.md).

## Installing

Download the zip for the latest release from this repository's
[Releases](https://github.com/Inkbraid/casement-plugin-x2go/releases) and
unpack it so that you get

```
%LOCALAPPDATA%\Casement\plugins\
    x2go\
        plugin.json
        icon.png
        casement-x2go.exe
```

then restart Casement. Each release also carries the matching source code
(the tag and GitHub's source archives).

## Connections

```
x2go://[user@]server[:port][?command=…][&layout=…][&resume][&close=suspend|terminate][&hostkey=SHA256:…][&key=…]
```

| Field | In the address | Meaning |
|---|---|---|
| User | `user@` | The SSH user. Without it, your Windows user name, as `ssh` does. |
| Server | host | Name or address of the X2Go server; IPv6 addresses in brackets. |
| SSH port | `:port` | 22 unless given. |
| Desktop | `command=` | What runs in the session, passed to the server's `x2goruncommand`: a desktop name (`XFCE`, `LXDE`, `LXQT`, `MATE`, `KDE`, `GNOME`, `CINNAMON`, `UNITY`, `TRINITY`, `ICEWM`, `OPENBOX`, `TERMINAL`; case doesn't matter, except that `openbox` and `icewm` in lower case run those programs) or a program with arguments, such as `/usr/bin/xterm -bg black`. `XFCE` when empty. Letters, digits, spaces and `. _ / + = : , @ % -` only; anything else belongs in a script on the server. |
| Keyboard layout | `layout=` | The X keyboard layout for the session, with an optional variant: `us`, `de`, `fr(oss)`, `de(nodeadkeys)`. When empty, the layout matching the Windows keyboard layout, or `us`. |
| Resume a suspended session | `resume` | Resume your newest suspended KDrive session that was started with the same desktop, instead of starting a new one. On in the connection dialog. |
| Closing the tab | `close=` | `suspend` (default): the session keeps running on the server and can be resumed. `terminate`: the session ends. |
| Host key fingerprint | `hostkey=` | Trust the server whose host key has this fingerprint, as `ssh-keygen -lf` prints it (`SHA256:…`), instead of looking it up in `known_hosts`. |
| Private key file | `key=` | An OpenSSH or PEM private key; `%VARIABLES%` and a leading `~` are expanded. |

Examples:

```
x2go://ann@build.example.com?command=XFCE&resume
x2go://ann@10.0.0.5:2222?command=%2Fusr%2Fbin%2Fxterm&layout=de&close=terminate
x2go://ann@build.example.com?resume&hostkey=SHA256:Bx%2FoMqjQz9twH07lwPyoDE0AznE98f8ZxuKi6fC8Z5w
```

The password comes from Casement, saved or typed when the plugin reports
that one is needed; the plugin doesn't store it. It is used for password and
keyboard-interactive sign-in, and as the passphrase of a protected key.

## How it works

1. **SSH.** One connection to the server (libssh2). The host key is checked
   (see below), then the plugin signs in: with the key file from `key=` if
   there is one, then with keys from Pageant or the Windows OpenSSH agent,
   then with `id_ed25519`, `id_ecdsa` and `id_rsa` from `%USERPROFILE%\.ssh`,
   then with the password (password, then keyboard-interactive).
2. **The session.** In exec channels on that connection, through a login
   shell as x2goclient does it: with `resume`, `x2golistsessions` and, for
   the newest suspended KDrive session (type `K`) with the same desktop,
   `x2goresume-session`; otherwise `x2gostartagent … K <desktop> …` for a
   new session, at the tab's size if Casement has sent it by then.
3. **The picture.** A direct-tcpip channel on the same connection to
   `localhost:<graphics port>` on the server, where `x2gokdrive` waits. The
   plugin sends the session cookie and speaks the KDrive protocol: frames
   (JPEG and PNG, with the server's frame cache), cursor shapes, clipboard,
   keepalives. For a new session, `x2goruncommand` then starts the desktop.
   Images are decoded with the Windows Imaging Component into Casement's
   surface.
4. **Input.** Casement's set-1 scancodes become X keycodes, which the server
   turns into characters with the session's own XKB layout, so dead keys and
   AltGr work as on the server. Pointer, buttons and wheel go straight
   through. A `resize` from Casement becomes a new screen size 0.4 seconds
   after the last one, so that dragging the window's edge doesn't resize the
   session at every step.
5. **Closing.** Closing the tab runs `x2gosuspend-session` (or, with
   `close=terminate`, `x2goterminate-session`), then disconnects.

What Casement sees as `state`:

| | |
|---|---|
| `connected` | the KDrive connection is up |
| `auth_failed` | no password, or the server didn't accept it |
| `failed` | the server can't be reached, its host key is unknown or doesn't match, X2Go or its KDrive server isn't installed, the session didn't start, … with a sentence saying which |
| `closed` | the session was suspended or ended on the server, or the connection was lost |

## Things to know

- **KDrive only.** The server needs `x2goserver`, `x2goserver-x2gokdrive`
  and `xserver-x2gokdrive` (x2gokdrive 0.0.0.2 or later). Sessions started by x2goclient without "Run in
  X2GoKDrive" are NX sessions; the plugin doesn't resume those, and they
  aren't affected. The server must be a little-endian machine (x86, ARM,
  POWER little-endian), `AllowTcpForwarding` must be on in its sshd, and
  `localhost` must resolve to its IPv4 loopback address (the default).
- **Host keys.** The plugin never asks and never writes: it reads
  `%USERPROFILE%\.ssh\known_hosts`, `known_hosts2` and
  `%ProgramData%\ssh\ssh_known_hosts`, with hashed names and `[host]:port`
  entries as OpenSSH writes them. For an unknown server it fails with the
  server's fingerprint and says how to trust it: add `hostkey=` with that
  fingerprint, or connect once with `ssh.exe`. A host key that differs from
  the one in `known_hosts`, or one marked `@revoked`, always fails with a
  warning. A pinned fingerprint may be that of any of the server's Ed25519,
  ECDSA or RSA keys. The plugin offers the key types `known_hosts` lists for
  the server first, then Ed25519, ECDSA and RSA, like OpenSSH.
- **Sign-in.** Servers allow six attempts by default (`MaxAuthTries`) and
  count every key they refuse, so when Casement has a password the plugin
  offers at most four keys before trying it. Keyboard-interactive prompts
  that ask for a password get it; other prompts (one-time codes) can't be
  answered in protocol v1, and the plugin then fails saying so.
- **Sessions.** Only suspended sessions are resumed: a running session
  belongs to another client. (A running session whose client went away
  suspends itself within 30 seconds and can then be resumed.) Resuming the
  session from elsewhere ends this connection: X2Go's scripts end the SSH
  connection that last started or resumed a session. A session that was
  started but never connected to suspends itself after 30 seconds.
  `resume` is a flag: an address typed by hand resumes only when it has
  `resume` in it.
- **Keyboard.** The server types characters with the layout from `layout=`
  (or the Windows layout at start), not the one Windows uses at the moment.
  Right Alt is AltGr on layouts that have it (most outside the US) and Alt
  on the others; the plugin decides this from the layout name, from a table
  made from xkb-data 2.41, and treats unknown layouts as having AltGr.
  The X2Go server releases every key the moment it is pressed, so held keys
  repeat only if the client sends repeats, and modifiers are held through
  the modifier state that comes with each event.
- **What the protocol doesn't say, and what the plugin assumes:**
  - *AltGr:* Windows puts a left Ctrl press in front of every AltGr press.
    Whether Casement forwards it isn't specified, so the plugin handles both:
    a left Ctrl followed by right Alt within 50 ms is dropped, together with
    its release; otherwise left Ctrl counts. (Ctrl pressed and AltGr within
    50 ms of it therefore gives AltGr alone.)
  - *Key repeat:* if Casement sends repeated key-down messages for a held
    key, those are the repeats. If it doesn't, the plugin repeats the last
    key held down itself, with Windows' repeat delay (plus 150 ms) and rate,
    until the key is released, at most for 30 seconds. The first repeat
    Casement sends switches the plugin's own off for the connection.
  - *Caps Lock and Num Lock* start as Windows reports them to the plugin
    when the connection opens. After that the plugin follows the Caps Lock
    and Num Lock keys Casement forwards. If they get out of step (a lock key
    pressed while Casement didn't have the keyboard), reconnecting brings
    them back in step.
- **Clipboard.** Text only, the CLIPBOARD selection (the one Ctrl+C and
  Ctrl+V use), not PRIMARY (select and middle-click); images on the remote
  clipboard aren't passed on.
- **Picture.** The server sends changes as JPEG (quality 70, lower while it
  is busy) and replaces them with lossless PNG tiles as soon as it is idle.
  It doesn't send the pointer's position, so Casement shows the cursor at
  the local pointer.
- **Not in this version:** sound, file sharing, printing, single
  applications, several monitors, and the UDP frame transport (which
  wouldn't pass through SSH).
- **Logging.** The plugin logs to stderr, which Casement copies into its log;
  `CASEMENT_X2GO_LOG=debug` adds every server command it runs.
  `CASEMENT_X2GO_KNOWN_HOSTS` names a known_hosts file to read instead of the
  user's and the system's; it is meant for tests.

## Building

The deliverable is a Windows x64 program. It is C++20 with CMake, built with
MSVC on Windows and cross-compiled with MinGW-w64 on Linux, where the tests
run under Wine.

### Windows (MSVC)

libssh2 (with OpenSSL and zlib) comes from vcpkg, as `vcpkg.json` lists it,
with the static C runtime, so the program needs nothing else installed:

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build
ctest --test-dir build -L unit
```

The plugin is built for the Windows GUI subsystem, so starting it never
opens a console window; its redirected standard handles work as usual.

### Linux (MinGW-w64)

```
ci/mingw-deps.sh /opt/mingw64 zlib openssl libssh2
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_PREFIX_PATH=/opt/mingw64
cmake --build build
WINEDEBUG=-all ctest --test-dir build -L unit
```

`ci/mingw-deps.sh` builds static zlib, OpenSSL and libssh2 into the prefix,
from downloads checked against pinned SHA-256 sums. `cmake/mingw-w64.cmake`
adds `CMAKE_PREFIX_PATH` to the find roots, so packages in the prefix are
found.

## Testing

- **Unit tests** (`ctest -L unit`): the KDrive protocol against messages
  captured from a real session (frames, cursors, clipboard chunks, the
  events a working client sent), the frame cache and drawing, JPEG and PNG
  decoding with WIC, cursor decoding, clipboard chunking and compression,
  the parsing of x2gostartagent, x2golistsessions and x2goresume-session
  output, session ids, the server commands, connection settings,
  known_hosts matching (plain, patterns, hashed, markers) and fingerprints,
  scancode translation, layouts, and the key event scheme.
- **End-to-end tests** (`ctest -L e2e`) run the plugin through
  `tools/sim`, Casement's side of the protocol, against a real X2Go server:
  password and key sign-in (also a key with a passphrase), a wrong password,
  an unknown host key and pinned keys, the first picture, typing into an
  xterm, a click, the cursor, the clipboard both ways, resizing, suspending
  and resuming, and ending the session on close.
  `tools/e2e/x2go-server.sh` sets up that server on the local machine (the
  packages, a test user with generated keys and a throwaway password, a
  small desktop, its own sshd on port 2322); `ci/e2e.sh` starts it and runs
  the tests:

  ```
  ci/e2e.sh build
  ```

  or by hand:

  ```
  sudo tools/e2e/x2go-server.sh start
  eval "$(sudo tools/e2e/x2go-server.sh env)"
  WINEDEBUG=-all ctest --test-dir build -L e2e
  ```

  Without `X2GO_E2E_HOST` the end-to-end tests are skipped, unless
  `CASEMENT_E2E=1` is set; then a missing server is a failure.
- **By hand:** `casement-sim` plays Casement's side for one connection:

  ```
  casement-sim casement-x2go.exe "x2go://ann@server?command=XFCE&resume" --password … --seconds 20 --out .
  ```

## Protocol v2

These proposals (numbers as in the plugin specs) would remove the v1
workarounds above:

- **P2, prompts:** accept an unknown host key after showing its fingerprint
  (instead of `hostkey=` or a first connection with `ssh.exe`), answer
  two-factor and other keyboard-interactive prompts, choose which suspended
  session to resume, and confirm before ending one.
- **P1, progress text:** starting a session takes a few seconds (sign-in,
  `x2gostartagent`, the desktop); the tab could say which step it is at.
- **P3, data folder:** a known_hosts of the plugin's own, written when the
  user accepts a key, instead of reading the user's.
- **P9, initial size:** start the session at the tab's size; now it starts
  at 1024×768, or at the size of a `resize` that arrived first, and is
  resized when Casement sends one.

Open questions that only Casement's behaviour can settle, and the plugin's
answer for now (see "Things to know"):

1. Does Casement forward the left Ctrl Windows sends before AltGr? (The
   plugin drops it when right Alt follows within 50 ms.)
2. Does Casement forward key repeats? (The plugin repeats held keys itself
   until it sees one.)
3. Can Casement tell the plugin the lock key states, for example with P4
   (focus)? (The plugin reads them once at start.)
4. When does Casement send its first `resize`? (Any time; before the
   session starts it sets the starting size.)

## Licensing

This program is free software under the **GNU General Public License,
version 3 or later** ([LICENSE](LICENSE)), following x2gokdriveclient, the
reference KDrive client.

Third-party components built into the program:

| Component | Version | Licence | Source |
|---|---|---|---|
| libssh2 | 1.11.1 | BSD-3-Clause | https://libssh2.org |
| OpenSSL | 3.5.4 (MinGW build); vcpkg's (MSVC build) | Apache-2.0 | https://www.openssl.org |
| zlib | 1.3.1 (MinGW build); vcpkg's (MSVC build) | Zlib | https://zlib.net |
| nlohmann/json | 3.12.0, vendored in `third_party/nlohmann/` | MIT ([LICENSE.MIT](third_party/nlohmann/LICENSE.MIT)) | https://github.com/nlohmann/json |

Casement itself is a separate program that talks to this plugin only through
the documented protocol in [docs/plugin-protocol.md](docs/plugin-protocol.md);
the plugin's licence does not extend to it. That document is a copy from
[Inkbraid/casement-plugin-protocol](https://github.com/Inkbraid/casement-plugin-protocol),
licensed CC BY 4.0. The icon is the project's own artwork
(`tools/make-icon.py`).

The plugin implements the KDrive protocol as the X2Go sources define it
(x2gokdrive and x2gokdriveclient, both GPL-3.0-or-later); it contains no code
from them.
