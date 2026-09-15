# spnav-onshape-bridge — Postmortem (2026-09-14)

## Current status (2026-09-15)

The persistent reconnect loop is fixed. The user confirmed the feature works
in Chromium and Firefox. The sensitivity setting added afterward,
`--sensitivity 0.35`, was retested in Firefox and rated clearly better; 0.35
is now the default. Build, unit, and integration tests pass. Details and
remaining limitations are in [BROWSER_TEST.md](BROWSER_TEST.md), in
particular the Firefox certificate exception and the single later Chromium
disconnect.

All temporary test processes and test data were stopped/removed at the
user's request. No systemd service and no certificates were installed
permanently. The source code is committed for the first time at this point.
The historical report starting at "Goal" below is kept for traceability.

## Addendum: live diagnosis on 2026-09-15

The text below describes the state at the time. In a renewed test with the
real Onshape application, the disconnect was reproduced and a concrete
trigger was found: after the command tree, Onshape sends a further WAMP
update with command icons in the `images` field, roughly 373,583 bytes in
the observed document. The connection dropped immediately afterward.

The tests done so far, up to about 31 KB, never exercised this case. The
input buffer only held 64 KiB, the reassembly buffer 256 KiB. A single large
frame filled the input buffer before it could be fully decoded, and the
daemon then closed the connection. Even with smaller fragments, the overall
message limit was too low. The browser reported this as 1006.

A new integration test with synthetic icon data reproduced a TCP connection
reset before the fix. There is now a shared, bounded message limit of 1 MiB;
the input buffer additionally has room for the frame header. Tests for
single and fragmented messages of about 374 KB pass. After swapping out only
the temporary test daemon, it accepted the real icon message; the previously
continuous reconnects stopped during the observation period. The user then
confirmed successful testing of the mouse control and closed Chromium. The
test controller stopped the daemon and removed the temporary browser
profile along with its certificate.

This means the earlier statements "protocol correct" and "server ruled out"
were too broad. Likewise, 1006 alone does not prove a server-independent
TLS/browser cause. The comments that named the leaf-certificate switch as
the fix have been corrected.

