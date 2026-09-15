#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "controller.h"
#include "cJSON.h"

static struct controller *g_ctrl;
static char g_last_affine_write[4096];
static int g_write_count;
static double g_last_extent;

static const cJSON *fake_value_for(const char *prop)
{
	static cJSON *identity, *extents, *perspective_false;
	if(!identity) {
		identity = cJSON_Parse("[1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]");
		extents = cJSON_Parse("[-10,-5,-8, 10,5,8]");
		perspective_false = cJSON_CreateFalse();
	}
	if(strcmp(prop, "model.extents") == 0 || strcmp(prop, "view.extents") == 0)
		return extents;
	if(strcmp(prop, "view.affine") == 0 || strcmp(prop, "views.front") == 0)
		return identity;
	if(strcmp(prop, "view.perspective") == 0)
		return perspective_false;
	return NULL;
}

/* Acts as a scripted fake Onshape client: whenever the controller sends an
 * EVENT-wrapped self:read/self:update RPC, immediately synthesize and feed
 * back a plausible CALLRESULT, so the whole chain runs to completion inside
 * this single-threaded test. */
static void fake_send(void *user, const char *json, size_t len)
{
	(void)user;
	printf(">>> %.*s\n", (int)len, json);

	cJSON *root = cJSON_ParseWithLength(json, len);
	if(!root)
		return;
	int type = (int)cJSON_GetArrayItem(root, 0)->valuedouble;
	if(type == 8 /* EVENT */) {
		cJSON *inner = cJSON_GetArrayItem(root, 2);
		const char *call_id = cJSON_GetArrayItem(inner, 1)->valuestring;
		const char *kind = cJSON_GetArrayItem(inner, 2)->valuestring;
		const char *prop = cJSON_GetArrayItem(inner, 4)->valuestring;

		if(strcmp(kind, "self:update") == 0 && strcmp(prop, "view.extents") == 0)
			g_last_extent = cJSON_GetArrayItem(cJSON_GetArrayItem(inner, 5), 3)->valuedouble;

		if(strcmp(kind, "self:update") == 0 && strcmp(prop, "view.affine") == 0) {
			cJSON *val = cJSON_GetArrayItem(inner, 5);
			char *s = cJSON_PrintUnformatted(val);
			snprintf(g_last_affine_write, sizeof g_last_affine_write, "%s", s);
			free(s);
			g_write_count++;
		}

		cJSON *reply = cJSON_CreateArray();
		cJSON_AddItemToArray(reply, cJSON_CreateNumber(3)); /* CALLRESULT */
		cJSON_AddItemToArray(reply, cJSON_CreateString(call_id));
		const cJSON *fv = fake_value_for(prop);
		cJSON_AddItemToArray(reply, fv ? cJSON_Duplicate(fv, 1) : cJSON_CreateTrue());
		char *reply_json = cJSON_PrintUnformatted(reply);
		printf("<<< %s\n", reply_json);
		controller_on_message(g_ctrl, reply_json, strlen(reply_json));
		free(reply_json);
		cJSON_Delete(reply);
	}
	cJSON_Delete(root);
}

static void feed(const char *msg)
{
	printf("--- feeding: %s\n", msg);
	int rc = controller_on_message(g_ctrl, msg, strlen(msg));
	assert(rc == 0);
}

