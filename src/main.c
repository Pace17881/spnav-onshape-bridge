#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "tls.h"
#include "http.h"
#include "ws.h"
#include "wamp.h"
#include "origins.h"
#include "controller.h"
#include "spnav_bridge.h"

#define DEFAULT_HOST "127.51.68.120"
#define DEFAULT_PORT 8181
#define MAX_CLIENTS 8
#define BUF_SIZE (64 * 1024)
#define NLPROXY_VERSION "1.4.8.21486"
/* Onshape sends command icon images (~374 KB observed), as either a single
 * frame or a fragmented message. Bound both forms to the same 1 MiB limit;
 * the receive buffer also needs room for the largest masked frame header. */
#define MSG_BUF_SIZE (1024 * 1024)
#define INPUT_BUF_SIZE (MSG_BUF_SIZE + 14)

#define IO_TIMEOUT 10

enum client_phase { PHASE_TLS, PHASE_HTTP, PHASE_WS, PHASE_CLOSING };

struct client {
	int used;
	int fd;
	SSL *ssl;
	enum client_phase phase;
	struct controller *ctrl;
	unsigned char inbuf[INPUT_BUF_SIZE];
	size_t inlen;
	char peer[64];
	unsigned char outbuf[BUF_SIZE * 2];
	size_t outlen, write_len;
	short tls_wait, write_wait, read_wait;
	time_t accepted_at, write_started;
	int failed;

	unsigned char msgbuf[MSG_BUF_SIZE];
	size_t msglen;
	int msg_opcode; /* -1 if no fragmented message is currently in progress */
};

static struct client clients[MAX_CLIENTS];
static volatile sig_atomic_t g_running = 1;
static float g_sensitivity = CONTROLLER_DEFAULT_SENSITIVITY;

static void on_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

static char *default_state_dir(void)
{
	static char path[512];
	const char *systemd_state = getenv("STATE_DIRECTORY");
	const char *xdg = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");

	/* When run under systemd with StateDirectory= configured, systemd
	 * creates this directory itself (before setting up the sandbox, unlike
	 * ReadWritePaths=) and exports its path here - see systemd.exec(5). */
	if(systemd_state && *systemd_state) {
		snprintf(path, sizeof path, "%s", systemd_state);
		return path;
	}

	if(xdg && *xdg) {
		snprintf(path, sizeof path, "%s/spnav-onshape-bridge", xdg);
	} else if(home && *home) {
		snprintf(path, sizeof path, "%s/.local/share/spnav-onshape-bridge", home);
	} else {
		snprintf(path, sizeof path, "/tmp/spnav-onshape-bridge");
	}
	return path;
}

static void close_client(struct client *cl)
{
	if(cl->ctrl) {
		controller_destroy(cl->ctrl);
		cl->ctrl = NULL;
	}
	if(cl->ssl) {
		/* Never wait for the peer during teardown. */
		SSL_free(cl->ssl);
		cl->ssl = NULL;
	}
	if(cl->fd >= 0) {
		close(cl->fd);
	}
	memset(cl, 0, sizeof *cl);
	cl->fd = -1;
}

static time_t monotonic_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

static void queue_output(struct client *cl, const void *data, size_t len)
{
	if(cl->failed)
		return;
	if(len > sizeof cl->outbuf - cl->outlen) {
		fprintf(stderr, "outgoing queue full for %s, closing\n", cl->peer);
		cl->failed = 1;
		return;
	}
	if(!cl->outlen)
		cl->write_started = monotonic_seconds();
	memcpy(cl->outbuf + cl->outlen, data, len);
	cl->outlen += len;
}

/* Keep the buffer address and attempted length unchanged across WANT retries. */
static void flush_output(struct client *cl)
{
	if(!cl->outlen || cl->failed)
		return;
	if(!cl->write_len)
		cl->write_len = cl->outlen;
	ERR_clear_error();
	int n = SSL_write(cl->ssl, cl->outbuf, (int)cl->write_len);
	if(n > 0) {
		cl->outlen -= (size_t)n;
		memmove(cl->outbuf, cl->outbuf + n, cl->outlen);
		cl->write_len = 0;
		cl->write_wait = 0;
		cl->write_started = monotonic_seconds();
	} else {
		int err = SSL_get_error(cl->ssl, n);
		if(err == SSL_ERROR_WANT_READ)
			cl->write_wait = POLLIN;
		else if(err == SSL_ERROR_WANT_WRITE)
			cl->write_wait = POLLOUT;
		else
			cl->failed = 1;
	}
}

