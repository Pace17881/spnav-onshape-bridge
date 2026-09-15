#ifndef SPNAV_BRIDGE_H_
#define SPNAV_BRIDGE_H_

enum sb_event_type { SB_EVENT_MOTION, SB_EVENT_BUTTON };

struct sb_event {
	enum sb_event_type type;

	/* motion: verbatim libspnav axis values (spnav_event_motion.x/y/z/rx/ry/rz),
	 * NOT yet remapped to Onshape's axis convention - see controller.c for
	 * that (derived from cross-referencing libspnav's wire parsing against
	 * spacenav-ws's raw-socket parsing, see comment there). */
	int x, y, z, rx, ry, rz;
	unsigned int period;

	/* button */
	int bnum;
	int pressed;
};

/* Opens the connection to spacenavd via libspnav. Returns 0 on success. */
int sb_open(void);
void sb_close(void);

/* fd suitable for poll()/select(), or -1 if not connected. */
int sb_fd(void);

/* Drains one pending event into *ev. Returns 1 if an event was read, 0 if
 * none was pending (call after poll() reports sb_fd() readable), -1 on
 * unrecoverable error (connection to spacenavd lost). */
int sb_poll_event(struct sb_event *ev);

#endif
