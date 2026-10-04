/*
 * t_aslm.c -- mkaslm built for this system: round trip and layout checks
 * of a library it built and of the CD's AppleTalk library, then both run
 * in memory (-x): entry selectors, export registration, calls; malformed
 * copies are refused without a crash on this 32-bit build.  Apple's
 * ddp library also runs as far as it can without the Toolbox.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include "t.h"

#define	D	"/tests/n0n2/"

static int
run(name, a1, a2, a3)
	char *name, *a1, *a2, *a3;
{
	int pid, st;

	fflush(stdout);
	if ((pid = fork()) == 0) {
		execl(D "mkaslm", "mkaslm", a1, a2, a3, (char *)0);
		_exit(127);
	}
	if (pid < 0 || t_waitchild(pid, &st, 60) < 0)
		return t_check(name, 0, "%s", T_ERR);
	return t_check(name, WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %x", st);
}

int
main()
{
	t_init("aslm", 280);
	if (access(D "mkaslm", 0) < 0) {
		t_skip("all", "no " D "mkaslm");
		return t_done();
	}
	run("ours_roundtrip", "-t", D "auxtest.bin", (char *)0);
	run("ours_layout", "-l", D "auxtest.bin", (char *)0);
	run("ours_run", "-x", D "auxtest.bin", (char *)0);
	{
		DIR *d = opendir(D);
		struct dirent *e;
		char path[256];
		int n = 0, bad = 0, pid, st;

		while (d && (e = readdir(d)) != 0) {
			if (strncmp(e->d_name, "bad", 3) != 0)
				continue;
			sprintf(path, "%s%s", D, e->d_name);
			n++;
			if ((pid = fork()) == 0) {
				close(1);
				close(2);
				execl(D "mkaslm", "mkaslm", "-l", path, (char *)0);
				_exit(127);
			}
			if (pid < 0 || t_waitchild(pid, &st, 30) < 0 || !WIFEXITED(st) ||
			    WEXITSTATUS(st) > 2) {
				t_info("hostile", "%s: status %x", e->d_name, st);
				bad++;
			}
		}
		if (d)
			closedir(d);
		t_check("hostile", n > 0 && bad == 0, "%d of %d files", bad, n);
	}
	if (access(D "atalk.bin", 0) == 0) {
		run("ot_roundtrip", "-t", D "atalk.bin", (char *)0);
		run("ot_layout", "-l", D "atalk.bin", (char *)0);
		/* Apple's code calls the Toolbox: how far it gets is reported */
		fflush(stdout);
		if (system(D "mkaslm -x " D "atalk.bin 'OTLib$ddp'") != 0)
			t_info("ot_ddp_run", "stops outside a Mac environment (see above)");
	} else
		t_skip("ot", "no AppleTalk library");
	return t_done();
}
