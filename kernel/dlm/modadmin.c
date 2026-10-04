/*
 * modadmin -- load, unload and query loadable kernel modules.
 *
 *	modadmin -l name|path ...	load; print the ids
 *	modadmin -u id ...		unload (0 = every unused module)
 *	modadmin -U name ...		unload by name
 *	modadmin -q id ...		full status
 *	modadmin -Q name ...		full status by name
 *	modadmin -s			one line per loaded module
 *	modadmin -S			full status of every loaded module
 *	modadmin -d dir[:dir]		prepend to the module search path
 *	modadmin -D			reset the search path
 *
 * Exit status 1 if any operand failed.  Built for AMIX with libmod.a.
 *
 * K&R C.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include "sys/mod.h"

extern int errno;
extern char *strerror();

char	*progname = "modadmin";
int	status;

char *
errstr(e)
	int e;
{
	static char buf[32];

	switch (e) {
	case ENOLOAD:	return "a required module could not be loaded";
	case ERELOC:	return "object file, symbol or relocation error";
	case ENOMATCH:	return "symbol not found";
	case EBADVER:	return "module wrapper revision mismatch";
	case ECONFIG:	return "configured kernel resource exhausted";
	}
	if (strerror(e))
		return strerror(e);
	sprintf(buf, "error %d", e);
	return buf;
}

void
fail(what, arg)
	char *what, *arg;
{
	fprintf(stderr, "%s: %s %s: %s\n", progname, what, arg, errstr(errno));
	status = 1;
}

char *
tyname(t)
	int t;
{
	static char *n[] = { "none", "cdev", "bdev", "str", "fs", "sdev",
	    "misc", "exec", "sys" };

	return t >= 0 && t <= MOD_TY_MAX ? n[t] : "?";
}

/* id of a loaded module by name: ms_name, else the path's last part */
int
byname(name)
	char *name;
{
	struct modstatus st;
	char *p;
	int id = 1;

	while (modstat(id, &st, 1) == 0) {
		p = strrchr(st.ms_path, '/');
		if (strcmp(st.ms_name, name) == 0 ||
		    strcmp(p ? p + 1 : st.ms_path, name) == 0)
			return st.ms_id;
		id = st.ms_id + 1;
	}
	return -1;
}

void
full(st)
	struct modstatus *st;
{
	int i;

	printf("Module Name:\t\t%s\n", st->ms_name);
	printf("Module ID:\t\t%d\n", st->ms_id);
	printf("Module Path:\t\t%s\n", st->ms_path);
	printf("Module Size:\t\t%u\n", st->ms_size);
	printf("Base Address:\t\t0x%lx\n", (unsigned long)st->ms_base);
	printf("Module Revision:\t%d\n", st->ms_rev);
	printf("Unload Delay:\t\t%ld seconds\n", (long)st->ms_unload_delay);
	printf("Reference Count:\t%d\n", st->ms_refcnt);
	printf("Dependent Count:\t%d\n", st->ms_depcnt);
	printf("Flags:\t\t\t%s%s%s\n", st->ms_flags & MS_DEMAND ? "demand " : "",
	    st->ms_flags & MS_LOCKED ? "locked " : "",
	    st->ms_flags & MS_CAND ? "candidate" : "");
	for (i = 0; i < MODMAXLINK; i++) {
		if (st->ms_msinfo[i].mss_type == 0 && st->ms_msinfo[i].mss_linkinfo[0] == 0)
			continue;
		printf("Linkage %d:\t\t%s (%s) %d %d %d %d\n", i,
		    st->ms_msinfo[i].mss_linkinfo, tyname(st->ms_msinfo[i].mss_type),
		    st->ms_msinfo[i].mss_p0[0], st->ms_msinfo[i].mss_p0[1],
		    st->ms_msinfo[i].mss_p1[0], st->ms_msinfo[i].mss_p1[1]);
	}
	printf("\n");
}

void
query(id, arg)
	int id;
	char *arg;
{
	struct modstatus st;

	if (id < 0 || modstat(id, &st, 0) < 0) {
		if (id < 0)
			errno = EINVAL;
		fail("cannot query", arg);
		return;
	}
	full(&st);
}

void
all(verbose)
	int verbose;
{
	struct modstatus st;
	int id = 1;

	if (!verbose)
		printf("ID\tREFS\tDEPS\tNAME\n");
	while (modstat(id, &st, 1) == 0) {
		if (verbose)
			full(&st);
		else
			printf("%d\t%d\t%d\t%s\n", st.ms_id, st.ms_refcnt,
			    st.ms_depcnt, st.ms_name);
		id = st.ms_id + 1;
	}
}

void
usage()
{
	fprintf(stderr, "usage: %s -l name|path ... | -u id ... | -U name ... |\n"
	    "\t-q id ... | -Q name ... | -s | -S | -d dir[:dir] | -D\n", progname);
	exit(2);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	int i, id;
	char *op;

	if (argc < 2 || argv[1][0] != '-' || strlen(argv[1]) != 2)
		usage();
	op = argv[1];
	switch (op[1]) {
	case 'l':
		for (i = 2; i < argc; i++)
			if ((id = modload(argv[i])) < 0)
				fail("cannot load", argv[i]);
			else
				printf("%s: loaded, id %d\n", argv[i], id);
		break;
	case 'u':
		for (i = 2; i < argc; i++)
			if (moduload(atoi(argv[i])) < 0)
				fail("cannot unload", argv[i]);
		break;
	case 'U':
		for (i = 2; i < argc; i++) {
			if ((id = byname(argv[i])) < 0) {
				errno = EINVAL;
				fail("cannot unload", argv[i]);
			} else if (moduload(id) < 0)
				fail("cannot unload", argv[i]);
		}
		break;
	case 'q':
		for (i = 2; i < argc; i++)
			query(atoi(argv[i]), argv[i]);
		break;
	case 'Q':
		for (i = 2; i < argc; i++)
			query(byname(argv[i]), argv[i]);
		break;
	case 's':
	case 'S':
		all(op[1] == 'S');
		break;
	case 'd':
		if (argc != 3)
			usage();
		if (modpath(argv[2]) < 0)
			fail("cannot set path", argv[2]);
		break;
	case 'D':
		if (modpath((char *)0) < 0)
			fail("cannot reset path", "");
		break;
	default:
		usage();
	}
	return status;
}
