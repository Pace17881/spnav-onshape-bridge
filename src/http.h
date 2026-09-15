#ifndef HTTP_H_
#define HTTP_H_

#include <stddef.h>

#define HTTP_MAX_HEADERS 32

struct http_request {
	char method[8];
	char path[256];
	char origin[256];        /* Origin header value, empty if absent */
	char ws_key[256];        /* Sec-WebSocket-Key header value, empty if absent */
	int is_ws_upgrade;       /* Connection: Upgrade + Upgrade: websocket seen */
};

/* Parses a full HTTP request (headers only, buf must contain the terminating
 * blank line "\r\n\r\n") out of buf[0..len). Returns the number of bytes
 * making up the header block (i.e. where the body/frame data would start)
 * on success, 0 if the headers are not yet complete, -1 on malformed input.
 */
int http_parse_request(const char *buf, size_t len, struct http_request *req);

#endif
