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
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include "ws.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

int ws_compute_accept(const char *client_key, char accept_out[29])
{
	char concat[256];
	unsigned char digest[SHA_DIGEST_LENGTH];
	int len;

	len = snprintf(concat, sizeof concat, "%s%s", client_key, WS_GUID);
	if(len < 0 || (size_t)len >= sizeof concat) {
		return -1;
	}

	SHA1((unsigned char*)concat, (size_t)len, digest);

	/* base64 of a 20-byte SHA1 digest is always exactly 28 chars + NUL */
	EVP_EncodeBlock((unsigned char*)accept_out, digest, SHA_DIGEST_LENGTH);
	return 0;
}

int ws_encode_frame(unsigned char *buf, size_t bufsz, int opcode,
		const void *payload, size_t payload_len)
{
	size_t hdrlen;
	unsigned char *p = buf;

	if(payload_len < 126) {
		hdrlen = 2;
	} else if(payload_len <= 0xffff) {
		hdrlen = 4;
	} else {
		hdrlen = 10;
	}

	if(payload_len > INT_MAX - hdrlen || bufsz < hdrlen || payload_len > bufsz - hdrlen) {
		return -1;
	}

	p[0] = 0x80 | (opcode & 0x0f); /* FIN=1, no RSV, opcode */

	if(payload_len < 126) {
		p[1] = (unsigned char)payload_len; /* MASK bit clear: server frame */
	} else if(payload_len <= 0xffff) {
		uint16_t be = htons((uint16_t)payload_len);
		p[1] = 126;
		memcpy(p + 2, &be, 2);
	} else {
		uint32_t hi = (uint32_t)((uint64_t)payload_len >> 32);
		uint32_t lo = (uint32_t)((uint64_t)payload_len & 0xffffffffu);
		uint32_t be_hi = htonl(hi), be_lo = htonl(lo);
		p[1] = 127;
		memcpy(p + 2, &be_hi, 4);
		memcpy(p + 6, &be_lo, 4);
	}

	if(payload_len) {
		memcpy(p + hdrlen, payload, payload_len);
	}
	return (int)(hdrlen + payload_len);
}

int ws_decode_frame(unsigned char *buf, size_t len, int *fin, int *opcode,
		unsigned char **payload, size_t *payload_len)
{
	unsigned char b0, b1;
	int mask;
	uint64_t plen;
	size_t off = 2;
	unsigned char *maskkey;
	size_t i;

	if(len < 2) {
		return 0;
	}
	b0 = buf[0];
	b1 = buf[1];

	if(b0 & 0x70) {
		/* reserved bits set: not supported */
		return -1;
	}
	*fin = (b0 & 0x80) != 0;

	mask = (b1 & 0x80) != 0;
	if(!mask) {
		/* RFC6455: client frames MUST be masked */
		return -1;
	}

	plen = b1 & 0x7f;
	if(plen == 126) {
		uint16_t v;
		if(len < off + 2) return 0;
		memcpy(&v, buf + off, 2);
		plen = ntohs(v);
		off += 2;
	} else if(plen == 127) {
		uint32_t hi, lo;
		if(len < off + 8) return 0;
		memcpy(&hi, buf + off, 4);
		memcpy(&lo, buf + off + 4, 4);
		plen = ((uint64_t)ntohl(hi) << 32) | ntohl(lo);
		off += 8;
	}

	/* Reject invalid 64-bit lengths and values our int return cannot hold. */
	if(plen > INT_MAX - (off + 4)) return -1;
	*opcode = b0 & 0x0f;
	if(*opcode != WS_OP_CONT && *opcode != WS_OP_TEXT && *opcode != WS_OP_BIN &&
			*opcode != WS_OP_CLOSE && *opcode != WS_OP_PING && *opcode != WS_OP_PONG) return -1;
	if((*opcode & 8) && (!*fin || plen > 125)) return -1;

	if(len < off + 4) {
		return 0;
	}
	maskkey = buf + off;
	off += 4;

	if(plen > len - off) {
		return 0;
	}

	for(i = 0; i < plen; i++) {
		buf[off + i] ^= maskkey[i % 4];
	}

	*opcode = b0 & 0x0f;
	*payload = buf + off;
	*payload_len = (size_t)plen;
	return (int)(off + plen);
}
