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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <poll.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
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
#define MSG_BUF_SIZE ((size_t)1024 * 1024)
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
static int g_verbose = 0;

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

/* ---- `--doctor`: self-diagnosis for the parts of setup that most often go
 * wrong, so a new user isn't left guessing at raw error output. Read-only:
 * makes no changes, only reports and suggests the fix. */

static int doctor_check_spacenavd(void)
{
	printf("[..] spacenavd connection ...");
	fflush(stdout);
	if(sb_open() == 0) {
		sb_close();
		printf(" OK\n");
		return 1;
	}
	printf(" FAILED\n");
	printf("     -> spacenavd isn't reachable. Is it installed and running?\n");
	printf("        (systemctl status spacenavd, or start it: systemctl start spacenavd)\n");
	return 0;
}

/* Whether `name` can be exec'd via $PATH at all, no shell involved (avoids
 * any quoting/injection concern) and regardless of what it then does - NSS's
 * certutil, unlike most tools, exits nonzero even for -H/--help, so this
 * can't just check for a zero exit status like run_quiet() below does. exit
 * code 127 from the child is execvp() itself failing (command not found);
 * anything else means the program was found and ran. */
static int command_exists(const char *name)
{
	pid_t pid = fork();
	if(pid < 0) {
		return 0;
	}
	if(pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);
		if(devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
		}
		char *argv[] = { (char*)name, NULL };
		execvp(name, argv);
		_exit(127);
	}
	int status;
	if(waitpid(pid, &status, 0) != pid) {
		return 0;
	}
	return !(WIFEXITED(status) && WEXITSTATUS(status) == 127);
}

static int doctor_check_certutil(void)
{
	printf("[..] certutil (NSS tools, needed to import the certificate) ...");
	fflush(stdout);
	if(command_exists("certutil")) {
		printf(" OK\n");
		return 1;
	}
	printf(" MISSING\n");
	printf("     -> install your distro's NSS tools package:\n");
	printf("        Arch: pacman -S nss | Debian/Ubuntu: apt install libnss3-tools | Fedora: dnf install nss-tools\n");
	return 0;
}

static int doctor_cert_trusted_in(const char *db, const char *label)
{
	char sqldb[700];
	int found;
	int fds[2];

	snprintf(sqldb, sizeof sqldb, "sql:%s", db);

	if(pipe(fds) != 0) {
		return 0;
	}
	pid_t pid = fork();
	if(pid == 0) {
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		int devnull = open("/dev/null", O_WRONLY);
		if(devnull >= 0) dup2(devnull, STDERR_FILENO);
		close(fds[1]);
		char *argv[] = { "certutil", "-L", "-d", sqldb, NULL };
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);

	found = 0;
	if(pid > 0) {
		char buf[4096];
		ssize_t n;
		while((n = read(fds[0], buf, sizeof buf - 1)) > 0) {
			buf[n] = 0;
			if(strstr(buf, "spnav-onshape-bridge")) {
				found = 1;
			}
		}
		int status;
		waitpid(pid, &status, 0);
	}
	close(fds[0]);

	printf("     %s %s (%s)\n", found ? "[OK]" : "[--]", label, db);
	return found;
}

static int doctor_check_cert(const char *state_dir)
{
	char crt[600];
	const char *home = getenv("HOME");
	const char *xdg_state = getenv("XDG_STATE_HOME");
	int trusted_anywhere = 0;

	/* Mirrors contrib/nss-trust-install.sh: when run under the systemd
	 * --user service, the certificate lives under XDG_STATE_HOME
	 * (StateDirectory=); run by hand in the foreground, it defaults to
	 * `state_dir` (XDG_DATA_HOME) instead - see default_state_dir(). This
	 * command is normally run by hand, so check the systemd location too
	 * rather than only whichever one applies to *this* invocation. */
	if(xdg_state && *xdg_state) {
		snprintf(crt, sizeof crt, "%s/spnav-onshape-bridge/ca.crt.pem", xdg_state);
	} else if(home && *home) {
		snprintf(crt, sizeof crt, "%s/.local/state/spnav-onshape-bridge/ca.crt.pem", home);
	} else {
		crt[0] = 0;
	}
	printf("[..] certificate file ...");
	if(crt[0] == 0 || access(crt, R_OK) != 0) {
		snprintf(crt, sizeof crt, "%s/ca.crt.pem", state_dir);
		if(access(crt, R_OK) != 0) {
			printf(" MISSING (%s)\n", crt);
			printf("     -> run spnav-onshape-bridge once (without --doctor) to generate it\n");
			return 0;
		}
	}
	printf(" OK (%s)\n", crt);

	printf("[..] certificate trusted by browsers:\n");
	if(home && *home) {
		char chrome_db[600];
		snprintf(chrome_db, sizeof chrome_db, "%s/.pki/nssdb", home);
		trusted_anywhere |= doctor_cert_trusted_in(chrome_db, "Chromium/Chrome");

		char ff_root[600];
		snprintf(ff_root, sizeof ff_root, "%s/.mozilla/firefox", home);
		DIR *d = opendir(ff_root);
		if(d) {
			struct dirent *ent;
			while((ent = readdir(d))) {
				if(ent->d_name[0] == '.') continue;
				char profile_db[1024], cert9[1100], label[1100];
				snprintf(profile_db, sizeof profile_db, "%s/%s", ff_root, ent->d_name);
				snprintf(cert9, sizeof cert9, "%s/cert9.db", profile_db);
				if(access(cert9, F_OK) != 0) continue;
				snprintf(label, sizeof label, "Firefox profile %s", ent->d_name);
				trusted_anywhere |= doctor_cert_trusted_in(profile_db, label);
			}
			closedir(d);
		}
	}
	if(!trusted_anywhere) {
		printf("     -> none found. Run: contrib/nss-trust-install.sh\n");
	}
	return trusted_anywhere;
}

