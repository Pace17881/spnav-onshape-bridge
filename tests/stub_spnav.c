/* Quiet, pollable spacenavd substitute for transport integration tests. */
#include <unistd.h>
#include "spnav_bridge.h"
static int fds[2];
int sb_open(void) { return pipe(fds); }
int sb_fd(void) { return fds[0]; }
int sb_poll_event(struct sb_event *ev)
{
	(void)ev;
	return 0;
}
void sb_close(void)
{
	close(fds[0]);
	close(fds[1]);
}
