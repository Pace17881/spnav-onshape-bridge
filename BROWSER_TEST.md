# Browser test on 2026-09-15

## Result

The current daemon kept the WebSocket connection open for 35 seconds in an
isolated Chromium test: 15 seconds with no requests after the WAMP
handshake, then 20 seconds with roughly ten messages per second. No
WebSocket error and no unexpected close event during this period.

At the measurement point: 200 large requests sent, 199 responses received,
WebSocket.readyState = OPEN. The last request was sent immediately before
the snapshot; its response was no longer waited for.

The disconnect after about one second described in POSTMORTEM.md was not
reproduced in this test. This does not yet prove the original Onshape
problem is fixed.

## Setup and limitations

- Chromium 153.0.8010.36 (Arch Linux), headless, fresh temporary profile.
- Current real daemon, started directly, connected to the existing
  spacenavd socket; no systemd service.
- The browser's real TLS/WebSocket stack against 127.51.68.120:8181.
- A test page served locally via DevTools under the origin
  https://cad.onshape.com. The real Onshape application was not loaded.
- Synthetic WAMP handshake: create mouse, create controller (name Onshape),
  prefix and subscribe. Followed by update calls each with 31,000 characters
  of filler data in the commands field. Not a real Onshape command tree.
- No recorded physical mouse movements and no visual check of the camera
  control. Firefox and the reference implementation were not tested.
- Fresh self-signed test certificate. Only the test browser accepted its
  public key via --ignore-certificate-errors-spki-list. Normal certificate
  validation through the browsers' trust stores was therefore not tested.

## Observation regarding browser permissions

Without the right permission, the connection failed before the WAMP
handshake with:

    net::ERR_BLOCKED_BY_LOCAL_NETWORK_ACCESS_CHECKS

The browser reported close code 1006 after a few milliseconds; the daemon
never received a WebSocket upgrade. This is a different failure mode than
the disconnect described in the postmortem, which happened after data was
already flowing successfully.

Browser.setPermission with the name local-network-access was not sufficient
in this test. Using the name loopback-network with setting=granted for the
test origin made the connection work. No LNA checks were disabled via
browser flags.

## System state

No systemd service was installed or activated. No import into existing
Chromium/Firefox certificate stores. No changes to existing browser
profiles. Test daemon and test browser were stopped after the run.
Temporary browser profiles, certificates, and the test script were then
removed.

The first, synthetic test alone did not yet allow any conclusion about the
real Onshape application; the user tests that followed are documented
below.


## Addendum: real Onshape application

In the visible test browser subsequently operated by the user, the
reconnect loop occurred again, with disconnects about 150 ms after the
upgrade. Diagnostics showed an additional `images` message of roughly
374 KB following the smaller command tree. This exceeded both buffer
limits that existed at the time.

The new integration test reproduced the connection reset before the fix.
After raising the bounded message limit to 1 MiB and sizing the frame input
buffer accordingly, the test passes for single and fragmented icon
messages. In the still-open real Onshape document, the replaced test daemon
accepted the icon message; the reconnect loop stopped. The user then
confirmed successful testing of the camera control and closed the test
window.

The visible test has also ended. The test controller stopped the daemon and
removed the temporary browser profile along with its certificate. No
service was installed and no certificate was imported into existing trust
stores.

The diagnostic log still showed a single 1006 disconnect after about 160
seconds and a subsequent reconnect after the fix; its cause was not
separately investigated. The previously continuous reconnect loop had
stopped. The successful user test is therefore not proof of complete
long-term stability.

## Addendum 2026-09-15: a plausible cause for that single disconnect

[KittyCAD/modeling-app#7169](https://github.com/KittyCAD/modeling-app/issues/7169)
independently documents that this same proxy-server role can get "wedged"
if the client ever fails to answer one of the `self:read`/`self:update`
RPCs sent to it - "it may process a few events then just stop." This
project's `src/controller.c` had exactly that gap: no timeout on the RPC
chain, so a single missed client response would silently block every
future motion/button event. Fixed in v0.1.13 with a 10-second chain
timeout. Not confirmed as the actual explanation for the disconnect above
(it was never reproduced on demand to test against), but it is a real,
independently-corroborated failure mode that's now closed off either way.


## Firefox 155.0.1: successful user test

The user also confirmed flawless operation in Firefox. In the visible test,
a single bridge connection was counted with 4,370 messages sent and 4,367
received, with no close event during the measurement. The largest message
sent was 372,216 characters and was processed successfully. Browser and
test daemon were stopped; the temporary profile and test certificate were
removed.

The initial NSS import with `P,,` alone was not sufficient in this Firefox
profile: Firefox reported MOZILLA_PKIX_ERROR_SELF_SIGNED_CERT. For the
successful test, an exception for exactly 127.51.68.120:8181 and the
SHA-256 fingerprint of the test certificate was instead added to
`cert_override.txt` in the temporary profile. `acceptInsecureCerts`
remained disabled; the HTTPS discovery loaded successfully before the
Onshape login. Existing Firefox profiles and system-wide trust stores were
not changed. The previously documented NSS import alone is therefore not a
verified working Firefox setup path.

The user reported the sensitivity was too high across both browsers.
`--sensitivity` was then added and the default set to 0.35. Unit tests
check the change for rotation, translation, and zoom. A further Firefox
test was run with `--sensitivity 0.35`. The user rated the control as
"already much better", confirming the new default as a suitable starting
point.

In this test, one connection was recorded with 5,267 messages sent and
5,264 received, with no close event up to the final measurement. At the
user's request, everything was then stopped: test browser, daemon,
temporary profile, certificate, and diagnostic scripts. There is still no
permanent installation. Further individual tuning is available via
`--sensitivity`.


## Addendum 2026-09-15: cause of the Firefox certificate problem found

The manual host+fingerprint exception documented above as necessary in
Firefox had a concrete, avoidable cause: in the meantime, the project (while
debugging the original Chromium connection drop, see POSTMORTEM.md) had been
switched from a local CA signing a leaf certificate to a single, directly
self-signed leaf certificate — on the (incorrect) assumption that the CA
trust model had caused the Chromium disconnect. In fact, that bug was caused
by a message-size limit that was too small, completely unrelated to the
certificate model.

Empirically verified (each with `firefox --headless --screenshot` against a
fresh, temporary profile):

- Self-signed leaf certificate, imported with NSS peer trust
  (`certutil -t P,,`): Firefox shows the certificate warning, no automatic
  acceptance.
- Leaf certificate signed by a local CA, CA imported with regular CA trust
  (`certutil -t C,,`): Firefox loads the page directly, with no warning or
  manual interaction — likewise in Chromium.

Firefox's certificate validation (mozilla::pkix) apparently does not
reliably support NSS peer trust for self-signed certificates the way the
classic NSS model suggests it should; real CA-chain validation is the
reliably supported path. `src/tls.c` and `contrib/nss-trust-install.sh`
were reverted accordingly (local CA + leaf, import with `C,,`). This
removes the manual exception step for Firefox entirely — confirmed with the
same screenshot procedure against the real, running daemon (a real
generated certificate, the real `nss-trust-install.sh` import command):
`https://127.51.68.120:8181/3dconnexion/nlproxy` loads directly, with no
interstitial.

So the pure trust-model switch (CA vs. peer) never had anything to do with
the Chromium 1006 bug, but very directly with the Firefox experience — two
independent questions that got conflated during debugging.