int main(void)
{
	g_ctrl = controller_create(fake_send, NULL);

	feed("[1, \"ctrl\", \"wss://127.51.68.120/3dconnexion3dcontroller/\"]");
	feed("[2, \"C1\", \"3dx_rpc:create\", \"3dconnexion:3dmouse\", \"1.7.14\"]");
	feed("[2, \"C2\", \"3dx_rpc:create\", \"3dconnexion:3dcontroller\", \"mouse0\", "
		 "{\"name\": \"Onshape\", \"version\": \"1.7.14\"}]");
	feed("[5, \"ctrl:controller0\"]"); /* SUBSCRIBE */

	printf("\n=== sending a motion event (orthographic path) ===\n");
	struct sb_event ev = {0};
	ev.type = SB_EVENT_MOTION;
	ev.x = 40;
	ev.y = -15;
	ev.z = 22;
	ev.rx = 12;
	ev.ry = -7;
	ev.rz = 3;
	ev.period = 16;
	controller_on_spnav_event(g_ctrl, &ev);

	assert(g_write_count == 1);
	assert(strcmp(g_last_affine_write, "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]") != 0);
	printf("\nwrite_count=%d\nlast view.affine write: %s\n", g_write_count, g_last_affine_write);

	printf("\n=== sending a button event ===\n");
	struct sb_event bev = {0};
	bev.type = SB_EVENT_BUTTON;
	bev.bnum = 0;
	bev.pressed = 1;
	controller_on_spnav_event(g_ctrl, &bev);

	printf("\n=== unknown origin client should still complete handshake but be muted ===\n");
	struct controller *c2 = controller_create(fake_send, NULL);
	assert(g_write_count == 2);
	assert(strcmp(g_last_affine_write, "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]") == 0);
	struct controller *saved = g_ctrl;
	g_ctrl = c2;
	feed("[1, \"ctrl\", \"wss://127.51.68.120/3dconnexion3dcontroller/\"]");
	feed("[2, \"U1\", \"3dx_rpc:create\", \"3dconnexion:3dmouse\"]");
	feed("[2, \"U2\", \"3dx_rpc:create\", \"3dconnexion:3dcontroller\", \"mouse0\", "
		 "{\"name\":\"Unknown\"}]");
	feed("[5, \"ctrl:controller0\"]");
	controller_on_spnav_event(c2, &ev);
	assert(g_write_count == 2);
	assert(controller_on_message(c2, "invalid", 7) == -1);
	controller_destroy(c2);
	g_ctrl = saved;

	/* Verify default gain against independently calculated pure movement. */
	struct sb_event move = {0};
	move.type = SB_EVENT_MOTION;
	move.x = 100;
	move.z = 100;
	controller_on_spnav_event(g_ctrl, &move);
	cJSON *aff = cJSON_Parse(g_last_affine_write);
	assert(fabs(cJSON_GetArrayItem(aff, 12)->valuedouble + 0.0175) < 1e-6);
	assert(fabs(cJSON_GetArrayItem(aff, 14)->valuedouble - 0.0175) < 1e-6);
	assert(fabs(g_last_extent - 10.07) < 1e-5);
	cJSON_Delete(aff);

	assert(controller_set_sensitivity(g_ctrl, 1.0f) == 0);
	assert(controller_set_sensitivity(g_ctrl, 0) == -1);
	assert(controller_set_sensitivity(g_ctrl, -1) == -1);
	assert(controller_set_sensitivity(g_ctrl, NAN) == -1);
	assert(controller_set_sensitivity(g_ctrl, INFINITY) == -1);
	assert(controller_set_sensitivity(g_ctrl, 11) == -1);
	controller_on_spnav_event(g_ctrl, &move);
	aff = cJSON_Parse(g_last_affine_write);
	assert(fabs(cJSON_GetArrayItem(aff, 12)->valuedouble + 0.05) < 1e-6);
	assert(fabs(g_last_extent - 10.2) < 1e-5);
	cJSON_Delete(aff);

	move.x = move.z = 0;
	move.rz = 100;
	controller_on_spnav_event(g_ctrl, &move);
	aff = cJSON_Parse(g_last_affine_write);
	assert(fabs(fabs(cJSON_GetArrayItem(aff, 1)->valuedouble) -
		sin(2.0 * 3.141592653589793 / 180.0)) < 1e-6);
	cJSON_Delete(aff);
	assert(controller_set_sensitivity(g_ctrl, 0.35f) == 0);
	controller_on_spnav_event(g_ctrl, &move);
	aff = cJSON_Parse(g_last_affine_write);
	assert(fabs(fabs(cJSON_GetArrayItem(aff, 1)->valuedouble) -
		sin(0.7 * 3.141592653589793 / 180.0)) < 1e-6);
	cJSON_Delete(aff);

	controller_destroy(g_ctrl);
	return 0;
}
