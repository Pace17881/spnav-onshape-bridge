#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "ws.h"

int main(void)
{
	int fin, op;
	unsigned char *payload;
	size_t len;
	unsigned char overflow[] = {0x81, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
								0xff, 0xff, 0xff, 1,	2,	  3,	4};
	assert(ws_decode_frame(overflow, sizeof overflow, &fin, &op, &payload, &len) == -1);
	unsigned char large[] = {0x81, 0xff, 0, 0, 0, 0, 0x80, 0, 0, 0, 1, 2, 3, 4};
	assert(ws_decode_frame(large, sizeof large, &fin, &op, &payload, &len) == -1);
	unsigned char text[] = {0x81, 0x82, 1, 2, 3, 4, 'h' ^ 1, 'i' ^ 2};
	for(size_t n = 0; n < sizeof text; n++)
		assert(ws_decode_frame(text, n, &fin, &op, &payload, &len) == 0);
	assert(ws_decode_frame(text, sizeof text, &fin, &op, &payload, &len) == sizeof text);
	assert(fin && op == WS_OP_TEXT && len == 2 && !memcmp(payload, "hi", 2));
	unsigned char ping[] = {0x09, 0x80, 1, 2, 3, 4};
	assert(ws_decode_frame(ping, sizeof ping, &fin, &op, &payload, &len) == -1);
	unsigned char buf[16];
	assert(ws_encode_frame(buf, sizeof buf, WS_OP_TEXT, "hi", 2) == 4);
	assert(!memcmp(buf, "\x81\x02hi", 4));
	assert(ws_encode_frame(buf, sizeof buf, WS_OP_TEXT, "hi", SIZE_MAX) == -1);
	char accept[29];
	assert(ws_compute_accept("dGhlIHNhbXBsZSBub25jZQ==", accept) == 0);
	assert(!strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
	return 0;
}
