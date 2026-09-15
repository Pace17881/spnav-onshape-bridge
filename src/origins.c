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
