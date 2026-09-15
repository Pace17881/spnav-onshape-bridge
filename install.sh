#!/bin/sh
# Automates everything that CAN be automated from setup (README.md "Setup"
# steps 1-4): build, install the binary, generate+import the TLS certificate, and
# enable the systemd --user service. Loading the browser extension /
# userscript (steps 5-6) needs browser UI clicks and is printed at the end
# instead.
#
# Safe to re-run.

set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN_DIR="$HOME/.local/bin"
# Matches the systemd unit's StateDirectory=spnav-onshape-bridge, which
# systemd resolves under XDG_STATE_HOME for --user units.
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/spnav-onshape-bridge"
UNIT_DIR="$HOME/.config/systemd/user"

echo "==> Checking prerequisites"

if ! systemctl is-active --quiet spacenavd 2>/dev/null; then
	echo "WARNING: spacenavd doesn't look active (systemctl is-active spacenavd)." >&2
	echo "         The bridge will keep restarting until it is. Continuing anyway." >&2
fi

if ! command -v certutil >/dev/null 2>&1; then
	echo "certutil not found - install your distro's NSS tools package first" >&2
	echo "(Arch: pacman -S nss | Debian/Ubuntu: apt install libnss3-tools | Fedora: dnf install nss-tools)" >&2
	exit 1
fi

echo "==> Building"
cd "$REPO_DIR"
[ -f Makefile ] || ./configure
make

echo "==> Installing binary to $BIN_DIR"
mkdir -p "$BIN_DIR"
cp spnav-onshape-bridge "$BIN_DIR/"

echo "==> Installing systemd --user unit"
mkdir -p "$UNIT_DIR"
cp contrib/systemd/spnav-onshape-bridge.service "$UNIT_DIR/"
systemctl --user daemon-reload
# restart (not just enable --now): picks up unit-file changes immediately
# even if a previous run of this script left it in a restart loop
systemctl --user enable spnav-onshape-bridge
systemctl --user restart spnav-onshape-bridge

echo "==> Waiting for the daemon to generate its local certificate..."
i=0
while [ ! -f "$STATE_DIR/ca.crt.pem" ]; do
	i=$((i + 1))
	if [ "$i" -gt 20 ]; then
		echo "Timed out waiting for $STATE_DIR/ca.crt.pem" >&2
		echo "Check: systemctl --user status spnav-onshape-bridge" >&2
		exit 1
	fi
	sleep 0.5
done
echo "    found: $STATE_DIR/ca.crt.pem"

echo "==> Importing the certificate into Firefox's and Chromium's certificate stores"
./contrib/nss-trust-install.sh

cat <<EOF

==============================================================================
Automated steps done. Two manual steps remain (browser UI, can't be scripted):

  Chromium/Chrome:
    1. Open chrome://extensions
    2. Enable "Developer mode" (top right)
    3. "Load unpacked" -> select: $REPO_DIR/browser-extension/

  Firefox:
    1. Install the Tampermonkey extension, if you don't have it yet:
       https://addons.mozilla.org/en-US/firefox/addon/tampermonkey/
    2. Tampermonkey dashboard -> create a new script -> paste the contents of:
       $REPO_DIR/browser-extension/onshape-3d-mouse-linux.user.js

Then RESTART both browsers (for the freshly imported certificate to take
effect), open an Onshape document, and move the mouse.
==============================================================================
EOF
