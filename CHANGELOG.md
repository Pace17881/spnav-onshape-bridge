# Changelog

All notable changes to this project are documented here.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [0.1.3] - 2026-09-15

### Added
- `man/spnav-onshape-bridge.1`, installed by `make install` for the manual,
  Debian, and Arch install paths alike.

### Fixed
- `install.sh` waited for `$STATE_DIR/server.crt.pem` to appear before
  continuing, a filename left over from before the 0.1.1 revert back to the
  CA+leaf certificate model - the daemon has generated `ca.crt.pem` since
  then, so every fresh manual install would silently time out at this step.
- Arch packaging (`packaging/arch/PKGBUILD`) listed `libx11` as a runtime
  `depends`, but the binary never actually links against `libX11.so` -
  `libspnav`'s header only pulls in `X11/Xlib.h` at compile time when
  `libspnav` itself was built with X11 support. Moved to `makedepends`.
- Debian packaging's `postinst` triggered lintian's
  `maintainer-script-calls-systemctl` check (false positive: it only prints
  the commands, never runs them) - documented and suppressed via
  `debian/spnav-onshape-bridge.lintian-overrides`.

### Verified
- Built both packages in clean containers and ran the distros' own
  packaging-QA tools against the result: `lintian --pedantic` (Debian) is
  now completely clean, and `namcap` (Arch) has no remaining findings
  beyond well-known, benign noise for this kind of package (documented in
  `packaging/README.md`).

## [0.1.2] - 2026-09-15

