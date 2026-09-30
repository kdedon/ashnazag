/*
 * auxreg -- let A/UX programs run: add the guest modules' directory to
 * the module search path and register auxexec for COFF magic 0x150
 * ahead of the stock loader.  The modules load on the first A/UX exec.
 *
 *	auxreg dir
 *
 * K&R C.
 */

#include <stdio.h>
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

	if (argc != 2) {
		fprintf(stderr, "usage: auxreg dir\n");
		return 2;
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
	return 0;
}
