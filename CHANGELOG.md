# Changelog

All notable changes to this project are documented here.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [0.1.12] - 2026-09-15

### Fixed
- `.github/workflows/packaging.yml`'s `arch-package` job, third fix in this
  chain: 0.1.11 got past the missing-`.git` problem, but then failed with
  `fatal: invalid reference: v0.1.11` - `actions/checkout@v4`'s default
  shallow clone doesn't fetch tags at all, but the PKGBUILD's
  `source=` still pinned `#tag=v${pkgver}` even after the URL was
  redirected to `git+file://$GITHUB_WORKSPACE`. Added a second `sed` step
  stripping the `#tag=...` fragment entirely, so it clones
  `$GITHUB_WORKSPACE`'s checked-out HEAD instead of a specific (and, in
  CI, absent) tag. Verified with a full local reproduction matching the
  real job's shallow/no-tags checkout before pushing this time, rather
  than after.
- `debian-package` job's 0.1.10 fix confirmed working (first fully green
  job in this workflow since publishing).

## [0.1.11] - 2026-09-15

### Fixed
- `.github/workflows/packaging.yml`'s `arch-package` job still failed after
  0.1.10's fix ("does not appear to be a git repository") - the real cause
  was different from what that fix addressed: `archlinux:base-devel` has no
  `git` preinstalled, and `actions/checkout@v4` running before it was
  installed silently falls back to a git-less tarball download, leaving no
  `.git` directory for the `git+file://` PKGBUILD source to clone from
  afterward. Fixed by installing `git` in a step *before* `actions/checkout`
  instead of after. (0.1.10's `safe.directory` fix addressed a different
  problem that only showed up in this project's own local Docker
  reproduction, not the real failure - left in place since it's harmless,
  but it wasn't the actual fix.)

## [0.1.10] - 2026-09-15

### Fixed
- `.github/workflows/packaging.yml`, found by the very first real run on
  GitHub Actions after publishing (never caught by local container testing,
  which didn't exactly match the runner environment):
  - `arch-package`: git (>= 2.35.2) refused to let the `builder` user clone
    `$GITHUB_WORKSPACE` after the workflow `chown`s it to `builder`,
    since `builder` has no gitconfig of its own yet ("detected dubious
    ownership in repository"). Fixed with
    `git config --system --add safe.directory "*"` before switching users.
  - `debian-package`: `actions/upload-artifact@v4` rejects path patterns
    containing `..`, which `path: ../spnav-onshape-bridge_*.deb` used
    (`dpkg-buildpackage` puts its output one directory up). Fixed by moving
    the built `.deb` into a `dist/` folder inside the workspace first.

## [0.1.9] - 2026-09-15

### Added
- Full GPLv3 license headers in every `src/*.c`/`src/*.h` file, matching
  spacenavd's own per-file convention (this project previously relied on
  `COPYING` alone).
- `configure` now requires OpenSSL >= 3.0 and fails with a clear message
  otherwise. This project is GPLv3, which is compatible with OpenSSL's
  Apache-2.0 license from 3.0 on; older OpenSSL's own dual OpenSSL/SSLeay
  license is not GPL-compatible without an explicit linking exception this
  project doesn't have.
- Named `libspnav`'s license (modified BSD-3-Clause) next to its mention in
  README.md, and added a `Comment:` field to `debian/copyright` noting the
  licenses of both dynamically-linked, non-bundled dependencies
  (libspnav, OpenSSL).

Prompted by an audit of the project's own and third-party licensing ahead
of publication: confirmed compatible license chain throughout (this
project GPLv3; libspnav modified BSD-3-Clause; spacenavd and spacenav-ws,
credited for the reverse-engineered protocol, both GPLv3 themselves; cJSON
MIT, vendored and already attributed; OpenSSL Apache-2.0 from 3.0 on) -
no incompatibilities found, but the file-header and OpenSSL-version gaps
above were real and are now closed.

## [0.1.8] - 2026-09-15

### Changed
- Gated the per-message WAMP logging line (`<- from ...`) behind a new
  `--verbose` flag, off by default. It used to run unconditionally, several
  times a second at normal motion rates - too noisy for the journal to
  leave on permanently in day-to-day use, though still valuable when
  actually debugging a protocol issue (as it was during the original
  connection-drop investigation - see POSTMORTEM.md). Covered by a new
  `tests/test_robustness.py` case asserting the log line is absent by
  default and present with `--verbose`.

## [0.1.7] - 2026-09-15

### Fixed
- `browser-extension/content.js` still had a "TEMPORARY diagnostic" patch
  that replaced `window.WebSocket` globally on the whole Onshape page
  (`all_frames: true`, `world: MAIN`) - a leftover from the original
  connection-drop debugging (see POSTMORTEM.md) that was never removed,
  despite that same file's comment claiming "one property override, on one
  site, nothing else". Removed. It only ever existed in the Chromium
  extension, not the Firefox userscript.
- The `navigator.platform` spoof itself (both `content.js` and the Firefox
  userscript) applied to every frame on the page, not just the top-level
  document, via `all_frames: true` / Tampermonkey's frame-injection
  default. There's no reason a nested iframe needs to see a spoofed
  platform - only the top-level document drives the 3D-mouse connection -
  and reported after upgrading to this version: a spoofed platform in a
  hidden iframe (e.g. one Onshape might use for export/thumbnail
  rendering) could plausibly make Onshape's own code pick a
  Windows-assuming path (such as a specific GPU adapter request) that
  doesn't correspond to reality on Linux, breaking unrelated features like
  model export. Restricted to the top-level frame only
  (`all_frames: false` in `manifest.json`, `@noframes` in the userscript).
  Not yet independently confirmed as the fix for the reported export
  failure - report back after reloading the extension/updating the
  userscript and retrying export.

## [0.1.6] - 2026-09-15

### Fixed
- **Stack buffer overflow in the main event loop** (`src/main.c`): the
  per-iteration `client_idx[]` array, used to map poll() results back to
  client slots, was sized `MAX_CLIENTS` (8) but indexed the same way as
  `pfds[]` - offset by 2 for the listener and spacenavd fds - so it needed
  room for `2 + MAX_CLIENTS` entries. With 7 or 8 simultaneously connected
  clients (a realistic scenario, e.g. several browser tabs), the last one
  or two writes landed past the end of the array, corrupting adjacent stack
  memory. Confirmed with AddressSanitizer: reverting the fix reliably
  reproduces a `stack-buffer-overflow` abort pointing at the exact write;
  with it, `make test-sanitize` and the new multi-client integration test
  below are both clean. Found by the new `tests/test_robustness.py`
  MAX_CLIENTS test, not by manual testing or a user report - the existing
  suite never held more than 1-2 connections open simultaneously.

### Added
- `make coverage`: line coverage per `src/*.c` file via `gcov`, run after
  the full `make test`/`make test-integration` suite.
- `tests/test_cli.py`: black-box tests for `--port`/`--sensitivity`
  argument validation, `--help`, and `--doctor`, run against
  `tests/test_daemon` so results don't depend on whether a real spacenavd
  happens to be running on the machine executing the tests.
- `tests/test_robustness.py` (the overflow above, plus): spacenavd motion
  events now fan out correctly to every simultaneously subscribed client
  (previously untested - `tests/stub_spnav.c` only ever returned "no
  event"), and an oversized single WebSocket frame is rejected without
  crashing or hanging the daemon for other clients.
- `tests/stub_spnav.c` can now inject a synthetic spacenavd event from
  outside the test process via a `SPNAV_TEST_EVENTS` FIFO, for the fan-out
  test above.
- `tests/wstest.py`: shared TLS/WS/WAMP test helpers factored out for the
  two new test files (`tests/test_transport.py` is left untouched, using
  its own pre-existing, already-verified helpers).

Line coverage across `src/*.c` went from 71.9% to 79.1% with this change;
see `make coverage`'s per-file breakdown. The remaining gaps are
`src/spnav_bridge.c` (0% - the real libspnav wrapper is replaced by a stub
in every automated test; only ever exercised manually against real
hardware) and parts of `src/main.c`'s `--doctor` subsystem (drives real
subprocesses and inspects real system state, not easily mocked).

## [0.1.5] - 2026-09-15

### Changed
- Translated `POSTMORTEM.md` and `BROWSER_TEST.md` from German to English
  ahead of publishing - both are linked from README.md, CONTRIBUTING.md,
  CHANGELOG.md and `src/tls.c`'s comments as the record behind several
  design decisions (the CA+leaf certificate model, the 1 MiB message
  limit, the 0.35 sensitivity default), so they needed to be readable by
  the same audience as the rest of the project. Content is unchanged -
  every number, hypothesis, and the ruled-out-causes table carried over
  exactly.

## [0.1.4] - 2026-09-15

### Added
- A trademark/no-affiliation notice in README.md: the project reimplements
  a small part of Onshape/3Dconnexion's published client-side behavior to
  interoperate with it, and doesn't claim any affiliation.

### Changed
- Replaced every placeholder/upstream-issue URL used as this project's own
  identity with its real repository, ahead of first publication:
  `https://github.com/Pace17881/spnav-onshape-bridge` now appears in
  README.md's badges, `debian/control`'s Homepage, `debian/copyright`'s
  Source, `packaging/arch/PKGBUILD`'s url/source, the systemd unit's
  Documentation=, and `.github/workflows/packaging.yml`'s local-checkout
  substitution. Prose references to the spacenavd#30 upstream discussion
  (README.md, CONTRIBUTING.md) are left as-is - those describe motivation,
  not this project's own identity.

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
