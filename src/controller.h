#ifndef CONTROLLER_H_
#define CONTROLLER_H_

#include <stddef.h>
#include "spnav_bridge.h"

struct controller;

#define CONTROLLER_DEFAULT_SENSITIVITY 0.35f

/* Called by the controller whenever it needs to send a WAMP JSON message
 * to the browser client. The caller (main.c) is responsible for wrapping
 * `json` into a WebSocket text frame and writing it to the socket. */
typedef void (*controller_send_fn)(void *user, const char *json, size_t len);

/* Creates a controller for a freshly accepted, WAMP-subprotocol-negotiated
 * WebSocket connection, and immediately emits the WELCOME message via
 * `send`. */
struct controller *controller_create(controller_send_fn send, void *send_user);
void controller_destroy(struct controller *c);

/* Overall motion gain: 1 restores the original speed; range 0.01..10.
 * Returns -1 for invalid values, leaving the previous setting unchanged. */
int controller_set_sensitivity(struct controller *c, float sensitivity);

/* Feed one complete incoming WAMP JSON text message (already unwrapped from
 * its WS frame). Returns 0 normally, -1 on an unrecoverable protocol
 * violation (caller should close the connection). */
int controller_on_message(struct controller *c, const char *json, size_t len);

/* Feed one spacenavd event. Only has an effect once the client has
 * completed the create-mouse/create-controller handshake, subscribed, and
 * is a recognized client (mirrors spacenav-ws's controller.py). */
void controller_on_spnav_event(struct controller *c, const struct sb_event *ev);

#endif
