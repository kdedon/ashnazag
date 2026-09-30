/*
 * auxreg -- let A/UX programs run: add the guest modules' directory to
 * the module search path and register auxexec for COFF magic 0x150
 * ahead of the stock loader.  The modules load on the first A/UX exec.
 * With a major, uinter is registered as that character device too.
 *
 *	auxreg dir [major]
 *
 * K&R C.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include "sys/mod.h"

extern int errno;
extern char *strerror();

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct mod_mreg reg;
	struct mod_execreg er;
	int mj;
	long v;
	char *e;

	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: auxreg dir [major]\n");
		return 2;
	}
	mj = 0;
	if (argc == 3) {
		v = strtol(argv[2], &e, 10);
		if (*argv[2] == '\0' || *e != '\0' || v < 1 || v > 255) {
			fprintf(stderr, "auxreg: bad major %s\n", argv[2]);
			return 2;
		}
		mj = (int)v;
	}
	if (modpath(argv[1]) < 0) {
		fprintf(stderr, "auxreg: module path %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	strcpy(reg.md_modname, "auxexec");
	reg.md_typedata = (caddr_t)&er;
	er.er_magic = 0x150;
	er.er_flags = EXF_FIRST;
	if (modadm(MOD_TY_EXEC, MOD_C_MREG, &reg) < 0) {
		fprintf(stderr, "auxreg: register auxexec: %s\n", strerror(errno));
		return 1;
	}
	if (mj) {
		strcpy(reg.md_modname, "uinter");
		reg.md_typedata = (caddr_t)&mj;
		if (modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) < 0) {
			fprintf(stderr, "auxreg: register uinter: %s\n", strerror(errno));
			return 1;
		}
	}
	return 0;
}
