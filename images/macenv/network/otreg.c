#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include "sys/mod.h"

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct mod_mreg reg;
	int major = 55;

	if (argc != 2) {
		fprintf(stderr, "usage: otreg module-directory\n");
		return 2;
	}
	if (modpath(argv[1]) < 0) {
		perror("otreg: module path");
		return 1;
	}
	strcpy(reg.md_modname, "otbridge");
	reg.md_typedata = (caddr_t)&major;
	if (modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) < 0) {
		perror("otreg: register otbridge");
		return 1;
	}
	return 0;
}
