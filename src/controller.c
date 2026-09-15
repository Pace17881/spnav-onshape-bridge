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
#include <math.h>
#include "controller.h"
#include "wamp.h"
#include "mat4.h"
#include "cJSON.h"

#define CONTROLLER_URI "wss://127.51.68.120/3dconnexion3dcontroller/controller0"
#define UPDATE_URI     "wss://127.51.68.120/3dconnexion#update"

enum hs_state { HS_WAIT_MOUSE, HS_WAIT_CONTROLLER, HS_READY };
enum chain_kind { CHAIN_NONE, CHAIN_MOTION, CHAIN_BUTTON };

enum chain_step {
	MSTEP_READ_EXTENTS, MSTEP_READ_PERSPECTIVE, MSTEP_READ_AFFINE,
	MSTEP_READ_VIEWEXTENTS, MSTEP_WRITE_MOTION, MSTEP_WRITE_VIEWEXTENTS, MSTEP_WRITE_AFFINE,
	BSTEP_READ_EXTENTS, BSTEP_READ_FRONT, BSTEP_WRITE_AFFINE, BSTEP_WRITE_EXTENTS
};

#define MAX_PREFIXES 8
struct prefix_entry { char prefix[64]; char uri[256]; };

struct controller {
	controller_send_fn send;
	void *send_user;

	enum hs_state hs;
	int known_client;
	int subscribed;
	int focus;
	float sensitivity;

	struct prefix_entry prefixes[MAX_PREFIXES];
	int nprefixes;

	enum chain_kind chain;
	enum chain_step step;
	char pending_call_id[19];

	struct sb_event active_ev;
	struct sb_event pending_motion;
	int have_pending_motion;
	int have_pending_button;

	float model_extents[6];
	int perspective;
	float curr_affine[16];
	float new_affine[16];
	float view_extents[6]; /* reused for new_extents once computed */
};

static void send_json(struct controller *c, char *json)
{
	if(!json) return;
	c->send(c->send_user, json, strlen(json));
	free(json);
}

static void rpc_read(struct controller *c, const char *prop)
{
	send_json(c, wamp_build_rpc_call(CONTROLLER_URI, "self:read", prop, NULL, c->pending_call_id));
}

static void rpc_write(struct controller *c, const char *prop, const cJSON *value)
{
	send_json(c, wamp_build_rpc_call(CONTROLLER_URI, "self:update", prop, value, c->pending_call_id));
}

static int json_to_floats(const cJSON *arr, float *out, int n)
{
	int i;
	if(!arr || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < n) {
		return -1;
	}
	for(i = 0; i < n; i++) {
		cJSON *item = cJSON_GetArrayItem(arr, i);
		if(!cJSON_IsNumber(item)) return -1;
		out[i] = (float)item->valuedouble;
	}
	return 0;
}

static cJSON *floats_to_json(const float *in, int n)
{
	cJSON *arr = cJSON_CreateArray();
	int i;
	for(i = 0; i < n; i++) {
		cJSON_AddItemToArray(arr, cJSON_CreateNumber(in[i]));
	}
	return arr;
}

static void resolve_uri(struct controller *c, const char *uri, char *out, size_t outsz)
{
	const char *colon = strchr(uri, ':');
	const char *rest;
	size_t plen;
	int i;

	if(!colon) {
		snprintf(out, outsz, "%s", uri);
		return;
	}
	plen = (size_t)(colon - uri);
	rest = colon + 1;

	for(i = 0; i < c->nprefixes; i++) {
		if(strlen(c->prefixes[i].prefix) == plen &&
				strncmp(c->prefixes[i].prefix, uri, plen) == 0) {
			snprintf(out, outsz, "%s%s", c->prefixes[i].uri, rest);
			return;
		}
	}
	/* unknown prefix: matches spacenav-ws's resolve(), which does
	 * `prefixes.get(prefix, "") + res` - the "prefix:" part is dropped */
	snprintf(out, outsz, "%s", rest);
}

/* Computes the new view.affine following controller.py's update_client
 * exactly (Gram-Schmidt instead of SVD for re-orthonormalization - see
 * mat4.c). Axis relabeling (ev->y/z swap) matches the cross-reference
 * between libspnav's wire parsing and spacenav-ws's raw-socket parsing:
 * spacenav-ws's "event.y" is libspnav's z, and its "event.z" is libspnav's y.
 */