## Goal
An independent, auditable C daemon as a more secure alternative to
[spacenav-ws](https://github.com/RmStorm/spacenav-ws), to make a SpaceMouse
(via spacenavd) usable in Onshape on Linux (Chromium + Firefox).

**Status: not resolved.** The daemon implements the protocol correctly (see
below), but the WebSocket connection to the browser drops after about 1
second with code `1006` (abnormal closure, no close frame). The cause was
not found despite extensive narrowing-down.

Source code remains at `~/dev/spnav-onshape-bridge` (never committed, but
kept at the user's request). All system installation (service, certificates,
browser trust entries) has been reverted — see "Cleanup" below.

## What reliably works (verified)

- **spacenavd + libspnav**: the SpaceMouse Wireless is detected correctly,
  events arrive cleanly.
- **TLS handshake**: fully functional, verified with `openssl s_client` and
  a real Python WebSocket client (chain validation, IP-SAN match, both
  green).
- **HTTP/CORS layer**: the `/3dconnexion/nlproxy` endpoint returns the
  correct response including `Access-Control-Allow-Origin` — confirmed in a
  real browser (green lock, valid JSON).
- **WebSocket framing including fragmentation**: the RFC6455 handshake and
  frame codec (including reconstructing the fragmentation reassembly for
  Onshape's ~20-30KB "commands" tree message) were tested in isolation and
  work correctly.
- **WAMP protocol logic**: the complete handshake (create mouse/controller,
  subscribe, prefix resolution) as well as the motion-event-to-`view.affine`
  conversion (rotation/translation/pivot, Gram-Schmidt instead of SVD) were
  verified against real, captured Onshape messages — including real
  SpaceMouse motion data that correctly produced sensible camera matrices.
- **Performance**: a simulated client with a real ~31KB message
  (reconstructing the "commands" tree) and ten rapid consecutive requests
  got responses in <1ms. The server sat at 0% CPU usage throughout the test
  phase.
- **Origin allow-list (security fix)**: tested live against both a wrong and
  a correct origin — works as intended (403 for a foreign origin).

In short: everything we could test with our own, non-browser client (Python
`websockets`) works flawlessly — including with realistic message sizes and
patterns.

## The unresolved problem

As soon as a **real browser** (Chromium 153 or Firefox) opens the
connection:

1. The TLS handshake and WAMP handshake complete cleanly.
2. Real data flows correctly for a few hundred milliseconds up to about 1
   second (sometimes several full request/response cycles including real
   mouse motion data).
3. The underlying TCP/TLS connection is then **abruptly disconnected without
   a WebSocket close frame** — confirmed via direct JS instrumentation:
   `CloseEvent.code = 1006`, `reason = ""`, `wasClean = false`.
4. Onshape's own reconnect logic (`window.ab.connect.maxRetries`) kicks in,
   re-establishes the connection, the same short working phase, then
   disconnects again — an infinite loop.
5. Result for the user: jerky, delayed response, increased CPU load (from
   the constant reconnect cycle including retransmitting the large
   command-tree message).

Code 1006 means the disconnect happens **below** the WebSocket protocol
layer (TCP reset or TLS abort), not through a JS-side `ws.close()` decision
and not through a regular WAMP protocol error on our side (no close frame,
no error message server-side).

## Narrowed-down and ruled-out causes

Each of the following hypotheses was concretely tested (not just assumed)
and **ruled out**:

| Hypothesis | Test | Result |
|---|---|---|
| WebSocket frame fragmentation not supported | Found: `first bytes: 01 ff 00 00` (FIN=0) in real logs; RFC6455 reassembly implemented and verified in isolation | Real bug, fixed, but **not** the cause of the 1006 problem |
| Missing `Access-Control-Allow-Origin` (CORS) on `/3dconnexion/nlproxy` | Header added, verified with `curl` | Fixed (Onshape now gets as far as attempting the WebSocket), but doesn't solve the core problem |
| Chrome/Firefox "Local Network Access" (LNA) / "Private Network Access" (PNA), in general | `chrome://flags/#local-network-access-check` → Disabled, Chromium restarted | No change |
| LNA/PNA specifically for WebSockets (`LocalNetworkAccessChecksWebSockets`) | `chromium --disable-features=LocalNetworkAccessChecksWebSockets` | No change |
| Firefox-specific LNA | `network.lna.skip-domains` set to `cad.onshape.com` | No change |
| Missing `Access-Control-Allow-Private-Network` header | Header added to the HTTP and WS upgrade responses | No change |
| Our own server logging slows the event loop | Direct timing test with a simulated client: <1ms response time even for 31KB messages | Ruled out — the server is not the bottleneck |
| Certificate trust model: local CA (more powerful, more "suspicious") vs. a directly pinned leaf certificate (as spacenav-ws uses) | Switched from a CA+leaf hierarchy to a single self-signed leaf certificate, NSS import with `P,,` (peer trust) instead of `C,,` (CA trust) | No change |
| Onshape document permissions (403 error on `/api/v14/documents/.../permissionset`) | Tested with a newly created, guaranteed-own document | No change — user confirmed: not an Onshape/permissions issue |
| Ad blocker/privacy extension | Pi-hole (network-wide DNS blocker on a separate host) tested disabled | No change (irrelevant for loopback traffic anyway) |
| Chrome sandboxing / kernel RC version (`7.3.0-rc2-1-mainline`) interacting with network namespaces | `chromium --no-sandbox` | No change |

## Most promising next step, not carried out

**Run the reference implementation (`uvx spacenav-ws@latest serve`,
Python/FastAPI/Uvicorn) on this exact machine, in this exact browser,
against the same document.**

This A/B test was started (our service stopped, port 8181 freed) but
abandoned at the user's request in favor of cleanup and documentation
instead. It would have clearly split the search in two:

- **spacenav-ws also disconnects with code 1006** → the cause lies with this
  machine/this browser build/this network environment, not with our own
  server code. Not a C-code problem.
- **spacenav-ws runs stably** → there is a real, still-unknown structural
  difference between our minimal C implementation and a mature ASGI/Uvicorn
  WebSocket stack implementation (e.g. HTTP header details, TCP socket
  options, keep-alive behavior, TLS record timing) that could be narrowed
  down specifically.

If this topic is picked up again later, this is clearly the most sensible
first step.

## Cleanup (performed on 2026-09-14)

- systemd `--user` service stopped, disabled, unit file removed
- Binary removed from `~/.local/bin/spnav-onshape-bridge`
- Certificates/keys removed from `~/.local/state/spnav-onshape-bridge`
- Certificate trust entries removed from `~/.pki/nssdb` (Chromium) and both
  Firefox profiles (`99cn02xt.default`, `dhbxwyki.default-release`),
  verified clean

**Still to be done manually by you** (cannot be automated from the
terminal):

- Chromium: `chrome://extensions` → uninstall "SpaceMouse for Onshape"
- Firefox: Tampermonkey → delete the "Onshape 3D-Mouse on Linux" script
- Firefox: `about:config` → reset/clear `network.lna.skip-domains`
- Chromium: `chrome://flags/#local-network-access-check` back to "Default"
  (if still set to "Disabled")
- The source directory `~/dev/spnav-onshape-bridge` is kept at your request;
  delete it yourself later if needed (`rm -rf ~/dev/spnav-onshape-bridge`) —
  never committed, so this is irreversible without git history.

## In the meantime, for Onshape/SpaceMouse

Until (if) this problem is solved, [spacenav-ws](https://github.com/RmStorm/spacenav-ws)
remains the only known working solution for this use case on Linux — with
the security caveats mentioned at the start (see the initial analysis:
missing Origin check on WebSocket upgrades, shared TLS key in the public
repo, unpinned runtime code fetching).
