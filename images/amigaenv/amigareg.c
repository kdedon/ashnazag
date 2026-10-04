#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "sys/mod.h"
#include "amigaio.h"

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct mod_mreg reg;
	int major = AMIGA_MAJOR;

	if (argc != 2) {
		fprintf(stderr, "usage: amigareg module-directory\n");
		return 2;
	}
	if (modpath(argv[1]) < 0) {
		perror("amigareg: module path");
		return 1;
	}
	memset(&reg, 0, sizeof reg);
	strcpy(reg.md_modname, "amigaguest");
	reg.md_typedata = (caddr_t)&major;
	if (modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) < 0) {
		perror("amigareg: register amigaguest");
		return 1;
	}
	return 0;
}
