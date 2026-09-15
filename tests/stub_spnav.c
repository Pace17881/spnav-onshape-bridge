/* Quiet, pollable spacenavd substitute for transport integration tests.
 *
 * By default sb_poll_event() never returns an event - most tests only care
 * about HTTP/TLS/WS/WAMP behavior, not spacenavd motion. When the
 * SPNAV_TEST_EVENTS environment variable names a FIFO, sb_open() reads
 * synthetic events from it instead, letting a test inject a motion/button
 * event into a running test_daemon from the outside (see
 * tests/wstest.py's inject_event(), used by tests/test_robustness.py to
 * cover the multi-client fan-out in main.c that a real spacenavd event
 * normally drives). The wire format there is 10 little-endian int32
 * fields - type,x,y,z,rx,ry,rz,period,bnum,pressed - independent of
 * struct sb_event's in-memory layout. */
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "spnav_bridge.h"

static int fds[2];
static int inject_fd = -1;

int sb_open(void)
{
	const char *path = getenv("SPNAV_TEST_EVENTS");
	if(path && *path) {
		inject_fd = open(path, O_RDONLY | O_NONBLOCK);
		if(inject_fd >= 0)
			return 0;
	}
	return pipe(fds);
}

int sb_fd(void)
{
	return inject_fd >= 0 ? inject_fd : fds[0];
}

int sb_poll_event(struct sb_event *ev)
{
	if(inject_fd >= 0) {
		unsigned char buf[40];
		ssize_t n = read(inject_fd, buf, sizeof buf);
		if(n != (ssize_t)sizeof buf)
			return 0; /* no data, EOF (no writer yet/closed), or a short read */
		int32_t vals[10];
		memcpy(vals, buf, sizeof vals);
		ev->type = (enum sb_event_type)vals[0];
		ev->x = vals[1]; ev->y = vals[2]; ev->z = vals[3];
		ev->rx = vals[4]; ev->ry = vals[5]; ev->rz = vals[6];
		ev->period = (unsigned int)vals[7];
		ev->bnum = vals[8];
		ev->pressed = vals[9];
		return 1;
	}
	(void)ev;
	return 0;
}

void sb_close(void)
{
	if(inject_fd >= 0) {
		close(inject_fd);
		inject_fd = -1;
		return;
	}
	close(fds[0]);
	close(fds[1]);
}