static void compute_new_affine(struct controller *c, const struct sb_event *ev)
{
	float r_cam[9], r_cam_t[9], r_delta[9], tmp9[9], r_world[9];
	float rot_delta[16], trans_delta[16];
	float pivot[3], pivot_pos[16], pivot_neg[16];
	float tmp16a[16], tmp16b[16];

	mat4_extract_orthonormal_rotation(r_cam, c->curr_affine);
	mat3_from_euler_xyz_deg(r_delta, ev->rx * 0.02f * c->sensitivity,
			ev->ry * 0.02f * c->sensitivity, -ev->rz * 0.02f * c->sensitivity);
	mat3_transpose(r_cam_t, r_cam);
	mat3_mul(tmp9, r_cam, r_delta);
	mat3_mul(r_world, tmp9, r_cam_t);

	mat4_from_rot3(rot_delta, r_world);
	mat4_from_translation_row(trans_delta, -ev->x * 0.0005f * c->sensitivity,
			-ev->y * 0.0005f * c->sensitivity, ev->z * 0.0005f * c->sensitivity);

	pivot[0] = (c->model_extents[0] + c->model_extents[3]) * 0.5f;
	pivot[1] = (c->model_extents[1] + c->model_extents[4]) * 0.5f;
	pivot[2] = (c->model_extents[2] + c->model_extents[5]) * 0.5f;
	mat4_from_translation_row(pivot_pos, pivot[0], pivot[1], pivot[2]);
	mat4_from_translation_row(pivot_neg, -pivot[0], -pivot[1], -pivot[2]);

	/* new_affine = trans_delta @ curr_affine @ (pivot_neg @ rot_delta @ pivot_pos) */
	mat4_mul(tmp16a, pivot_neg, rot_delta);
	mat4_mul(tmp16b, tmp16a, pivot_pos);
	mat4_mul(tmp16a, c->curr_affine, tmp16b);
	mat4_mul(c->new_affine, trans_delta, tmp16a);
}

static void start_motion_chain(struct controller *c, const struct sb_event *ev)
{
	c->active_ev = *ev;
	c->chain = CHAIN_MOTION;
	c->step = MSTEP_READ_EXTENTS;
	rpc_read(c, "model.extents");
}

static void start_button_chain(struct controller *c)
{
	c->chain = CHAIN_BUTTON;
	c->step = BSTEP_READ_EXTENTS;
	rpc_read(c, "model.extents");
}

static void chain_finished(struct controller *c)
{
	c->chain = CHAIN_NONE;
	if(c->have_pending_button) {
		c->have_pending_button = 0;
		c->have_pending_motion = 0; /* a view reset supersedes queued motion */
		start_button_chain(c);
	} else if(c->have_pending_motion) {
		c->have_pending_motion = 0;
		start_motion_chain(c, &c->pending_motion);
	}
}

static void advance_motion_chain(struct controller *c, const cJSON *result)
{
	switch(c->step) {
	case MSTEP_READ_EXTENTS:
		if(json_to_floats(result, c->model_extents, 6) != 0) { chain_finished(c); return; }
		c->step = MSTEP_READ_PERSPECTIVE;
		rpc_read(c, "view.perspective");
		break;

	case MSTEP_READ_PERSPECTIVE:
		c->perspective = cJSON_IsTrue(result) || (cJSON_IsNumber(result) && result->valuedouble != 0);
		c->step = MSTEP_READ_AFFINE;
		rpc_read(c, "view.affine");
		break;

	case MSTEP_READ_AFFINE:
		if(json_to_floats(result, c->curr_affine, 16) != 0) { chain_finished(c); return; }
		compute_new_affine(c, &c->active_ev);
		if(!c->perspective) {
			c->step = MSTEP_READ_VIEWEXTENTS;
			rpc_read(c, "view.extents");
		} else {
			c->step = MSTEP_WRITE_MOTION;
			cJSON *t = cJSON_CreateTrue();
			rpc_write(c, "motion", t);
			cJSON_Delete(t);
		}
		break;

	case MSTEP_READ_VIEWEXTENTS: {
		float scale;
		int i;
		if(json_to_floats(result, c->view_extents, 6) != 0) { chain_finished(c); return; }
		scale = 1.0f + c->active_ev.z * 0.0002f * c->sensitivity;
		for(i = 0; i < 6; i++) c->view_extents[i] *= scale;
		c->step = MSTEP_WRITE_MOTION;
		cJSON *t = cJSON_CreateTrue();
		rpc_write(c, "motion", t);
		cJSON_Delete(t);
		break;
	}

	case MSTEP_WRITE_MOTION:
		if(!c->perspective) {
			cJSON *ext = floats_to_json(c->view_extents, 6);
			c->step = MSTEP_WRITE_VIEWEXTENTS;
			rpc_write(c, "view.extents", ext);
			cJSON_Delete(ext);
		} else {
			cJSON *aff = floats_to_json(c->new_affine, 16);
			c->step = MSTEP_WRITE_AFFINE;
			rpc_write(c, "view.affine", aff);
			cJSON_Delete(aff);
		}
		break;

	case MSTEP_WRITE_VIEWEXTENTS: {
		cJSON *aff = floats_to_json(c->new_affine, 16);
		c->step = MSTEP_WRITE_AFFINE;
		rpc_write(c, "view.affine", aff);
		cJSON_Delete(aff);
		break;
	}

	case MSTEP_WRITE_AFFINE:
		chain_finished(c);
		break;

	default:
		chain_finished(c);
	}
}

