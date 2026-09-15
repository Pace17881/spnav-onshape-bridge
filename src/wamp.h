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
#ifndef WAMP_H_
#define WAMP_H_

#include "cJSON.h"

/* Minimal subset of WAMP v1 (https://wamp-proto.org, "v1" predates the
 * modern spec) actually used by Onshape's embedded 3Dconnexion client - not
 * a general implementation. Message shapes were reverse-engineered from
 * the spacenav-ws reference implementation (wamp.py/controller.py). */
enum wamp_msg_type {
	WAMP_WELCOME    = 0,
	WAMP_PREFIX     = 1,
	WAMP_CALL       = 2,
	WAMP_CALLRESULT = 3,
	WAMP_CALLERROR  = 4,
	WAMP_SUBSCRIBE  = 5,
	WAMP_EVENT      = 8
};

void wamp_gen_id(char *out, int len);

/* Builds and returns a malloc'd JSON string for [0, session_id, 1, ident].
 * Caller frees with free(). */
char *wamp_build_welcome(const char *session_id, const char *ident);

/* Builds [3, call_id, result]. `result` may be NULL (-> JSON null). Does not
 * take ownership of result. */
char *wamp_build_callresult(const char *call_id, const cJSON *result);

/* Builds [4, call_id, error_uri, desc, null]. */
char *wamp_build_callerror(const char *call_id, const char *error_uri, const char *desc);

/* Builds the EVENT-wrapped CALL used to invoke a "self:read"/"self:update"
 * RPC on the client's controller object, matching controller.py's
 * remote_read/remote_write framing exactly: the inner CALL's args are
 * always ["", prop, value?] (that leading empty string is part of the
 * observed wire format, not a bug).
 *
 * kind must be "self:read" or "self:update". `value` may be NULL for reads
 * (no third arg emitted). Writes call_id_out (must hold at least 19 bytes)
 * with the generated id, for matching the later CALLRESULT/CALLERROR.
 */
char *wamp_build_rpc_call(const char *controller_uri, const char *kind,
		const char *prop, const cJSON *value, char call_id_out[19]);

#endif
