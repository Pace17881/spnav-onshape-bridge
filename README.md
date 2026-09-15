# spnav-onshape-bridge

<!-- This repo has no public home yet - replace <owner>/spnav-onshape-bridge
     below once it's pushed somewhere, otherwise these badges 404. -->
![build](https://github.com/<owner>/spnav-onshape-bridge/actions/workflows/build.yml/badge.svg)
![packaging](https://github.com/<owner>/spnav-onshape-bridge/actions/workflows/packaging.yml/badge.svg)

A small, self-contained C daemon that lets [Onshape](https://www.onshape.com/)
(and any other web app using the same 3Dconnexion browser API) use a
[spacenavd](https://github.com/FreeSpacenav/spacenavd)-driven 3D mouse on
Linux, in both Chromium and Firefox.

The current bridge has been tested interactively with Onshape in Chromium
153 and Firefox 155. It accepts the large command-icon messages that caused
the earlier reconnect loop. Motion sensitivity defaults to 0.35 and is
adjustable with `--sensitivity`. See [BROWSER_TEST.md](BROWSER_TEST.md) for
the test results and remaining limitations.

## Why this exists instead of using spacenav-ws

Onshape's browser client only talks to a local, proprietary WebSocket API
provided by 3Dconnexion's Windows driver - there's no Linux equivalent from
3Dconnexion, and spacenavd doesn't implement this protocol (see
[spacenavd#30](https://github.com/FreeSpacenav/spacenavd/issues/30)). The
existing community solution, [spacenav-ws](https://github.com/RmStorm/spacenav-ws),
reverse-engineered that protocol and works, but has three structural security
problems this project fixes:

| Problem in spacenav-ws | Fix here |
|---|---|
| No `Origin` check on the WebSocket upgrade - any website open in the same browser can connect to the well-known local address and read raw 3D-mouse motion or spoof camera-control RPCs | Every upgrade request is checked against a server-side allow-list (`src/origins.c`) before the WAMP handshake proceeds; anything else is rejected with `403` |
| The TLS private key is committed to spacenav-ws's public git repo and reused by every installation, so the usual "add a security exception" click provides no real guarantee | A local CA + leaf certificate are generated fresh per installation (`src/tls.c`), and can be properly imported into your browsers' trust stores (`contrib/nss-trust-install.sh`) instead of clicking through a warning |
| `uvx ...@latest` re-fetches unpinned code from PyPI on every start, and the Tampermonkey script is live-loaded from Greasyfork, where anyone with edit access can change it | Compiled, versioned binary; no runtime code fetching. The one-line browser patch is vendored in this repo (`browser-extension/`) |

The protocol logic itself (the WAMP-over-WebSocket handshake and the
mouse-to-camera-matrix math) was reverse-engineered by the spacenav-ws
authors, not by this project - see `controller.c`'s comments for the mapping
back to their `controller.py`/`wamp.py`.

## Architecture

```
spacenavd  --(libspnav, AF_UNIX)-->  spnav-onshape-bridge  --(wss://127.51.68.120:8181, Origin-checked)-->  Onshape tab
```

- `src/spnav_bridge.*` - thin libspnav wrapper for reading motion/button events.
- `src/tls.*` - per-install CA + leaf certificate generation and the server `SSL_CTX`.
- `src/http.*` - minimal HTTP/1.1 parsing for the `/3dconnexion/nlproxy` discovery endpoint and the WebSocket upgrade request.
- `src/ws.*` - RFC6455 handshake and frame codec.
- `src/origins.*` - the `Origin` allow-list check.
- `src/wamp.*` / `src/controller.*` - the WAMP-v1-like message framing and the per-connection handshake/RPC state machine that turns spacenavd motion events into `view.affine` updates.
- `src/mat4.*` - the 4x4 matrix math (rotation/translation/pivot), using Gram-Schmidt re-orthonormalization instead of SVD to avoid a linear-algebra library dependency.
- `third_party/cJSON.*` - vendored, pinned (v1.7.18) JSON library.

## Building

```sh
./configure
make
make test        # matrix, controller and WebSocket regression tests
make test-integration # TLS/HTTP tests with a simulated device connection (Python 3)
make install      # installs to $PREFIX/bin (default /usr/local), or run from ./spnav-onshape-bridge directly
```

Requires `libspnav` (built/installed from this project's sibling
[libspnav](https://github.com/FreeSpacenav/libspnav) repo) and OpenSSL
development headers.

## Installing a package instead

Debian/Ubuntu (`debian/`) and Arch Linux (`packaging/arch/`) packaging is
available and validated (builds and installs cleanly in CI on every push -
see `.github/workflows/packaging.yml`), though neither has been submitted to
an actual distribution archive/AUR yet. See [packaging/README.md](packaging/README.md)
for build instructions and exactly what's still missing for that. If you
just want to try it, `dpkg-buildpackage`/`makepkg` locally is quicker than
the manual build below and installs the same guided `--doctor` command.

## Setup

The `install.sh` script automates steps 1-4 below (build, install the
binary, generate+import the certificate, enable the systemd service) and
prints exactly what's left to do by hand for the browser side. Run
`spnav-onshape-bridge --doctor` at any point to check what's still missing.

1. Make sure `spacenavd` is running and sees your device (`spnavcfg`, or
   check its log).
2. Run `spnav-onshape-bridge` once by hand. On first run it generates a local
   CA and a leaf certificate under `$XDG_DATA_HOME/spnav-onshape-bridge` (or
   `~/.local/share/spnav-onshape-bridge`) and prints the CA's path.
3. `contrib/nss-trust-install.sh` imports the certificate into NSS stores
   (requires your distro's `certutil` tool). This imports the daemon's local
   CA with real CA trust and needs no further manual certificate steps in
   either browser - verified with a real headless Firefox test; see
   [BROWSER_TEST.md](BROWSER_TEST.md) for why an earlier version of this
   project needed a manual per-site exception in Firefox and no longer does.
   Restart browsers after trust changes.
4. Install it as a per-user systemd service so it starts with your session:
   ```sh
   mkdir -p ~/.local/bin && cp spnav-onshape-bridge ~/.local/bin/
   mkdir -p ~/.config/systemd/user && cp contrib/systemd/spnav-onshape-bridge.service ~/.config/systemd/user/
   systemctl --user daemon-reload
   systemctl --user enable --now spnav-onshape-bridge
   ```
5. **Chromium/Chrome:** open `chrome://extensions`, enable "Developer mode",
   "Load unpacked", select the `browser-extension/` folder.
6. **Firefox:** see "Known limitation" below - for now, install a userscript
   manager (e.g. Tampermonkey) and add `browser-extension/onshape-3d-mouse-linux.user.js`
   from this repo (not from Greasyfork).
7. Open an Onshape document and move the mouse.

## Mouse sensitivity

The default motion sensitivity is now 0.35 (35% of the original bridge
speed), applied to rotation, translation and zoom. Adjust it at startup:

```sh
./spnav-onshape-bridge --sensitivity 0.2   # gentler
./spnav-onshape-bridge --sensitivity 0.5   # faster than the new default
./spnav-onshape-bridge --sensitivity 1     # original speed
```

Accepted values are 0.01 to 10. This setting belongs to the bridge and
therefore applies equally in Firefox and Chromium. Button-triggered view
resets are unaffected. The new default is a starting point for tuning,
not a calibration to FreeCAD.

## Known limitation: Firefox extension

Firefox requires Mozilla to sign any extension that's installed permanently;
an unsigned one only loads temporarily (`about:debugging`) until the browser
restarts. So for now, Firefox uses a userscript manager instead of a native
extension (step 6 above) - the vendored script is version-pinned in this
repo, unlike the live Greasyfork version spacenav-ws documents.

A cleaner long-term option: sign the `browser-extension/` folder once via
Mozilla's free "unlisted" self-distribution signing
(`web-ext sign`, requires a free AMO account) and install the resulting
`.xpi` permanently. That's a manual, external step this project can't
automate.

## Security model

- Client TLS and socket I/O is nonblocking. Incomplete handshakes/HTTP requests
  and stalled writes time out after 10 seconds; outgoing queues are bounded.
  Incoming messages are limited to 1 MiB, including fragmented messages, to
  accommodate Onshape’s command icon uploads.
- The daemon refuses to bind anywhere but `127.0.0.0/8` (checked in code, not
  just by convention).
- Every WebSocket upgrade is rejected unless its `Origin` header exactly
  matches the allow-list in `src/origins.c` (currently just
  `https://cad.onshape.com`).
- Only clients identifying themselves as `"Onshape"` or `"WebThreeJS Sample"`
  during the handshake are ever sent mouse events (defense in depth on top of
  the Origin check).
- TLS keys are generated locally per installation, never shared or committed
  anywhere, and stored `0600` under a `0700` directory.
- The systemd user unit additionally sandboxes the process (see
  `contrib/systemd/spnav-onshape-bridge.service`): no new privileges, no
  filesystem access outside its state directory, no address families beyond
  `AF_INET`/`AF_UNIX`, and no non-loopback IP traffic.

## Contributing, security, changes

- [CONTRIBUTING.md](CONTRIBUTING.md) - code style, build/test workflow, what
  a security-relevant PR should include.
- [SECURITY.md](SECURITY.md) - how to report a vulnerability privately.
- [CHANGELOG.md](CHANGELOG.md) - what changed in each release.
- [POSTMORTEM.md](POSTMORTEM.md) / [BROWSER_TEST.md](BROWSER_TEST.md) - the
  debugging history behind the design decisions above, including the ones
  that turned out to be dead ends.

## License

GPLv3 or later (see `COPYING`), matching the spacenav project this is meant
to eventually integrate with.
