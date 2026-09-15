// Onshape only probes for a local 3Dconnexion driver
// (https://127.51.68.120:8181/3dconnexion/nlproxy) when navigator.platform
// looks like Windows. spnav-onshape-bridge implements that same local
// endpoint on Linux, so this just needs to get Onshape to ask for it.
//
// This is the entire client-side footprint of this extension: one property
// override, on one site, nothing else. Runs in the page's own main world
// (see manifest.json's "world": "MAIN") so Onshape's own bundled script -
// not just this extension - observes the overridden value.
Object.defineProperty(Navigator.prototype, 'platform', { get: () => 'Win32' });
console.log('[spnav-onshape-bridge] navigator.platform ->', navigator.platform);

// TEMPORARY diagnostic: log the real close code/reason for every WebSocket
// Onshape opens to our bridge, since DevTools doesn't surface it directly.
// Installed at document_start, before Onshape's own bundle captures its
// WebSocket reference, so (unlike a console-pasted patch after load) this
// actually intercepts every connection. Remove once the mystery disconnect
// is root-caused.
(function() {
	const OrigWS = window.WebSocket;
	function PatchedWS(url, protocols) {
		const ws = protocols === undefined ? new OrigWS(url) : new OrigWS(url, protocols);
		if (typeof url === 'string' && url.includes('127.51.68.120')) {
			console.log('[spnav-monitor] created:', url);
			ws.addEventListener('close', (e) => {
				console.log('[spnav-monitor] CLOSED code=' + e.code + ' reason="' + e.reason + '" wasClean=' + e.wasClean);
			});
			ws.addEventListener('error', (e) => {
				console.log('[spnav-monitor] ERROR', e);
			});
		}
		return ws;
	}
	PatchedWS.prototype = OrigWS.prototype;
	PatchedWS.CONNECTING = OrigWS.CONNECTING;
	PatchedWS.OPEN = OrigWS.OPEN;
	PatchedWS.CLOSING = OrigWS.CLOSING;
	PatchedWS.CLOSED = OrigWS.CLOSED;
	window.WebSocket = PatchedWS;
})();
