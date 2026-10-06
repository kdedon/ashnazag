/*
 * t_cpm.c -- CP/M-68K as a Unix process: /cpm/bin/startcpm.
 *
 * Registers tosguest (as t_mint does) and drives startcpm -e t through
 * pipes: a new A: from the distribution, the A> prompt, DIR, TYPE of a
 * file on B: (a host directory), a program from B:, DRI's STAT, a copy
 * with PIP, a second session of the same environment refused, EXIT;
 * then a second boot finds PIP's copy on A: and ends at end of input.
 * Skips without guest support, the module, startcpm or CP/M.
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
#include "sys/mod.h"
#include "tosio.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	START	"/cpm/bin/startcpm"
#define	H	"/tmp/cpmh"
#define	E	H "/CPM/t"

static char out[32768];
static int nout, in, from;
static pid_t pid;

static pid_t
start(fd0, fd1)
	int fd0, fd1;
{
	pid_t p;

	if ((p = fork()) == 0) {
		setpgid(0, 0);
		putenv("HOME=" H);
		dup2(fd0, 0);
		dup2(fd1, 1);
		dup2(fd1, 2);
		for (fd0 = 3; fd0 < 64; fd0++)	/* the input's write end: EOF */
			close(fd0);
		execl(START, "startcpm", "-e", "t", (char *)0);
		_exit(126);
	}
	if (p > 0)
		setpgid(p, p);
	return p;
}

/* a session on pipes: in is its input, out collects what it writes */
static int
boot()
{
	static int ofd = -1;
	int i[2], o[2];

	if (ofd >= 0)
		close(ofd);
	if (pipe(i) < 0 || pipe(o) < 0)
		return -1;
	pid = start(i[0], o[1]);
	close(i[0]);
	close(o[1]);
	in = i[1];
	ofd = o[0];
	nout = from = 0;
	out[0] = 0;
	return pid < 0 ? -1 : ofd;
}

/* output since the last command until s appears; 1 if it did */
static int
waitfor(fd, s, ms)
	int fd, ms;
	char *s;
{
	struct pollfd pf;
	long end = t_now_ms() + ms;
	int k;

	pf.fd = fd;
	pf.events = POLLIN;
	while (strstr(out + from, s) == 0) {
		if (t_now_ms() > end || nout >= sizeof out - 1)
			return 0;
		if (poll(&pf, 1, 1000) <= 0)
			continue;
		if ((k = read(fd, out + nout, sizeof out - 1 - nout)) <= 0)
			return 0;
		nout += k;
		out[nout] = 0;
	}
	return 1;
}

/* a command line; its output, up to the next prompt, starts at out + from */
static int
cmd(fd, line, name)
	int fd;
	char *line, *name;
{
	char b[128];

	from = nout;
	sprintf(b, "%s\r", line);
	write(in, b, strlen(b));
	if (waitfor(fd, "\nA>", 60000))
		return 1;
	t_info(name, "no prompt after %s; output \"%.400s\"", line, out + from);
	return 0;
}

static void
put(path, s, n)
	char *path, *s;
	int n;
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd >= 0) {
		write(fd, s, n);
		close(fd);
	}
}

static void
clean()
{
	static char *f[] = { E "/a.img", E "/.env", E "/lst.txt", E "/b/README.TXT",
	    E "/b/HELLO.68K", 0 };
	int i;

	for (i = 0; f[i]; i++)
		unlink(f[i]);
	rmdir(E "/b");
	rmdir(E);
	rmdir(H "/CPM");
	rmdir(H);
}

static void
second()
{
	int fd, st = -1;
	pid_t p;

	fd = open("/dev/null", O_RDWR);
	if ((p = start(fd, fd)) > 0 && t_waitchild(p, &st, 30) < 0)
		st = -1;
	close(fd);
	t_check("one_session", WIFEXITED(st) && WEXITSTATUS(st) == 1, "status %#x", st);
}