static void client_send(void *user, const char *json, size_t len)
{
	struct client *cl = user;
	unsigned char frame[BUF_SIZE];
	int n = ws_encode_frame(frame, sizeof frame, WS_OP_TEXT, json, len);
	if(n < 0) {
		cl->failed = 1;
		return;
	}
	queue_output(cl, frame, (size_t)n);
}

static void send_http_response(struct client *cl, int status, const char *status_text,
							   const char *extra_headers, const char *body)
{
	char hdr[1024];
	size_t body_len = body ? strlen(body) : 0;
	int n = snprintf(hdr, sizeof hdr,
					 "HTTP/1.1 %d %s\r\n"
					 "Content-Length: %zu\r\n"
					 "%s"
					 "Connection: close\r\n\r\n",
					 status, status_text, body_len, extra_headers ? extra_headers : "");
	queue_output(cl, hdr, (size_t)n);
	if(body_len) {
		queue_output(cl, body, body_len);
	}
}

static void handle_ws_upgrade(struct client *cl, const struct http_request *req)
{
	char accept[29];

	if(!origin_allowed(req->origin)) {
		fprintf(stderr, "rejecting WebSocket upgrade from disallowed origin '%s' (peer %s)\n",
				req->origin[0] ? req->origin : "(none)", cl->peer);
		send_http_response(cl, 403, "Forbidden", NULL, "origin not allowed");
		cl->phase = PHASE_CLOSING;
		return;
	}
	if(req->ws_key[0] == 0 || ws_compute_accept(req->ws_key, accept) != 0) {
		send_http_response(cl, 400, "Bad Request", NULL, "missing Sec-WebSocket-Key");
		cl->phase = PHASE_CLOSING;
		return;
	}

	{
		char hdr[512];
		int n = snprintf(hdr, sizeof hdr,
						 "HTTP/1.1 101 Switching Protocols\r\n"
						 "Upgrade: websocket\r\n"
						 "Connection: Upgrade\r\n"
						 "Sec-WebSocket-Accept: %s\r\n"
						 "Sec-WebSocket-Protocol: wamp\r\n"
						 "Access-Control-Allow-Private-Network: true\r\n\r\n",
						 accept);
		queue_output(cl, hdr, (size_t)n);
	}

	fprintf(stderr, "client %s upgraded to WebSocket (origin: %s)\n", cl->peer, req->origin);
	cl->phase = PHASE_WS;
	cl->ctrl = controller_create(client_send, cl);
	controller_set_sensitivity(cl->ctrl, g_sensitivity);
}

/* Onshape's own client fetches /3dconnexion/nlproxy via plain XHR/fetch, not
 * a WebSocket upgrade - that path IS subject to the browser's CORS checks
 * (unlike the WS upgrade, which Origin-checking in handle_ws_upgrade covers
 * instead). Without this header the browser gets our 200 response but
 * refuses to let the page read the body, and Onshape silently never
 * proceeds to the WebSocket step. Reflects the origin (rather than "*")
 * because that's the same allow-list already enforced everywhere else. */
static void append_cors_header(const struct http_request *req, char *hdr, size_t hdrsz)
{
	if(origin_allowed(req->origin)) {
		size_t n = strlen(hdr);
		/* Access-Control-Allow-Private-Network: Chrome's Private Network
		 * Access wants this on responses from a private-network target (we
		 * always are one, being bound to 127.0.0.0/8) even though we're not
		 * fully sure yet whether it also gates the WebSocket path or only
		 * plain fetch/XHR - it's harmless to always send. */
		snprintf(hdr + n, hdrsz - n,
				 "Access-Control-Allow-Origin: %s\r\n"
				 "Access-Control-Allow-Private-Network: true\r\n",
				 req->origin);
	}
}

static void handle_http_request(struct client *cl, const struct http_request *req)
{
	if(req->is_ws_upgrade) {
		handle_ws_upgrade(cl, req);
		return;
	}

	if(strcmp(req->method, "OPTIONS") == 0) {
		/* CORS preflight, in case the browser decides to send one */
		char hdr[512] = "";
		append_cors_header(req, hdr, sizeof hdr);
		strncat(hdr,
				"Access-Control-Allow-Methods: GET, OPTIONS\r\nAccess-Control-Allow-Headers: *\r\n",
				sizeof hdr - strlen(hdr) - 1);
		send_http_response(cl, 204, "No Content", hdr, "");
	} else if(strcmp(req->path, "/3dconnexion/nlproxy") == 0) {
		char body[128];
		char hdr[512] = "Content-Type: application/json\r\n";
		append_cors_header(req, hdr, sizeof hdr);
		snprintf(body, sizeof body, "{\"port\": %d, \"version\": \"%s\"}", DEFAULT_PORT,
				 NLPROXY_VERSION);
		send_http_response(cl, 200, "OK", hdr, body);
	} else {
		send_http_response(cl, 404, "Not Found", NULL, "");
	}
	cl->phase = PHASE_CLOSING;
}

