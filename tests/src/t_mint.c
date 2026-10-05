/*
 * t_mint.c -- MiNT programs as Unix processes: /tos/bin/mintrun.
 *
 * Registers tosguest (as t_tos does) and runs MiNTLib test programs
 * from /tests/mint: hello world, cat and a directory listing, exit
 * status, a pipe between forked processes, signals, exec of another
 * program, and bash from /tos/mint/bin when present; on the network
 * root also an echo at 10.0.2.100:7 and a refused connect.  Skips
 * without guest support, the module, mintrun or the programs.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "sys/mod.h"
#include "tosio.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	MINTRUN	"/tos/bin/mintrun"
#define	PRG	"/tests/mint/"

static char out[8192];
static int trace;
static char *last[4];

/* mintrun with args; its stdout and stderr in out, its wait status */
static int
run(a1, a2, a3, a4)
	char *a1, *a2, *a3, *a4;
{
	int p[2], n = 0, k, st = -1;
	pid_t pid;
	struct pollfd pf;
	long end = t_now_ms() + 30000;

	out[0] = 0;
	last[0] = a1, last[1] = a2, last[2] = a3, last[3] = a4;
	if (pipe(p) < 0)
		return -1;
	if ((pid = fork()) == 0) {
		setpgid(0, 0);			/* its group kills stay off the runner */
		if (trace)
			putenv(trace > 1 ? "MINTTRACE=2" : "MINTTRACE=1");
		close(p[0]);
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[1]);
		execl(MINTRUN, "mintrun", a1, a2, a3, a4, (char *)0);
		_exit(126);
	}
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		return -1;
	}
	setpgid(pid, pid);			/* before the child's own, for the kill below */
	pf.fd = p[0];
	pf.events = POLLIN;
	while (n < sizeof out - 1 && t_now_ms() < end) {
		if (poll(&pf, 1, 1000) <= 0)
			continue;
		if ((k = read(p[0], out + n, sizeof out - 1 - n)) <= 0)
			break;
		n += k;
	}
	out[n] = 0;
	close(p[0]);
	if (t_waitchild(pid, &st, 10) < 0)
		st = -1;
	kill(-pid, SIGKILL);			/* what it left behind */
	return st;
}

/* on failure the run again with every call logged */
static void
expect(name, st, code, want)
	char *name, *want;
	int st, code;
{
	int n, k, w;

	if (t_check(name, WIFEXITED(st) && WEXITSTATUS(st) == code && strcmp(out, want) == 0,
	    "status %#x, output \"%.300s\"", st, out))
		return;
	for (trace = 1; trace <= 2; trace++) {
		st = run(last[0], last[1], last[2], last[3]);
		t_info(name, "MINTTRACE=%d: status %#x", trace, st);
		for (n = 0, k = strlen(out); n < k; n += w)
			if ((w = write(1, out + n, k - n)) <= 0)
				break;
	}
	trace = 0;
}

static void
sockets()
{
	int st;

	st = run(PRG "msock.prg", "echo", "10.0.2.100", "7");
	expect("sock_echo", st, 0, "got mint echo\n");
	st = run(PRG "msock.prg", "refused", "10.0.2.2", "1");
	expect("sock_refused", st, 0, "refused\n");
}

/*
 * A lone guest claims nothing: the session's owner is unchanged, an
 * ENTER for the machine is refused, and with no session the input
 * call finds none.
 */