### Added
- `make lint` (clang-tidy + cppcheck), `make shellcheck` (configure,
  install.sh, contrib/*.sh, debian/*.postinst), and `make test-sanitize`
  (unit + integration tests rebuilt with AddressSanitizer and
  UndefinedBehaviorSanitizer). All three are now also run in CI on every
  push/PR (`.github/workflows/build.yml`).
- `.clang-tidy`: bugprone-*, clang-analyzer-*, cert-*, performance-*,
  portability-* enabled, with a small number of documented exclusions for
  false positives and checks that don't fit this project's style.

### Fixed
- `MSG_BUF_SIZE` was computed as an `int` multiplication (`1024*1024`)
  before being used as a `size_t`; harmless at the current value but a latent
  overflow trap for anyone raising it. Now computed directly as `size_t`,
  with the two `fprintf` format strings that used it corrected from `%d` to
  `%zu`.
- `--port` was parsed with `atoi`, which silently accepts garbage and can't
  distinguish "0" from "not a number". Switched to `strtol` with the same
  range/error checking already used for `--sensitivity`.
- `wamp_gen_id()` used `rand()` seeded from `time()^getpid()` for session/
  call IDs - predictable, and irrelevant entropy for anything
  security-sensitive reusing that function later. Switched to OpenSSL
  `RAND_bytes()`; removed the now-unused `srand()` call in `main.c`.
- `controller_create()` didn't check `calloc()`'s return value; a failed
  allocation would crash on the first field write instead of failing
  cleanly. Now returns NULL on failure, and the one caller
  (`handle_ws_upgrade()` in `main.c`) closes the connection gracefully
  instead of dereferencing NULL.
- All of the above were found by clang-tidy/cppcheck, not by manual review
  or a reported bug - none had been observed causing a real failure.

## [0.1.1] - 2026-09-15

### Fixed
- Restored the local CA + leaf certificate model (reverted the 0.1.0 switch
  to a single directly-trusted self-signed leaf). That switch was made on
  an incorrect theory during the Chromium debugging in POSTMORTEM.md, and
  turned out to make Firefox trust setup *worse*: verified empirically with
  headless Firefox that a peer-trusted self-signed leaf shows the
  certificate warning interstitial, while a CA-trusted leaf loads with no
  interaction in both Firefox and Chromium. `contrib/nss-trust-install.sh`
  now imports with real CA trust (`certutil -t C,,`) again. See the
  BROWSER_TEST.md addendum for the full account.
- Fixed the shipped systemd unit's `ExecStart` (`%h/.local/bin/...`), which
  only worked for the manual `install.sh` install location and not the
  actual `/usr/bin` location a distro package installs to. Now a bare
  command name resolved via an explicit `PATH` covering both.
- `--doctor` only checked the manual-install certificate location
  (`XDG_DATA_HOME`), so it reported the certificate missing even when the
  systemd `--user` service (which uses `XDG_STATE_HOME` via
  `StateDirectory=`) had already generated one correctly. Now checks both,
  matching `nss-trust-install.sh`.

Both packaging bugs above were only caught by installing the actual built
package on a real machine, not by the container-based CI validation added
in 0.1.0 - the containers never exercised systemd or non-root browser
profiles.

### Versioning note
Starting with this release, tags are immutable once created - no more
force-moving `v0.1.0` to fold in fixes, which is exactly what happened
several times while stabilizing that release. Each further change gets a
new tag following semantic versioning.

## [0.1.0] - 2026-09-15

Initial working release.

### Added
- C daemon bridging spacenavd motion/button events to Onshape's WAMP-over-
  WebSocket 3Dconnexion API, as a security-hardened alternative to
  [spacenav-ws](https://github.com/RmStorm/spacenav-ws): Origin allow-list on
  every WebSocket upgrade, per-install self-signed TLS certificate (never
  shared/committed), no runtime code fetching.
- Chromium extension and Firefox userscript (`browser-extension/`) providing
  the one-line `navigator.platform` patch Onshape needs to probe for a local
  driver on Linux.
- `contrib/nss-trust-install.sh` for importing the certificate into browser
  trust stores; `contrib/systemd/` for a per-user systemd service.
- `--doctor`: self-diagnosis for spacenavd connectivity, certificate
  presence, and browser trust-store status.
- `--sensitivity`: motion speed multiplier (default 0.35, tuned from user
  feedback that the original 1:1 mapping felt too fast).
- Debian packaging (`debian/`) and Arch Linux packaging (`packaging/arch/`),
  both validated by building and installing in clean containers.
- Compiler/linker hardening by default: stack protector, `_FORTIFY_SOURCE=2`,
  full RELRO, PIE.
- Unit tests (`make test`) for the matrix math, the WAMP controller state
  machine, and WebSocket frame handling; a Python-based integration test
  (`make test-integration`) for the TLS/HTTP handshake layer.

### Fixed
- WebSocket message-size limit was too small (256 KiB) for Onshape's ~374 KB
  command-icon upload, causing the daemon to silently drop the connection
  without sending a close frame - which browsers then reported as a generic
  "abnormal closure" (code 1006), indistinguishable from a network/browser-
  policy failure. This sent debugging in several wrong directions (browser
  security flags, TLS trust model) before the actual cause was found; see
  [POSTMORTEM.md](POSTMORTEM.md) and [BROWSER_TEST.md](BROWSER_TEST.md) for
  the full account. Fixed by raising the limit to 1 MiB and switching to
  nonblocking I/O with bounded output queues and write timeouts.
- WebSocket frame fragmentation (RFC6455 continuation frames) wasn't
  supported at all; real browsers fragment large outgoing messages.

### Known limitations
- Long-running connection stability is not fully proven: one unexplained
  single disconnect+reconnect was observed after ~160s in testing (see
  BROWSER_TEST.md). The persistent reconnect loop from the 0.0.x debugging
  phase is resolved; this is a separate, much rarer, unresolved event.
- Firefox does not accept the certificate via NSS peer-trust (`certutil -t
  P,,`) import alone in current testing; a host+fingerprint exception was
  needed in the one successful test. See BROWSER_TEST.md and the setup
  instructions in README.md.
- No native, permanently-installable Firefox extension yet (requires Mozilla
  signing); Firefox uses a userscript manager instead.
- Debian and Arch packages are validated to build and install cleanly but
  have not been submitted to either distribution's actual package archive/AUR.
