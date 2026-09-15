# Changelog

All notable changes to this project are documented here.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

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