static void
solo()
{
	struct tosenter te;
	struct tosowner to, t0;
	struct tosinput ti;
	int fd, st, r = 0;
	pid_t pid;

	if ((pid = fork()) == 0) {
		if ((fd = open("/dev/tos", O_RDWR)) < 0 || ioctl(fd, TOSIOC_OWNER, &t0) < 0)
			_exit(1);
		te.te_ramsize = 0x80000;
		te.te_flags = TEF_NOMACH;
		if (ioctl(fd, TOSIOC_ENTER, &te) < 0)
			_exit(2);
		if (ioctl(fd, TOSIOC_OWNER, &to) < 0 || to.to_pid != t0.to_pid)
			_exit(3);
		te.te_flags = 0;
		if (ioctl(fd, TOSIOC_ENTER, &te) == 0 || errno != EBUSY)
			_exit(4);
		if (ioctl(fd, TOSIOC_OWNER, &to) < 0 || to.to_pid != t0.to_pid)
			_exit(5);
		ti.ti_n = 0;
		if (t0.to_pid == 0 && (ioctl(fd, TOSIOC_INPUT, &ti) == 0 || errno != ENXIO))
			_exit(6);
		_exit(0);
	}
	if (pid < 0 || t_waitchild(pid, &st, 10) < 0)
		r = -1;
	t_check("solo_claims_nothing", r == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
	    "status %#x", st);
}

int
main()
{
	struct mod_mreg reg;
	struct stat sb;
	int mj = TOS_MAJOR, fd, st;

	t_init("mint", 300);
	if (t_kmem("guest_loading") == -1) {
		t_skip("all", "kernel has no guest support");
		return t_done();
	}
	if (stat(MD "/tosguest", &sb) < 0 || stat("/dev/tos", &sb) < 0 ||
	    stat(MINTRUN, &sb) < 0) {
		t_skip("all", "no tosguest module or no mintrun on this root");
		return t_done();
	}
	if (stat(PRG "mhello.prg", &sb) < 0) {
		t_skip("all", "no MiNT test programs (no MiNT cross tools)");
		return t_done();
	}
	modpath(MD);
	strcpy(reg.md_modname, "tosguest");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "modadm: %s", T_ERR))
		return t_done();
	solo();

	st = run(PRG "mhello.prg", (char *)0, (char *)0, (char *)0);
	expect("hello", st, 0, "hello mint\n");

	mkdir("/tmp/mintd", 0755);
	mkdir("/tmp/mintd/sub", 0755);
	if ((fd = creat("/tmp/mintd/a", 0644)) >= 0) {
		write(fd, "abc", 3);
		close(fd);
	}
	st = run(PRG "mcat.prg", "/tmp/mintd/a", (char *)0, (char *)0);
	expect("cat", st, 0, "abc");
	st = run(PRG "mcat.prg", "u:\\host\\tmp\\mintd\\a", (char *)0, (char *)0);
	expect("cat_upath", st, 0, "abc");
	st = run(PRG "mcat.prg", "-l", "/tmp/mintd", (char *)0);
	t_check("ls", WIFEXITED(st) && WEXITSTATUS(st) == 0 && strstr(out, "a 3 -\n") &&
	    strstr(out, "sub ") && strstr(out, " d\n"), "status %#x, output \"%.300s\"", st, out);
	st = run(PRG "mcat.prg", "/tmp/mintd/none", (char *)0, (char *)0);
	t_check("cat_enoent", WIFEXITED(st) && WEXITSTATUS(st) == 1 && strstr(out, "No such file"),
	    "status %#x, output \"%.300s\"", st, out);
	unlink("/tmp/mintd/a");
	rmdir("/tmp/mintd/sub");
	rmdir("/tmp/mintd");

	st = run(PRG "mproc.prg", "exit", "3", (char *)0);
	expect("exit3", st, 3, "");
	st = run(PRG "mproc.prg", "pipe", (char *)0, (char *)0);
	expect("pipe", st, 0, "read through the pipe\nchild 7\n");
	st = run(PRG "mproc.prg", "sig", (char *)0, (char *)0);
	expect("signal", st, 0, "caught 1\nkilled 1\n");
	st = run(PRG "mproc.prg", "exec", PRG "mhello.prg", (char *)0);
	expect("exec", st, 0, "hello mint\nstatus 0\n");

	if (stat("/etc/inet/strcf", &sb) == 0)
		sockets();
	else
		t_skip("sock", "no network");
	if (stat("/tos/mint/bin/bash", &sb) < 0)
		t_skip("bash", "no /tos/mint/bin/bash");
	else {
		st = run("/tos/mint/bin/bash", "-c", "echo ok; exit 3", (char *)0);
		expect("bash", st, 3, "ok\n");
	}
	return t_done();
}
