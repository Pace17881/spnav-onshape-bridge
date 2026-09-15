#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "http.h"

static const char *find_header_end(const char *buf, size_t len)
{
	size_t i;
	for(i = 0; i + 3 < len; i++) {
		if(buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
			return buf + i;
		}
	}
	return NULL;
}

static void rstrip(char *s)
{
	size_t n = strlen(s);
	while(n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t')) {
		s[--n] = 0;
	}
}

static const char *skip_ws(const char *s)
{
	while(*s == ' ' || *s == '\t') s++;
	return s;
}

int http_parse_request(const char *buf, size_t len, struct http_request *req)
{
	const char *hdr_end;
	char line[1024];
	const char *p, *lineend;
	int first = 1;
	int upgrade_hdr = 0, connection_upgrade = 0;

	hdr_end = find_header_end(buf, len);
	if(!hdr_end) {
		return 0; /* incomplete */
	}

	memset(req, 0, sizeof *req);

	p = buf;
	while(p < hdr_end) {
		lineend = memchr(p, '\n', (size_t)((hdr_end + 4) - p));
		if(!lineend) {
			break;
		}
		size_t linelen = (size_t)(lineend - p);
		if(linelen >= sizeof line) {
			linelen = sizeof line - 1;
		}
		memcpy(line, p, linelen);
		line[linelen] = 0;
		rstrip(line);
		p = lineend + 1;

		if(first) {
			first = 0;
			if(sscanf(line, "%7s %255s", req->method, req->path) != 2) {
				return -1;
			}
			continue;
		}

		if(line[0] == 0) {
			continue;
		}

		{
			char *colon = strchr(line, ':');
			if(!colon) {
				continue;
			}
			*colon = 0;
			const char *name = line;
			const char *value = skip_ws(colon + 1);

			if(strcasecmp(name, "Origin") == 0) {
				snprintf(req->origin, sizeof req->origin, "%s", value);
			} else if(strcasecmp(name, "Sec-WebSocket-Key") == 0) {
				snprintf(req->ws_key, sizeof req->ws_key, "%s", value);
			} else if(strcasecmp(name, "Upgrade") == 0) {
				if(strcasecmp(value, "websocket") == 0) {
					upgrade_hdr = 1;
				}
			} else if(strcasecmp(name, "Connection") == 0) {
				/* may be a comma-separated list, e.g. "keep-alive, Upgrade" */
				if(strcasestr(value, "upgrade")) {
					connection_upgrade = 1;
				}
			}
		}
	}

	req->is_ws_upgrade = upgrade_hdr && connection_upgrade;
	return (int)((hdr_end - buf) + 4);
}
