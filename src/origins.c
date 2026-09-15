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
#include <string.h>
#include "origins.h"

static const char *allowed_origins[] = {
	"https://cad.onshape.com",
	NULL
};

int origin_allowed(const char *origin)
{
	int i;

	if(!origin) {
		return 0;
	}
	for(i=0; allowed_origins[i]; i++) {
		if(strcmp(origin, allowed_origins[i]) == 0) {
			return 1;
		}
	}
	return 0;
}
