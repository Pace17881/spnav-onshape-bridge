#ifndef WS_H_
#define WS_H_

#include <stddef.h>

enum {
	WS_OP_CONT  = 0x0,
	WS_OP_TEXT  = 0x1,
	WS_OP_BIN   = 0x2,
	WS_OP_CLOSE = 0x8,
	WS_OP_PING  = 0x9,
	WS_OP_PONG  = 0xA
};

/* Computes the RFC6455 Sec-WebSocket-Accept value for a given
 * Sec-WebSocket-Key header value. `accept_out` must be at least 29 bytes
 * (28 base64 chars + NUL). Returns 0 on success. */
int ws_compute_accept(const char *client_key, char accept_out[29]);

/* Encodes an unmasked server->client frame (as required by RFC6455 - servers
 * MUST NOT mask). Returns the number of bytes written to buf, or -1 if buf
 * is too small for the frame header + payload. */
int ws_encode_frame(unsigned char *buf, size_t bufsz, int opcode,
		const void *payload, size_t payload_len);

/* Parses one frame out of buf[0..len). The frame must be masked, as
 * required for client->server frames; fragmented messages ARE supported
 * (real browsers fragment large messages, e.g. Onshape's ~20-30KB command
 * tree, into multiple frames) - the caller is responsible for reassembling
 * consecutive frames using *fin and *opcode (a WS_OP_CONT frame continues
 * whatever message type was started by the preceding non-final frame).
 *
 * On success, returns the number of bytes consumed from buf, sets *fin,
 * *opcode, and points *payload at the (unmasked-in-place) payload inside
 * buf, with *payload_len set accordingly.
 * Returns 0 if buf does not yet contain a complete frame (caller should read
 * more and retry). Returns -1 on a malformed/unsupported frame.
 */
int ws_decode_frame(unsigned char *buf, size_t len, int *fin, int *opcode,
		unsigned char **payload, size_t *payload_len);

#endif
