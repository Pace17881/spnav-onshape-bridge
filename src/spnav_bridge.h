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
