/*
spnav-onshape-bridge - bridges spacenavd 3D-mouse events to Onshape's
browser 3Dconnexion API on Linux.
Copyright (C) 2026 Sebastian Polster <polsterseb@protonmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
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
