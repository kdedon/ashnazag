/*
 * runall.c -- run every test program on the console and total the results.
 *
 *   /tests/runall [test[:seconds] ...]
 *
 * Mounts /proc, runs each test (the table below, or the names given) with a
 * timeout, then prints "TESTS DONE pass=N fail=M skip=S" counting result
 * lines.  A program that times out, dies on a signal, or exits nonzero
 * without a FAIL line adds one FAIL.  With t_mac76 in the run, "HALT
 * NEXT" comes first, and after the totals "t_mac76 halt" shuts the
 * machine down; "HALT END" means it did not.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

static struct {
	char *name;
	int secs;
} tests[] = {
	{ "t_proc", 150 },
	{ "t_mem", 90 },
	{ "t_file", 90 },
	{ "t_pipe", 90 },
	{ "t_sig", 90 },
	{ "t_time", 60 },
	{ "t_tty", 30 },
	{ "t_arith", 60 },
	{ "t_sys", 60 },
	{ "t_streams", 60 },
	{ "t_stress", 90 },
	{ "t_ufs", 300 },
	{ "t_page", 900 },
	{ "t_moddemo", 120 },
	{ "t_dlm", 90 },
	{ "t_otb", 300 },
	{ "t_gate", 150 },
	{ "t_aux", 300 },
	{ "t_mac", 330 },
	{ "t_mac76", 600 },
	{ "t_mac6", 330 },
	/* a second run in the same boot: nothing of the first may linger */
	{ "t_mac", 330 },
	{ "t_mac6", 330 },
	{ "t_vtop", 60 },
	{ "t_display", 360 },
	{ "t_tos", 700 },
	{ "t_mint", 300 },
	{ "t_cpm", 600 },
	{ "t_amiga", 680 },
	{ "t_env", 240 },
	{ 0, 0 }
};

static void
say(char *s)
{
	write(1, s, strlen(s));
}

static void
count(pass, fail, skip)
int *pass, *fail, *skip;
{
	char buf[256];
	int fd, n, i;

	*pass = *fail = *skip = 0;
	fd = open(COUNTFILE, O_RDONLY);
	if (fd < 0)
		return;
	while ((n = read(fd, buf, sizeof buf)) > 0)
		for (i = 0; i < n; i++)
			if (buf[i] == 'P')
				(*pass)++;
			else if (buf[i] == 'F')
				(*fail)++;
			else if (buf[i] == 'S')
				(*skip)++;
	close(fd);
}

static void
addfail()
{
	int fd = open(COUNTFILE, O_WRONLY | O_APPEND | O_CREAT, 0666);

	if (fd >= 0) {
		write(fd, "F", 1);
		close(fd);
	}
}

static int timedout;
static char *runarg;		/* the test's one argument, if any */

static void
onalrm(sig)
int sig;
{
	timedout = 1;
}

static int
runone(name, secs)
char *name;
int secs;
{
	char path[64], buf[256];
	pid_t pid;
	int st, r, p0, f0, s0, p1, f1, s1;
	long t0;

	sprintf(path, "/tests/%s", name);
	/* a GROUP run leaves out the tests it didn't choose */
	if (access(path, 0) < 0 && access("/tests/.group", 0) == 0)
		return 0;
	sprintf(buf, "=== %s\n", name);
	say(buf);
	count(&p0, &f0, &s0);
	t0 = t_now_ms();
	pid = fork();
	if (pid == -1) {
		sprintf(buf, "FAIL %s.run: fork: %s\n", name, strerror(errno));
		say(buf);
		addfail();
		return 1;
	}
	if (pid == 0) {
		execl(path, name, runarg, (char *)0);
		sprintf(buf, "FAIL %s.run: exec: %s\n", name, strerror(errno));
		say(buf);
		addfail();
		_exit(127);
	}
	timedout = 0;
	signal(SIGALRM, onalrm);
	alarm(secs);
	for (;;) {
		r = waitpid(pid, &st, 0);
		if (r == pid || (r == -1 && errno != EINTR))
			break;
		if (timedout) {
			kill(pid, SIGKILL);
			alarm(5);
			timedout = 0;
			r = waitpid(pid, &st, 0);
			sprintf(buf, "FAIL %s.run: timeout after %d s\n", name, secs);
			say(buf);
			addfail();
			alarm(0);
			return 1;
		}
	}
	alarm(0);
	count(&p1, &f1, &s1);
	if (r != pid) {
		sprintf(buf, "FAIL %s.run: waitpid: %s\n", name, strerror(errno));
		say(buf);
		addfail();
		return 1;
	}
	if (WIFSIGNALED(st)) {
		sprintf(buf, "FAIL %s.run: killed by signal %d%s\n", name, WTERMSIG(st),
		    (st & 0200) ? " (core)" : "");
		say(buf);
		addfail();
		return 1;
	}
	if (WEXITSTATUS(st) != 0 && f1 == f0) {
		sprintf(buf, "FAIL %s.run: exit %d without a FAIL line\n", name, WEXITSTATUS(st));
		say(buf);
		addfail();
		return 1;
	}
	if (p1 == p0 && f1 == f0 && s1 == s0) {
		sprintf(buf, "FAIL %s.run: no results\n", name);
		say(buf);
		addfail();
		return 1;
	}
	sprintf(buf, "--- %s: exit %d, pass %d fail %d skip %d, %ld ms\n", name,
	    WEXITSTATUS(st), p1 - p0, f1 - f0, s1 - s0, t_now_ms() - t0);
	say(buf);
	return WEXITSTATUS(st) != 0;
}

int
main(argc, argv)
int argc;
char **argv;
{
	struct utsname u;
	char buf[256];
	int i, pass, fail, skip, halt;
	long fm;

	sprintf(buf, "T_RUNNER=%ld", (long)getpid());
	putenv(strdup(buf));
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	umask(022);
	unlink(COUNTFILE);
	close(creat(COUNTFILE, 0666));
	if (mount("/proc", "/proc", MS_DATA, "proc", (char *)0, 0) == -1 && errno != EBUSY) {
		sprintf(buf, "INFO runall: mount /proc: %s\n", strerror(errno));
		say(buf);
	}
	uname(&u);
	sprintf(buf, "TESTS START %s %s %s %s %s\n", u.sysname, u.release, u.version,
	    u.machine, u.nodename);
	say(buf);
	fm = t_kmem("freemem");
	sprintf(buf, "INFO runall: freemem %ld pages at start\n", fm);
	say(buf);

	halt = access("/tests/t_mac76", 0) == 0;
	if (argc > 1) {
		for (halt = 0, i = 1; i < argc; i++) {
			/* name[:seconds] */
			char *c = strchr(argv[i], ':');

			if (c)
				*c++ = 0;
			halt |= strcmp(argv[i], "t_mac76") == 0;
			runone(argv[i], c ? atoi(c) : 120);
		}
	} else {
		for (i = 0; tests[i].name != 0; i++)
			runone(tests[i].name, tests[i].secs);
	}

	sync();
	fm = t_kmem("freemem");
	sprintf(buf, "INFO runall: freemem %ld pages at end\n", fm);
	say(buf);
	count(&pass, &fail, &skip);
	if (halt)
		say("HALT NEXT\n");
	sprintf(buf, "TESTS DONE pass=%d fail=%d skip=%d\n", pass, fail, skip);
	say(buf);
	if (halt) {
		runarg = "halt";
		runone("t_mac76", 600);
		say("HALT END\n");
	}
	return fail != 0;
}