int
main()
{
	struct mod_mreg reg;
	struct stat sb;
	char buf[8192];
	int mj = TOS_MAJOR, fd, ofd, st, n;

	t_init("cpm", 600);
	if (t_kmem("guest_loading") == -1) {
		t_skip("all", "kernel has no guest support");
		return t_done();
	}
	if (stat(MD "/tosguest", &sb) < 0 || stat("/dev/tos", &sb) < 0 ||
	    stat(START, &sb) < 0 || stat("/cpm/sys/CPM.SYS", &sb) < 0) {
		t_skip("all", "no tosguest module, startcpm or CP/M-68K on this root");
		return t_done();
	}
	modpath(MD);
	strcpy(reg.md_modname, "tosguest");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "modadm: %s", T_ERR))
		return t_done();

	clean();
	mkdir(H, 0755);
	mkdir(H "/CPM", 0755);
	mkdir(E, 0755);
	mkdir(E "/b", 0755);
	put(E "/b/README.TXT", "HELLO FROM B:\r\n\032", 16);
	if ((fd = open("/tests/cpm/hello.68k", O_RDONLY)) >= 0) {
		n = read(fd, buf, sizeof buf);
		close(fd);
		put(E "/b/HELLO.68K", buf, n);
	}

	if ((ofd = boot()) < 0) {
		t_fail("boot", "pipe or fork: %s", T_ERR);
		return t_done();
	}
	if (!t_check("boot", waitfor(ofd, "A>", 120000) && strstr(out, "made A:"),
	    "output \"%.400s\"", out)) {
		kill(-pid, SIGKILL);
		t_waitchild(pid, &st, 10);
		return t_done();
	}
	t_check("a_img", stat(E "/a.img", &sb) == 0 && sb.st_size == 8L << 20,
	    "a.img: %s, %ld bytes", T_ERR, (long)sb.st_size);
	if (cmd(ofd, "DIR", "dir"))
		t_check("dir", strstr(out + from, "STAT") && strstr(out + from, "PIP") &&
		    strstr(out + from, "EXIT"), "output \"%.400s\"", out + from);
	if (cmd(ofd, "TYPE B:README.TXT", "type"))
		t_check("type", strstr(out + from, "HELLO FROM B:") != 0,
		    "output \"%.400s\"", out + from);
	if (cmd(ofd, "B:HELLO", "run"))
		t_check("run", strstr(out + from, "HELLO FROM CP/M-68K") != 0,
		    "output \"%.400s\"", out + from);
	if (cmd(ofd, "STAT", "stat"))
		t_check("stat", strstr(out + from, "SPACE") != 0, "output \"%.400s\"", out + from);
	if (cmd(ofd, "PIP A:COPY.TXT=B:README.TXT", "pip"))
		t_check("pip", cmd(ofd, "TYPE COPY.TXT", "pip") &&
		    strstr(out + from, "HELLO FROM B:") != 0, "output \"%.400s\"", out + from);
	second();
	from = nout;
	write(in, "EXIT\r", 5);
	st = -1;
	if (t_waitchild(pid, &st, 30) < 0)
		st = -1;
	t_check("exit", WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %#x, output \"%.300s\"",
	    st, out + from);
	close(in);

	if ((ofd = boot()) >= 0 && t_check("reboot", waitfor(ofd, "A>", 60000) &&
	    !strstr(out, "made A:"), "output \"%.400s\"", out)) {
		if (cmd(ofd, "TYPE COPY.TXT", "kept"))
			t_check("kept", strstr(out + from, "HELLO FROM B:") != 0,
			    "output \"%.400s\"", out + from);
	}
	close(in);
	st = -1;
	if (pid > 0 && t_waitchild(pid, &st, 30) < 0)
		st = -1;
	t_check("eof_ends", WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %#x", st);
	if (pid > 0)
		kill(-pid, SIGKILL);
	clean();
	return t_done();
}
