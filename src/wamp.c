#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <openssl/rand.h>
#include "wamp.h"

/* These IDs are just RPC call-correlation tags on a loopback connection
 * already gated by the Origin check (src/origins.c) - not secrets, and
 * predictability here isn't a real security issue. Uses RAND_bytes anyway,
 * rather than rand()/srand(), because it's already linked in for
 * src/tls.c's certificate generation and removes any seeding question. */
void wamp_gen_id(char *out, int len)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	unsigned char rnd[32];
	int i;

	if(len > (int)sizeof rnd) len = (int)sizeof rnd;
	RAND_bytes(rnd, len);
	for(i=0; i<len; i++) {
		out[i] = alphabet[rnd[i] % (sizeof alphabet - 1)];
	}
	out[len] = 0;
}

static char *finish(cJSON *arr)
{
	char *s = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	return s;
}

char *wamp_build_welcome(const char *session_id, const char *ident)
{
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToArray(arr, cJSON_CreateNumber(WAMP_WELCOME));
	cJSON_AddItemToArray(arr, cJSON_CreateString(session_id));
	cJSON_AddItemToArray(arr, cJSON_CreateNumber(1));
	cJSON_AddItemToArray(arr, cJSON_CreateString(ident));
	return finish(arr);
}

char *wamp_build_callresult(const char *call_id, const cJSON *result)
{
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToArray(arr, cJSON_CreateNumber(WAMP_CALLRESULT));
	cJSON_AddItemToArray(arr, cJSON_CreateString(call_id));
	cJSON_AddItemToArray(arr, result ? cJSON_Duplicate(result, 1) : cJSON_CreateNull());
	return finish(arr);
}

char *wamp_build_callerror(const char *call_id, const char *error_uri, const char *desc)
{
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToArray(arr, cJSON_CreateNumber(WAMP_CALLERROR));
	cJSON_AddItemToArray(arr, cJSON_CreateString(call_id));
	cJSON_AddItemToArray(arr, cJSON_CreateString(error_uri));
	cJSON_AddItemToArray(arr, cJSON_CreateString(desc));
	cJSON_AddItemToArray(arr, cJSON_CreateNull());
	return finish(arr);
}

char *wamp_build_rpc_call(const char *controller_uri, const char *kind,
		const char *prop, const cJSON *value, char call_id_out[19])
{
	cJSON *inner, *outer;

	wamp_gen_id(call_id_out, 18);

	inner = cJSON_CreateArray();
	cJSON_AddItemToArray(inner, cJSON_CreateNumber(WAMP_CALL));
	cJSON_AddItemToArray(inner, cJSON_CreateString(call_id_out));
	cJSON_AddItemToArray(inner, cJSON_CreateString(kind));
	cJSON_AddItemToArray(inner, cJSON_CreateString("")); /* observed wire format */
	cJSON_AddItemToArray(inner, cJSON_CreateString(prop));
	if(value) {
		cJSON_AddItemToArray(inner, cJSON_Duplicate(value, 1));
	}

	outer = cJSON_CreateArray();
	cJSON_AddItemToArray(outer, cJSON_CreateNumber(WAMP_EVENT));
	cJSON_AddItemToArray(outer, cJSON_CreateString(controller_uri));
	cJSON_AddItemToArray(outer, inner);
	return finish(outer);
}
