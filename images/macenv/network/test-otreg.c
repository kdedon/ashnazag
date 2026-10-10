#include <assert.h>
#define main otreg_main
#include "otreg.c"
#undef main

static int pathcalls, regcalls, failpath, failreg;
int modpath(path)
	char *path;
{
	pathcalls++;
	assert(strcmp(path, "/modules") == 0);
	return failpath ? -1 : 0;
}
int modadm(type, command, reg)
	int type, command;
	struct mod_mreg *reg;
{
	regcalls++;
	assert(type == MOD_TY_CDEV && command == MOD_C_MREG);
	assert(strcmp(reg->md_modname, "otbridge") == 0);
	assert(*(int *)reg->md_typedata == 55);
	return failreg ? -1 : 0;
}
int main()
{
	char *av[] = { "otreg", "/modules", 0 };
	assert(otreg_main(1, av) == 2);
	assert(pathcalls == 0 && regcalls == 0);
	failpath = 1;
	assert(otreg_main(2, av) == 1);
	assert(pathcalls == 1 && regcalls == 0);
	failpath = 0; failreg = 1;
	assert(otreg_main(2, av) == 1);
	assert(pathcalls == 2 && regcalls == 1);
	failreg = 0;
	assert(otreg_main(2, av) == 0);
	assert(otreg_main(2, av) == 0);
	assert(pathcalls == 4 && regcalls == 3);
	puts("PASS 10 gateway registration, errors and repeat cases");
	return 0;
}
