/*
 * dlmta -- MISC module that t_dlm loads first; dlmtb depends on it.
 */

#include "sys/types.h"
#include "sys/moddefs.h"

extern int printf();

int	dlmta_value = 1000;
int	dlmta_loads;			/* common, allocated by the loader */

int
dlmta_add(n)
	int n;
{
	dlmta_value += n;
	return dlmta_value;
}

static int
dlmta_load()
{
	dlmta_loads++;
	printf("dlmta: loaded\n");
	return 0;
}

static int
dlmta_unload()
{
	printf("dlmta: unloaded\n");
	return 0;
}

MOD_MISC_WRAPPER(dlmta, dlmta_load, dlmta_unload, "t_dlm base module");
