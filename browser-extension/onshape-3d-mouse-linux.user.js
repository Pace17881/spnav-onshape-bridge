// ==UserScript==
// @name         Onshape 3D-Mouse on Linux (spnav-onshape-bridge)
// @description  Vendored, version-pinned copy for Firefox (which requires
//               Mozilla signing for permanently-installed extensions) - see
//               ../README.md "Known limitation: Firefox extension". Chromium
//               users should use the browser-extension/ folder instead of
//               this userscript. Identical logic to content.js.
// @match        https://cad.onshape.com/documents/*
// @run-at       document-start
// @grant        none
// @version      0.1.0
// @license      MIT
// ==/UserScript==

Object.defineProperty(Navigator.prototype, 'platform', { get: () => 'Win32' });
console.log('[spnav-onshape-bridge] navigator.platform ->', navigator.platform);
