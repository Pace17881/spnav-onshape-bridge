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
#include <stddef.h>
#include <spnav.h>
#include "spnav_bridge.h"

static int connected;

int sb_open(void)
{
	if(spnav_open() == -1) {
		return -1;
	}
	connected = 1;
	return 0;
}

void sb_close(void)
{
	if(connected) {
		spnav_close();
		connected = 0;
	}
}

int sb_fd(void)
{
	return connected ? spnav_fd() : -1;
}

int sb_poll_event(struct sb_event *ev)
{
	spnav_event raw;
	int rc;

	if(!connected) {
		return -1;
	}

	rc = spnav_poll_event(&raw);
	if(rc == 0) {
		return 0;
	}

	switch(raw.type) {
	case SPNAV_EVENT_MOTION:
		ev->type = SB_EVENT_MOTION;
		ev->x = raw.motion.x;
		ev->y = raw.motion.y;
		ev->z = raw.motion.z;
		ev->rx = raw.motion.rx;
		ev->ry = raw.motion.ry;
		ev->rz = raw.motion.rz;
		ev->period = raw.motion.period;
		return 1;

	case SPNAV_EVENT_BUTTON:
		ev->type = SB_EVENT_BUTTON;
		ev->bnum = raw.button.bnum;
		ev->pressed = raw.button.press;
		return 1;

	default:
		/* device add/remove, config change, raw axis/button: not needed
		 * for the Onshape bridge, drop it and let the caller poll again */
		return 0;
	}
}
