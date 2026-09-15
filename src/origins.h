#ifndef ORIGINS_H_
#define ORIGINS_H_

/* Returns 1 if `origin` (the exact value of the HTTP Origin header, e.g.
 * "https://cad.onshape.com") is on the allow-list, 0 otherwise.
 *
 * This is the fix for the core vulnerability in spacenav-ws: nothing there
 * checks the Origin of an incoming WebSocket upgrade, so any website open in
 * the same browser can connect to the well-known local bridge address and
 * either read raw 3D-mouse motion or spoof camera-control RPCs. Every
 * upgrade request MUST be checked against this before the WAMP handshake is
 * allowed to proceed.
 */
int origin_allowed(const char *origin);

#endif