/* Consumes as many complete HTTP requests / WS frames as are currently
 * buffered in cl->inbuf, compacting the buffer as it goes. */
static void process_client_buffer(struct client *cl)
{
	for(;;) {
		if(cl->phase == PHASE_HTTP) {
			struct http_request req;
			int n = http_parse_request((char *)cl->inbuf, cl->inlen, &req);
			if(n == 0)
				return; /* need more data */
			if(n < 0) {
				cl->phase = PHASE_CLOSING;
				return;
			}
			handle_http_request(cl, &req);
			memmove(cl->inbuf, cl->inbuf + n, cl->inlen - (size_t)n);
			cl->inlen -= (size_t)n;
			if(cl->phase == PHASE_CLOSING)
				return;
			/* PHASE_WS: fall through and try to parse any pipelined frame data */
		} else if(cl->phase == PHASE_WS) {
			int fin, opcode;
			unsigned char *payload;
			size_t payload_len;
			int n = ws_decode_frame(cl->inbuf, cl->inlen, &fin, &opcode, &payload, &payload_len);
			if(n == 0)
				return;
			if(n < 0) {
				fprintf(stderr,
						"malformed/unsupported WS frame from %s (%zu bytes buffered, first bytes: "
						"%02x %02x %02x %02x), closing\n",
						cl->peer, cl->inlen, cl->inlen > 0 ? cl->inbuf[0] : 0,
						cl->inlen > 1 ? cl->inbuf[1] : 0, cl->inlen > 2 ? cl->inbuf[2] : 0,
						cl->inlen > 3 ? cl->inbuf[3] : 0);
				cl->phase = PHASE_CLOSING;
				return;
			}

			switch(opcode) {
			case WS_OP_TEXT:
			case WS_OP_BIN:
			case WS_OP_CONT: {
				/* WS_OP_CONT continues whichever message type a preceding
				 * non-final frame started; a fresh TEXT/BIN frame (fin or
				 * not) starts a new one (reassembly buffer reset to it). */
				if(opcode != WS_OP_CONT) {
					cl->msg_opcode = opcode;
					cl->msglen = 0;
				}
				if(cl->msg_opcode == WS_OP_TEXT) {
					if(cl->msglen + payload_len > MSG_BUF_SIZE) {
						fprintf(stderr, "message from %s exceeds %d byte limit, closing\n",
								cl->peer, MSG_BUF_SIZE);
						cl->phase = PHASE_CLOSING;
						return;
					}
					memcpy(cl->msgbuf + cl->msglen, payload, payload_len);
					cl->msglen += payload_len;
				}
				if(fin && cl->msg_opcode == WS_OP_TEXT) {
					/* capped: logging the full ~20-30KB command-tree message via
					 * fprintf/journald on every single (re)connect measurably
					 * blocks this single-threaded, blocking-I/O event loop */
					fprintf(stderr, "<- from %s: %.*s%s\n", cl->peer,
							(int)(cl->msglen > 200 ? 200 : cl->msglen), cl->msgbuf,
							cl->msglen > 200 ? "...[truncated]" : "");
					if(controller_on_message(cl->ctrl, (char *)cl->msgbuf, cl->msglen) != 0) {
						fprintf(stderr, "protocol violation from %s, closing\n", cl->peer);
						cl->phase = PHASE_CLOSING;
					}
				}
				if(fin) {
					cl->msg_opcode = -1;
					cl->msglen = 0;
				}
				break;
			}
			case WS_OP_PING: {
				unsigned char frame[BUF_SIZE];
				int fn = ws_encode_frame(frame, sizeof frame, WS_OP_PONG, payload, payload_len);
				if(fn > 0)
					queue_output(cl, frame, (size_t)fn);
				break;
			}
			case WS_OP_CLOSE: {
				int code = payload_len >= 2 ? (payload[0] << 8 | payload[1]) : -1;
				fprintf(stderr, "client %s sent WS close, code=%d reason='%.*s'\n", cl->peer, code,
						payload_len > 2 ? (int)payload_len - 2 : 0,
						payload_len > 2 ? (char *)payload + 2 : "");
				cl->phase = PHASE_CLOSING;
				break;
			}
			default:
				break;
			}

			memmove(cl->inbuf, cl->inbuf + n, cl->inlen - (size_t)n);
			cl->inlen -= (size_t)n;
			if(cl->phase == PHASE_CLOSING)
				return;
		} else {
			return;
		}
	}
}