static int run_doctor(const char *state_dir)
{
	int ok = 1;

	printf("spnav-onshape-bridge --doctor\n\n");
	ok &= doctor_check_spacenavd();
	ok &= doctor_check_certutil();
	ok &= doctor_check_cert(state_dir);

	printf("\n");
	if(ok) {
		printf("Everything checked out. Remaining manual steps (can't be checked from here):\n");
	} else {
		printf("Some checks failed - see the '->' suggestions above. Once fixed, other\n"
				"remaining manual steps (can't be checked from here):\n");
	}
	printf("  - Chromium/Chrome: load browser-extension/ as an unpacked extension\n");
	printf("    (chrome://extensions -> Developer mode -> Load unpacked)\n");
	printf("  - Firefox: install browser-extension/onshape-3d-mouse-linux.user.js\n");
	printf("    via a userscript manager (e.g. Tampermonkey)\n");
	printf("  - Restart both browsers after any certificate trust change\n");
	return ok ? 0 : 1;
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

	cl->ctrl = controller_create(client_send, cl);
	if(!cl->ctrl) {
		fprintf(stderr, "out of memory creating controller for %s, closing\n", cl->peer);
		cl->phase = PHASE_CLOSING;
		return;
	}
	controller_set_sensitivity(cl->ctrl, g_sensitivity);

	fprintf(stderr, "client %s upgraded to WebSocket (origin: %s)\n", cl->peer, req->origin);
	cl->phase = PHASE_WS;
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
						fprintf(stderr, "message from %s exceeds %zu byte limit, closing\n",
								cl->peer, MSG_BUF_SIZE);
						cl->phase = PHASE_CLOSING;
						return;
					}
					memcpy(cl->msgbuf + cl->msglen, payload, payload_len);
					cl->msglen += payload_len;
				}
				if(fin && cl->msg_opcode == WS_OP_TEXT) {
					/* --verbose only: at normal motion rates this fires several
					 * times a second, which is too noisy for the journal by
					 * default. Even truncated to 200 bytes, logging the full
					 * ~20-30KB command-tree message via fprintf/journald on
					 * every single (re)connect measurably blocks this
					 * single-threaded, blocking-I/O event loop. */
					if(g_verbose) {
						fprintf(stderr, "<- from %s: %.*s%s\n", cl->peer,
								(int)(cl->msglen > 200 ? 200 : cl->msglen), cl->msgbuf,
								cl->msglen > 200 ? "...[truncated]" : "");
					}
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
	int doctor_requested = 0;

	for(i = 1; i < argc; i++) {
		if(strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
			host = argv[++i];
		} else if(strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
			char *end;
			const char *value = argv[++i];
			long p;
			errno = 0;
			p = strtol(value, &end, 10);
			if(errno || end == value || *end || p < 1 || p > 65535) {
				fprintf(stderr, "--port must be a number from 1 to 65535\n");
				return 1;
			}
			port = (int)p;
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
		} else if(strcmp(argv[i], "--doctor") == 0) {
			doctor_requested = 1;
		} else if(strcmp(argv[i], "--verbose") == 0) {
			g_verbose = 1;
		} else if(strcmp(argv[i], "--help") == 0) {
			printf("usage: %s [--host IP] [--port N] [--state-dir DIR] [--sensitivity FACTOR] [--verbose] [--doctor]\n", argv[0]);
			printf("  --sensitivity: motion speed multiplier (default %.2f; 1 = original speed)\n",
					(double)CONTROLLER_DEFAULT_SENSITIVITY);
			printf("  --verbose: log every incoming WAMP message (truncated), not just lifecycle events\n");
			printf("  --doctor: check spacenavd/certificate/trust-store setup and exit\n");
			return 0;
		} else {
			fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
			return 1;
		}
	}

	if(doctor_requested) {
		return run_doctor(state_dir);
	}

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
		/* Indexed the same way as pfds[] (offset by the listener and spnav
		 * slots), not from 0 - must match pfds's size, not just MAX_CLIENTS,
		 * or the last one or two clients (once >= MAX_CLIENTS-1 are
		 * connected) overflow this array by 1-2 ints. Found via
		 * tests/test_robustness.py's MAX_CLIENTS test hanging under ASan. */
		int client_idx[2 + MAX_CLIENTS];

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
							fprintf(stderr, "input frame from %s exceeds %zu byte message limit, closing\n", cl->peer, MSG_BUF_SIZE);
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