static void advance_button_chain(struct controller *c, const cJSON *result)
{
	switch(c->step) {
	case BSTEP_READ_EXTENTS:
		if(json_to_floats(result, c->model_extents, 6) != 0) { chain_finished(c); return; }
		c->step = BSTEP_READ_FRONT;
		rpc_read(c, "views.front");
		break;

	case BSTEP_READ_FRONT:
		/* forward the front-view matrix straight back as the new affine,
		 * no need to round-trip it through our own float array */
		c->step = BSTEP_WRITE_AFFINE;
		rpc_write(c, "view.affine", result);
		break;

	case BSTEP_WRITE_AFFINE: {
		float scaled[6];
		int i;
		cJSON *ext;
		for(i = 0; i < 6; i++) scaled[i] = c->model_extents[i] * 1.2f;
		ext = floats_to_json(scaled, 6);
		c->step = BSTEP_WRITE_EXTENTS;
		rpc_write(c, "view.extents", ext);
		cJSON_Delete(ext);
		break;
	}

	case BSTEP_WRITE_EXTENTS:
		chain_finished(c);
		break;

	default:
		chain_finished(c);
	}
}

struct controller *controller_create(controller_send_fn send, void *send_user)
{
	struct controller *c = calloc(1, sizeof *c);
	char session_id[17];
	char *welcome;

	if(!c) {
		return NULL;
	}

	c->send = send;
	c->send_user = send_user;
	c->hs = HS_WAIT_MOUSE;
	c->sensitivity = CONTROLLER_DEFAULT_SENSITIVITY;

	wamp_gen_id(session_id, 16);
	welcome = wamp_build_welcome(session_id, "spnav-onshape-bridge v0.1");
	send_json(c, welcome);
	return c;
}

int controller_set_sensitivity(struct controller *c, float sensitivity)
{
	if(!isfinite(sensitivity) || sensitivity < 0.01f || sensitivity > 10.0f)
		return -1;
	c->sensitivity = sensitivity;
	return 0;
}

void controller_destroy(struct controller *c)
{
	free(c);
}

static const char *json_str(cJSON *arr, int idx)
{
	cJSON *item = cJSON_GetArrayItem(arr, idx);
	return (item && cJSON_IsString(item)) ? item->valuestring : NULL;
}

static int known_client_name(const char *name)
{
	return name && (strcmp(name, "Onshape") == 0 || strcmp(name, "WebThreeJS Sample") == 0);
}

