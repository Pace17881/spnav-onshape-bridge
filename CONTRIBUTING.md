# Contributing

Patches, bug reports and testing on other distros/browsers are welcome.

## Before you start

This project intentionally mirrors [spacenavd](https://github.com/FreeSpacenav/spacenavd)'s
and [libspnav](https://github.com/FreeSpacenav/libspnav)'s own conventions
(plain C, minimal dependencies, hand-written `configure`/`Makefile.in`), on
the theory that it might one day be relevant to
[spacenavd#30](https://github.com/FreeSpacenav/spacenavd/issues/30). Please
keep changes in that spirit rather than introducing a different build
system, a dependency manager, or a rewrite in another language.

For anything beyond a small fix, please open an issue first to discuss the
approach - especially for anything touching the TLS/certificate handling,
the Origin allow-list, or the WAMP protocol state machine, since those are
the security-relevant parts (see [SECURITY.md](SECURITY.md)).

## Code style

- K&R-ish brace placement, tabs for indentation (assumed 4 spaces wide), no
  spaces before parentheses in calls/conditionals (`if(x)`, not `if (x)`).
- C99, no external dependencies beyond `libspnav` and OpenSSL - `cJSON` is
  vendored precisely to avoid adding a new one. If you think a new
  dependency is justified, make the case for it in the issue/PR description.
- No comments explaining *what* code does; only *why*, when it's genuinely
  non-obvious (a protocol quirk, a security consideration, a workaround).

## Building and testing

```sh
./configure
make
make test              # unit tests: matrix math, controller state machine, WS framing
make test-integration   # requires python3: TLS/HTTP handshake against a simulated device
make test-sanitize      # rebuilds and re-runs the above under ASan+UBSan
make lint                # requires clang-tidy and cppcheck
make shellcheck          # requires shellcheck; covers configure and contrib/*.sh
make coverage            # requires gcov; line coverage per src/*.c file
```

`make test-integration` also runs `tests/test_cli.py` (argument parsing and
`--doctor`, against `tests/test_daemon` so it doesn't depend on whether a
real spacenavd happens to be running) and `tests/test_robustness.py`
(spacenavd-event fan-out to multiple simultaneous clients, the MAX_CLIENTS
connection limit, and the oversized-single-frame rejection path) -
`tests/wstest.py` has the shared TLS/WS/WAMP helpers those two use.

A change that touches `src/controller.c`, `src/wamp.c`, `src/ws.c`, or
`src/mat4.c` should come with a test in `tests/` exercising it - see the
existing tests for the pattern (`tests/test_controller.c` scripts a fake
Onshape client against the real controller state machine, `tests/test_ws.c`
exercises RFC6455 framing edge cases directly).

Any new C code should pass `make lint` and `make test-sanitize` cleanly -
`.clang-tidy` documents the few checks that are deliberately disabled and
why; don't silence a real finding by adding to that list without discussing
it first. Shell scripts (`configure`, `install.sh`, `contrib/*.sh`,
`debian/*.postinst`) should pass `make shellcheck`.

CI (`.github/workflows/build.yml`) builds, tests, lints and sanitizer-builds
on every push and PR. Please make sure it passes before requesting review.

## Security-sensitive changes

This daemon parses untrusted network input (WebSocket frames, JSON, HTTP
headers) from any local process that can reach its loopback port. For
changes to `src/ws.c`, `src/http.c`, `src/wamp.c`, `src/controller.c`, or
`src/tls.c`:

- Prefer rejecting malformed input outright over trying to interpret it
  leniently.
- Any new fixed-size buffer needs an explicit bound check before writing to
  it - see `MSG_BUF_SIZE`/`INPUT_BUF_SIZE` in `src/main.c` for the existing
  pattern (and see `POSTMORTEM.md`/`BROWSER_TEST.md` for what happens when
  that bound was set wrong - a silent, hard-to-diagnose connection drop
  rather than a crash).
- If you're changing the Origin allow-list logic (`src/origins.c`), explain
  the threat model change in the PR description.

## Reporting bugs

Please include:
- Browser + version (Chromium/Firefox), and whether it happens in both.
- The daemon's stderr/journal output around the time of the issue.
- Whether `spnav-onshape-bridge --doctor` reports anything relevant.

For anything you believe is a security issue rather than a functional bug,
see [SECURITY.md](SECURITY.md) instead - please don't open a public issue
for those.
