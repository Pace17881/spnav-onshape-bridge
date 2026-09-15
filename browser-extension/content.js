// Onshape only probes for a local 3Dconnexion driver
// (https://127.51.68.120:8181/3dconnexion/nlproxy) when navigator.platform
// looks like Windows. spnav-onshape-bridge implements that same local
// endpoint on Linux, so this just needs to get Onshape to ask for it.
//
// This is the entire client-side footprint of this extension: one property
// override, on one site, nothing else. Runs in the page's own main world
// (see manifest.json's "world": "MAIN") so Onshape's own bundled script -
// not just this extension - observes the overridden value. Restricted to
// the top-level frame only (manifest.json's "all_frames": false) - the
// 3D-mouse connection lives in the main document; there's no reason for a
// nested iframe (e.g. one Onshape might use for export/thumbnail
// rendering) to also see a spoofed platform and potentially take a
// different, Windows-assuming code path it wouldn't take on real Linux.
Object.defineProperty(Navigator.prototype, 'platform', { get: () => 'Win32' });
console.log('[spnav-onshape-bridge] navigator.platform ->', navigator.platform);