int controller_on_message(struct controller *c, const char *json, size_t len)
{
	cJSON *root = cJSON_ParseWithLength(json, len);
	int type;
	int rc = 0;

	if(!root || !cJSON_IsArray(root) || cJSON_GetArraySize(root) < 1) {
		cJSON_Delete(root);
		return -1;
	}
	{
		cJSON *t = cJSON_GetArrayItem(root, 0);
		if(!cJSON_IsNumber(t)) { cJSON_Delete(root); return -1; }
		type = (int)t->valuedouble;
	}

	switch(type) {
	case WAMP_PREFIX: {
		const char *prefix = json_str(root, 1);
		const char *uri = json_str(root, 2);
		if(prefix && uri && c->nprefixes < MAX_PREFIXES) {
			snprintf(c->prefixes[c->nprefixes].prefix, sizeof c->prefixes[0].prefix, "%s", prefix);
			snprintf(c->prefixes[c->nprefixes].uri, sizeof c->prefixes[0].uri, "%s", uri);
			c->nprefixes++;
		}
		break;
	}

	case WAMP_CALL: {
		const char *call_id = json_str(root, 1);
		const char *proc_uri = json_str(root, 2);

		if(!call_id || !proc_uri) { rc = -1; break; }

		if(c->hs == HS_WAIT_MOUSE) {
			const char *a0 = json_str(root, 3);
			if(strcmp(proc_uri, "3dx_rpc:create") != 0 || !a0 ||
					strcmp(a0, "3dconnexion:3dmouse") != 0) {
				rc = -1;
				break;
			}
			cJSON *result = cJSON_CreateObject();
			cJSON_AddStringToObject(result, "connexion", "mouse0");
			send_json(c, wamp_build_callresult(call_id, result));
			cJSON_Delete(result);
			c->hs = HS_WAIT_CONTROLLER;

		} else if(c->hs == HS_WAIT_CONTROLLER) {
			const char *a0 = json_str(root, 3);
			const char *a1 = json_str(root, 4);
			cJSON *metadata = cJSON_GetArrayItem(root, 5);
			if(strcmp(proc_uri, "3dx_rpc:create") != 0 || !a0 || !a1 ||
					strcmp(a0, "3dconnexion:3dcontroller") != 0 ||
					strcmp(a1, "mouse0") != 0) {
				rc = -1;
				break;
			}
			if(metadata && cJSON_IsObject(metadata)) {
				cJSON *name = cJSON_GetObjectItemCaseSensitive(metadata, "name");
				c->known_client = known_client_name(cJSON_IsString(name) ? name->valuestring : NULL);
				if(!c->known_client) {
					fprintf(stderr, "warning: unrecognized client '%s', "
							"will not forward mouse events to it\n",
							cJSON_IsString(name) ? name->valuestring : "?");
				}
			}
			cJSON *result = cJSON_CreateObject();
			cJSON_AddStringToObject(result, "instance", "controller0");
			send_json(c, wamp_build_callresult(call_id, result));
			cJSON_Delete(result);
			c->hs = HS_READY;

		} else {
			char resolved[320];
			resolve_uri(c, proc_uri, resolved, sizeof resolved);
			if(strcmp(resolved, UPDATE_URI) == 0) {
				cJSON *arg0 = cJSON_GetArrayItem(root, 3);
				if(arg0 && cJSON_IsObject(arg0)) {
					cJSON *focus = cJSON_GetObjectItemCaseSensitive(arg0, "focus");
					if(focus) c->focus = cJSON_IsTrue(focus);
				}
				send_json(c, wamp_build_callresult(call_id, NULL));
			} else {
				char desc[256];
				snprintf(desc, sizeof desc, "RPC '%s' not registered", proc_uri);
				send_json(c, wamp_build_callerror(call_id, "wamp.error.not_found", desc));
			}
		}
		break;
	}

	case WAMP_SUBSCRIBE: {
		const char *topic = json_str(root, 1);
		if(topic) {
			char resolved[320];
			resolve_uri(c, topic, resolved, sizeof resolved);
			if(strcmp(resolved, CONTROLLER_URI) == 0) {
				c->subscribed = 1;
				c->focus = 1;
			}
		}
		break;
	}

	case WAMP_CALLRESULT:
	case WAMP_CALLERROR: {
		const char *call_id = json_str(root, 1);
		if(c->chain != CHAIN_NONE && call_id && strcmp(call_id, c->pending_call_id) == 0) {
			if(type == WAMP_CALLERROR) {
				chain_finished(c);
			} else {
				cJSON *result = cJSON_GetArrayItem(root, 2);
				if(c->chain == CHAIN_MOTION) advance_motion_chain(c, result);
				else advance_button_chain(c, result);
			}
		}
		break;
	}

	default:
		break;
	}

	cJSON_Delete(root);
	return rc;
}

void controller_on_spnav_event(struct controller *c, const struct sb_event *ev)
{
	if(c->hs != HS_READY || !c->known_client || !c->subscribed || !c->focus) {
		return;
	}

	if(c->chain != CHAIN_NONE) {
		if(ev->type == SB_EVENT_MOTION) {
			c->pending_motion = *ev;
			c->have_pending_motion = 1;
		} else {
			c->have_pending_button = 1;
		}
		return;
	}

	if(ev->type == SB_EVENT_MOTION) {
		start_motion_chain(c, ev);
	} else {
		start_button_chain(c);
	}
}