static struct client *find_free_slot(void)
{
	int i;
	for(i = 0; i < MAX_CLIENTS; i++) {
		if(!clients[i].used)
			return &clients[i];
	}
	return NULL;
}

int main(int argc, char **argv)
{
	const char *host = DEFAULT_HOST;
	int port = DEFAULT_PORT;
	char *state_dir = default_state_dir();
	int i;

	for(i = 1; i < argc; i++) {
		if(strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
			host = argv[++i];
		} else if(strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
			port = atoi(argv[++i]);
		} else if(strcmp(argv[i], "--sensitivity") == 0 && i + 1 < argc) {
			char *end;
			const char *value = argv[++i];
			errno = 0;
			g_sensitivity = strtof(value, &end);
			if(errno || end == value || *end || !isfinite(g_sensitivity) ||
					g_sensitivity < 0.01f || g_sensitivity > 10.0f) {
				fprintf(stderr, "--sensitivity must be a number from 0.01 to 10\n");
				return 1;
			}
		} else if(strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) {
			state_dir = argv[++i];
		} else if(strcmp(argv[i], "--help") == 0) {
			printf("usage: %s [--host IP] [--port N] [--state-dir DIR] [--sensitivity FACTOR]\n", argv[0]);
			printf("  --sensitivity: motion speed multiplier (default %.2f; 1 = original speed)\n",
					(double)CONTROLLER_DEFAULT_SENSITIVITY);
			return 0;
		} else {
			fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
			return 1;
		}
	}

	srand((unsigned)(time(NULL) ^ getpid()));
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	SSL_library_init();
	SSL_load_error_strings();

	char cert_path[512];
	if(tls_ensure_certs(state_dir, host, cert_path, sizeof cert_path) != 0) {
		return 1;
	}
	SSL_CTX *ctx = tls_make_server_ctx(state_dir);
	if(!ctx) {
		return 1;
	}

	if(sb_open() != 0) {
		fprintf(stderr, "failed to connect to spacenavd - is it running?\n");
		return 1;
	}

	int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if(inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		fprintf(stderr, "invalid --host '%s'\n", host);
		return 1;
	}
	/* Refuse to bind anywhere but loopback: this bridge deliberately has no
	 * authentication beyond Origin-checking + TLS, and must never be
	 * reachable from the network. */
	if((ntohl(addr.sin_addr.s_addr) >> 24) != 127) {
		fprintf(stderr, "refusing to bind to non-loopback address %s\n", host);
		return 1;
	}

	if(bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		fprintf(stderr, "bind %s:%d failed: %s\n", host, port, strerror(errno));
		return 1;
	}
	listen(lfd, 8);

	for(i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;

	fprintf(stderr, "spnav-onshape-bridge listening on https://%s:%d\n", host, port);
	fprintf(stderr, "certificate (import once, see contrib/nss-trust-install.sh): %s\n", cert_path);

	while(g_running) {
		struct pollfd pfds[2 + MAX_CLIENTS];
		int nfds = 0;
		int poll_timeout = 1000;
		int listen_idx, spnav_idx;
		int client_idx[MAX_CLIENTS];

		pfds[nfds].fd = lfd;
		pfds[nfds].events = POLLIN;
		listen_idx = nfds++;

		pfds[nfds].fd = sb_fd();
		pfds[nfds].events = POLLIN;
		spnav_idx = nfds++;

		for(i = 0; i < MAX_CLIENTS; i++) {
			if(clients[i].used) {
				if((clients[i].phase == PHASE_HTTP || clients[i].phase == PHASE_WS) &&
				   !clients[i].write_len && SSL_pending(clients[i].ssl))
					poll_timeout = 0;
				pfds[nfds].fd = clients[i].fd;
				struct client *cl = &clients[i];
				if(cl->phase == PHASE_TLS) {
					pfds[nfds].events = cl->tls_wait;
				} else if(cl->write_len) {
					pfds[nfds].events = cl->write_wait;
				} else {
					pfds[nfds].events = cl->phase == PHASE_CLOSING ? 0 :
						(cl->read_wait ? cl->read_wait : POLLIN);
					if(cl->outlen) pfds[nfds].events |= POLLOUT;
				}
				client_idx[nfds] = i;
				nfds++;
			}
		}

		int rc = poll(pfds, (nfds_t)nfds, poll_timeout);
		if(rc < 0) {
			if(errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		if(pfds[listen_idx].revents & POLLIN) {
			struct sockaddr_in peer;
			socklen_t plen = sizeof peer;
			int cfd = accept4(lfd, (struct sockaddr *)&peer, &plen, SOCK_NONBLOCK | SOCK_CLOEXEC);
			if(cfd >= 0) {
				struct client *cl = find_free_slot();
				if(!cl) {
					fprintf(stderr, "too many concurrent connections, rejecting\n");
					close(cfd);
				} else {
					int nodelay = 1;
					setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
					cl->used = 1;
					cl->fd = cfd;
					cl->phase = PHASE_TLS;
					cl->tls_wait = POLLIN;
					cl->accepted_at = monotonic_seconds();
					cl->msg_opcode = -1;
					cl->msglen = 0;
					snprintf(cl->peer, sizeof cl->peer, "%s:%d", inet_ntoa(peer.sin_addr),
							 ntohs(peer.sin_port));
					cl->ssl = SSL_new(ctx);
					if(!cl->ssl || SSL_set_fd(cl->ssl, cfd) != 1)
						close_client(cl);
				}
			}
		}

		if(pfds[spnav_idx].revents & POLLIN) {
			struct sb_event ev;
			int r;
			while((r = sb_poll_event(&ev)) > 0) {
				for(i = 0; i < MAX_CLIENTS; i++) {
					if(clients[i].used && clients[i].ctrl) {
						controller_on_spnav_event(clients[i].ctrl, &ev);
					}
				}
			}
			if(r < 0) {
				fprintf(stderr, "lost connection to spacenavd, exiting\n");
				break;
			}
		}

		for(int k = 2; k < nfds; k++) {
			struct client *cl = &clients[client_idx[k]];
			if(!cl->used)
				continue;
			short events = pfds[k].revents;
			time_t now = monotonic_seconds();
			if((events & (POLLHUP | POLLERR | POLLNVAL)) ||
			   ((cl->phase == PHASE_TLS || cl->phase == PHASE_HTTP) &&
				now - cl->accepted_at >= IO_TIMEOUT) ||
			   (cl->outlen && now - cl->write_started >= IO_TIMEOUT))
				cl->failed = 1;

			if(!cl->failed && cl->phase == PHASE_TLS && (events & cl->tls_wait)) {
				ERR_clear_error();
				int n = SSL_accept(cl->ssl);
				if(n == 1)
					cl->phase = PHASE_HTTP;
				else {
					int err = SSL_get_error(cl->ssl, n);
					if(err == SSL_ERROR_WANT_READ)
						cl->tls_wait = POLLIN;
					else if(err == SSL_ERROR_WANT_WRITE)
						cl->tls_wait = POLLOUT;
					else
						cl->failed = 1;
				}
			}
			if(!cl->failed && cl->phase != PHASE_TLS) {
				if(cl->outlen && cl->read_wait != POLLOUT &&
				   (!cl->write_wait || (events & cl->write_wait)))
					flush_output(cl);
				/* A pending SSL_write must be retried before another TLS operation. */
				if(!cl->write_len && cl->phase != PHASE_CLOSING &&
				   ((events & (cl->read_wait ? cl->read_wait : POLLIN)) || SSL_pending(cl->ssl))) {
					/* Bound work per client, while draining already decrypted TLS records. */
					for(int reads = 0; reads < 16; reads++) {
						if(cl->inlen == sizeof cl->inbuf) {
							fprintf(stderr, "input frame from %s exceeds %d byte message limit, closing\n", cl->peer, MSG_BUF_SIZE);
							cl->failed = 1;
							break;
						}
						ERR_clear_error();
						int n =
							SSL_read(cl->ssl, cl->inbuf + cl->inlen, (int)(sizeof cl->inbuf - cl->inlen));
						if(n <= 0) {
							int err = SSL_get_error(cl->ssl, n);
							if(err == SSL_ERROR_WANT_READ)
								cl->read_wait = POLLIN;
							else if(err == SSL_ERROR_WANT_WRITE)
								cl->read_wait = POLLOUT;
							else
								cl->failed = 1;
							break;
						}
						cl->read_wait = 0;
						cl->inlen += (size_t)n;
						process_client_buffer(cl);
						if(cl->phase == PHASE_CLOSING || cl->failed || !SSL_pending(cl->ssl))
							break;
					}
				}
			}
			if(cl->failed || (cl->phase == PHASE_CLOSING && !cl->outlen))
				close_client(cl);
		}
	}

	for(i = 0; i < MAX_CLIENTS; i++)
		if(clients[i].used)
			close_client(&clients[i]);
	sb_close();
	SSL_CTX_free(ctx);
	close(lfd);
	return 0;
}
