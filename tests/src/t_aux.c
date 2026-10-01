/*
 * t_aux.c -- A/UX COFF programs as guest processes.
 *
 * Registers the auxexec module for magic 0x150 (modules in
 * /tests/aux/mod.d), then runs A/UX 3.1 programs from /aux/bin: the
 * Bourne shell on scripts (builtins, loops, redirection, command
 * substitution, pipes, exit status, native and A/UX children, traps
 * and kill), ls (-l: stat layout; errors), cp, mv, rm, awk.  With the
 * shared-library loader and the BSD calls (auxcore exports aux_select):
 * abi, a freestanding A/UX program checking the calls one by one;
 * sleep (libc1_s, SVR3 signal frame); more and vi on a pty with
 * ptem/ldterm/ttcompat and TERM=vt100.  Skips without guest support in
 * the kernel or without the A/UX binaries, which are local only.
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
#include <poll.h>
#include <signal.h>
#include <termio.h>
#include <stropts.h>
#include "sys/mod.h"
#include "t.h"

extern char *ptsname();
extern int getksym();

#define	MD	"/tests/aux/mod.d"
#define	SH	"/aux/bin/sh"
#define	OUT	"/tmp/aux.out"

static char *env[] = { "PATH=/aux/bin:/usr/bin:/sbin", "HOME=/tmp", 0 };
static char out[4096];

/* run argv with stdout and stderr to OUT; returns wait status, -1 on timeout */
static int
run(argv, secs)
char **argv;
int secs;
{
	int st, fd, n;
	pid_t pid;

	unlink(OUT);
	pid = fork();
	if (pid == 0) {
		fd = open(OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(fd, 1);
		dup2(fd, 2);
		close(fd);
		execve(argv[0], argv, env);
		_exit(127);
	}
	if (pid < 0 || t_waitchild(pid, &st, secs) < 0)
		st = -1;
	out[0] = 0;
	fd = open(OUT, O_RDONLY);
	if (fd >= 0) {
		n = read(fd, out, sizeof out - 1);
		out[n > 0 ? n : 0] = 0;
		close(fd);
	}
	return st;
}

static int
script(name, text, secs)
char *name, *text;
int secs;
{
	char path[64], *argv[3];
	int fd;

	sprintf(path, "/tmp/aux_%s.sh", name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	write(fd, text, strlen(text));
	close(fd);
	argv[0] = SH;
	argv[1] = path;
	argv[2] = 0;
	return run(argv, secs);
}

/* output with newlines shown, for FAIL lines */
static char *
shown()
{
	static char b[600];
	int i, j;

	for (i = j = 0; out[i] && j < sizeof b - 3; i++)
		if (out[i] == '\n') {
			b[j++] = '\\';
			b[j++] = 'n';
		} else
			b[j++] = out[i];
	b[j] = 0;
	return b;
}

static char s_basic[] =
	"echo start\n"
	"for i in 1 2 3; do echo \"i $i\"; done\n"
	"case abc in a*) echo case-ok;; *) echo case-bad;; esac\n"
	"x=`echo sub`; echo \"cmd $x\"\n"
	"mkdir /tmp/auxt\n"
	"echo line1 > /tmp/auxt/f1; echo line2 >> /tmp/auxt/f1\n"
	"while read l; do echo \"read $l\"; done < /tmp/auxt/f1\n"
	"if test -f /tmp/auxt/f1; then echo test-f; fi\n"
	"if [ -d /tmp/auxt ]; then echo test-d; fi\n"
	"(exit 3); echo \"status $?\"\n"
	"/usr/bin/echo native\n"
	"/aux/bin/sh -c 'echo nested'\n"
	"cp /tmp/auxt/f1 /tmp/auxt/f2; mv /tmp/auxt/f2 /tmp/auxt/f3; ls /tmp/auxt\n"
	"awk '{ print NR \": \" $0 }' /tmp/auxt/f3\n"
	"ls /tmp/auxt | awk '{ n++ } END { print n \" files\" }'\n"
	"rm /tmp/auxt/f3; ls /tmp/auxt\n"
	"rm /tmp/auxt/f1; rmdir /tmp/auxt; ls -d /tmp/auxt 2>/dev/null || echo gone\n"
	"echo done\n";
static char x_basic[] =
	"start\ni 1\ni 2\ni 3\ncase-ok\ncmd sub\nread line1\nread line2\n"
	"test-f\ntest-d\nstatus 3\nnative\nnested\nf1\nf3\n1: line1\n2: line2\n"
	"2 files\nf1\ngone\ndone\n";

static char s_sig[] =
	"trap 'echo caught 15' 15\n"
	"kill -15 $$\n"
	"echo after\n"
	"(sh -c 'kill -9 $$'; echo \"killed $?\") 2>/dev/null\n"
	"trap 'echo usr1' 16\n"
	"kill -16 $$\n"
	"echo end\n";
static char x_sig[] = "caught 15\nafter\nkilled 137\nusr1\nend\n";

static char s_ls[] =
	"echo 0123456789 > /tmp/aux_g\n"
	"ls -l /tmp/aux_g\n"
	"ls /tmp/aux_nonexistent\n"
	"echo \"status $?\"\n"
	"rm /tmp/aux_g\n";

/* ---- pty sessions ---- */

static char *tenv[] = { "PATH=/aux/bin:/usr/bin", "HOME=/tmp", "TERM=vt100", 0 };
static char tbuf[8192];
static int tlen;

/* a pty pair with ptem, ldterm and ttcompat on the slave, 24x80 */
static int
openpty(mp, sp, name)
int *mp, *sp;
char *name;
{
	struct winsize ws;
	char *n;
	int m, s;
	pid_t pid;

	if ((m = open("/dev/ptmx", O_RDWR)) < 0)
		return -1;
	pid = getpid();
	grantpt(m);
	if (getpid() != pid)
		_exit(1);
	if (unlockpt(m) < 0 || (n = ptsname(m)) == 0 ||
	    (s = open(n, O_RDWR | O_NOCTTY)) < 0) {
		close(m);
		return -1;
	}
	if (ioctl(s, I_PUSH, "ptem") < 0 || ioctl(s, I_PUSH, "ldterm") < 0 ||
	    ioctl(s, I_PUSH, "ttcompat") < 0) {
		close(s);
		close(m);
		return -1;
	}
	ws.ws_row = 24;
	ws.ws_col = 80;
	ws.ws_xpixel = ws.ws_ypixel = 0;
	ioctl(s, TIOCSWINSZ, &ws);
	strcpy(name, n);
	*mp = m;
	*sp = s;
	return 0;
}

/* argv in a new session on the slave as its controlling terminal */
static pid_t
spawn(name, argv)
char *name, **argv;
{
	pid_t pid;
	int fd, i;

	tlen = 0;
	tbuf[0] = 0;
	if ((pid = fork()) != 0)
		return pid;
	setsid();
	if ((fd = open(name, O_RDWR)) < 0)
		_exit(126);
	for (i = 0; i < 3; i++)
		dup2(fd, i);
	for (i = 3; i < 20; i++)
		close(i);
	execve(argv[0], argv, tenv);
	_exit(127);
}

/*
 * Collect the master's output until it has been quiet for quiet ms
 * after some output, or for max ms in all.  NULs become spaces.
 */
static void
drain(m, quiet, max)
int m, quiet, max;
{
	struct pollfd pf;
	long t0 = t_now_ms(), last = t0, now;
	int n, i, got = 0;

	for (;;) {
		if (tlen > sizeof tbuf - 512) {
			memmove(tbuf, tbuf + tlen - 2048, 2048);
			tlen = 2048;
		}
		pf.fd = m;
		pf.events = POLLIN;
		pf.revents = 0;
		if (poll(&pf, 1, 50) == 1) {
			if (!(pf.revents & POLLIN))
				break;
			n = read(m, tbuf + tlen, sizeof tbuf - 1 - tlen);
			if (n <= 0)
				break;
			for (i = 0; i < n; i++)
				if (tbuf[tlen + i] == 0)
					tbuf[tlen + i] = ' ';
			tlen += n;
			got = 1;
			last = t_now_ms();
			continue;
		}
		now = t_now_ms();
		if ((got && now - last >= quiet) || now - t0 >= max)
			break;
	}
	tbuf[tlen] = 0;
}

/* the collected output, escapes shown, for FAIL lines */
static char *
tshown()
{
	static char b[400];
	int i, j;
	char *p = tlen > 300 ? tbuf + tlen - 300 : tbuf;

	for (i = j = 0; p[i] && j < sizeof b - 5; i++)
		if (p[i] == 033) {
			b[j++] = '\\';
			b[j++] = 'e';
		} else if (p[i] == '\r' || p[i] == '\n') {
			b[j++] = '\\';
			b[j++] = p[i] == '\r' ? 'r' : 'n';
		} else if ((p[i] & 0xff) < ' ')
			b[j++] = '.';
		else
			b[j++] = p[i];
	b[j] = 0;
	return b;
}

static int
putfile(path, text)
char *path, *text;
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0)
		return -1;
	write(fd, text, strlen(text));
	close(fd);
	return 0;
}

/*
 * abi: its stdout to OUT, the pty master and slave as fds 3 and 4;
 * each "P name", "F name: why", "S name: why" line becomes a result.
 */
static void
test_abi()
{
	char name[32], sub[100], *l, *e, *why;
	static char *av[] = { "/aux/bin/abi", 0, 0 };
	unsigned long v, info;
	int m = -1, s = -1, st, fd, n, done = 0;
	pid_t pid;

	t_rearm(90);
	/* quadrant-1 transfers only where vtop honours the process */
	if (getksym("vtop_orig", &v, &info) == 0)
		av[1] = "q1";
	if (openpty(&m, &s, name) < 0)
		t_info("abi_pty", "no pty: %s", T_ERR);
	unlink(OUT);
	if ((pid = fork()) == 0) {
		fd = open(OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(fd, 1);
		dup2(fd, 2);
		if (m >= 0) {
			dup2(m, 3);
			dup2(s, 4);
		}
		for (fd = 5; fd < 20; fd++)
			close(fd);
		execve(av[0], av, env);
		_exit(127);
	}
	if (pid < 0 || t_waitchild(pid, &st, 60) < 0)
		st = -1;
	if (m >= 0) {
		close(s);
		close(m);
	}
	out[0] = 0;
	if ((fd = open(OUT, O_RDONLY)) >= 0) {
		n = read(fd, out, sizeof out - 1);
		out[n > 0 ? n : 0] = 0;
		close(fd);
	}
	for (l = out; *l; l = *e ? e + 1 : e) {
		for (e = l; *e && *e != '\n'; e++)
			;
		if (e - l == 4 && strncmp(l, "done", 4) == 0)
			done = 1;
		if (e - l < 3 || e - l >= sizeof sub - 5 || l[1] != ' ')
			continue;
		sprintf(sub, "abi_%.*s", (int)(e - l - 2), l + 2);
		if ((why = strchr(sub, ':')) != 0) {
			*why++ = 0;
			while (*why == ' ')
				why++;
		} else
			why = "";
		if (l[0] == 'P')
			t_pass(sub);
		else if (l[0] == 'S')
			t_skip(sub, "%s", why);
		else
			t_fail(sub, "%s", why);
	}
	t_check("abi_run", done && st != -1 && WIFEXITED(st), "status %#x, output '%s'", st,
	    shown());
}

/* /aux/bin/sleep 1: libc1_s, signal() and pause() with the SVR3 frame */
static void
test_sleep()
{
	char *argv[3];
	long t0, t;
	int st;

	argv[0] = "/aux/bin/sleep";
	argv[1] = "1";
	argv[2] = 0;
	t0 = t_now_ms();
	st = run(argv, 20);
	t = t_now_ms() - t0;
	/* the A/UX alarm is never early: sleep 1 takes at least 1 s */
	t_check("sleep_1", st == 0 && t >= 1000 && t < 4000, "status %#x, %ld ms, output '%s'",
	    st, t, shown());
}

/* more on a 24-line vt100: first page, a prompt, the next page, q */
static void
test_more()
{
	char name[32], txt[1200], *p, *argv[3];
	int m, s, st, i;
	pid_t pid;

	t_rearm(90);
	if (openpty(&m, &s, name) < 0) {
		t_skip("more", "no pty: %s", T_ERR);
		return;
	}
	for (p = txt, i = 1; i <= 60; i++)
		p += sprintf(p, "line %d\n", i);
	putfile("/tmp/aux_more.txt", txt);
	argv[0] = "/aux/bin/more";
	argv[1] = "/tmp/aux_more.txt";
	argv[2] = 0;
	pid = spawn(name, argv);
	drain(m, 700, 15000);
	t_check("more_page", strstr(tbuf, "line 1\r\n") && strstr(tbuf, "line 20\r\n") &&
	    !strstr(tbuf, "line 30\r\n") && strstr(tbuf, "More"), "output '%s'", tshown());
	write(m, " ", 1);
	drain(m, 700, 15000);
	t_check("more_next", strstr(tbuf, "line 40\r\n") != 0, "output '%s'", tshown());
	write(m, "q", 1);
	drain(m, 500, 5000);
	if (t_waitchild(pid, &st, 15) < 0)
		st = -1;
	t_check("more_quit", st == 0, "status %#x, output '%s'", st, tshown());
	close(s);
	close(m);
	unlink("/tmp/aux_more.txt");
}

/* vi: append a line, :wq, check the file */
static void
test_vi()
{
	char name[32], b[200], *argv[3];
	int m, s, st, fd, n;
	pid_t pid;

	t_rearm(90);
	if (openpty(&m, &s, name) < 0) {
		t_skip("vi", "no pty: %s", T_ERR);
		return;
	}
	putfile("/tmp/aux_vi.txt", "line one\nline two\n");
	argv[0] = "/aux/bin/vi";
	argv[1] = "/tmp/aux_vi.txt";
	argv[2] = 0;
	pid = spawn(name, argv);
	drain(m, 1000, 20000);
	t_check("vi_screen", strstr(tbuf, "line one") && strstr(tbuf, "line two") &&
	    strstr(tbuf, "\033["), "output '%s'", tshown());
	write(m, "Gohello from vi\033", 16);
	drain(m, 700, 10000);
	write(m, ":wq\r", 4);
	drain(m, 700, 10000);
	if (t_waitchild(pid, &st, 20) < 0)
		st = -1;
	b[0] = 0;
	if ((fd = open("/tmp/aux_vi.txt", O_RDONLY)) >= 0) {
		n = read(fd, b, sizeof b - 1);
		b[n > 0 ? n : 0] = 0;
		close(fd);
	}
	t_check("vi_edit", st == 0 && strcmp(b, "line one\nline two\nhello from vi\n") == 0,
	    "status %#x, file '%s', output '%s'", st, b, tshown());
	close(s);
	close(m);
	unlink("/tmp/aux_vi.txt");
}

/* ---- shared-library checks ---- */

static char img[16384], lib[100000];

/* src with its first .lib record naming libpath, as dst (mode 755) */
static int
relib(src, dst, libpath)
char *src, *dst, *libpath;
{
	int fd, n, i, opt, ns;
	char *sh;

	if ((fd = open(src, O_RDONLY)) < 0)
		return -1;
	n = read(fd, img, sizeof img);
	close(fd);
	if (n < 20)
		return -1;
	ns = (img[2] & 0xff) << 8 | (img[3] & 0xff);
	opt = (img[16] & 0xff) << 8 | (img[17] & 0xff);
	for (i = 0; i < ns; i++) {
		sh = img + 20 + opt + 40 * i;
		if (sh + 40 <= img + n && strncmp(sh, ".lib", 8) == 0) {
			long off = *(long *)(sh + 20);

			if (off + 64 > n)
				return -1;
			memset(img + off, 0, 64);
			strcpy(img + off, libpath);
			fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
			write(fd, img, n);
			close(fd);
			return 0;
		}
	}
	return -1;
}

/* exec path in a child: its errno if exec fails, else the wait status */
static int
tryexec(path, how)
char *path;
int *how;
{
	char *argv[3];
	int st;
	pid_t pid;

	argv[0] = path;
	argv[1] = "0";
	argv[2] = 0;
	if ((pid = fork()) == 0) {
		execve(path, argv, env);
		_exit(errno < 128 ? 128 + errno : 255);
	}
	if (pid < 0 || t_waitchild(pid, &st, 20) < 0)
		return *how = -1;
	*how = WIFEXITED(st) && WEXITSTATUS(st) >= 128 ? 1 : 0;
	return *how ? WEXITSTATUS(st) - 128 : st;
}

/*
 * Libraries are checked before the old image goes (errno back to the
 * caller); a mapping failure after that kills the process.
 */
static void
test_libs()
{
	int r, how, fd, n;

	t_rearm(60);
	if (relib("/aux/bin/sleep", "/tmp/aux_nolib", "/tmp/aux_nonexistent") < 0) {
		t_skip("lib_checks", "no .lib in /aux/bin/sleep");
		return;
	}
	r = tryexec("/tmp/aux_nolib", &how);
	t_check("lib_missing", how == 1 && r == ELIBACC, "exec %s %d", how == 1 ? "errno" : "status", r);
	relib("/aux/bin/sleep", "/tmp/aux_rellib", "shlib/libc1_s");
	chdir("/");
	r = tryexec("/tmp/aux_rellib", &how);
	t_check("lib_relative", how == 1 && r == ELIBACC, "exec %s %d", how == 1 ? "errno" : "status", r);
	relib("/aux/bin/sleep", "/tmp/aux_badlib", "/usr/bin/echo");
	r = tryexec("/tmp/aux_badlib", &how);
	t_check("lib_notcoff", how == 1 && r == ELIBBAD, "exec %s %d", how == 1 ? "errno" : "status", r);

	/* a copy of libc1_s whose bss leaves the quadrant: execmap fails */
	if ((fd = open("/shlib/libc1_s", O_RDONLY)) >= 0) {
		n = read(fd, lib, sizeof lib);
		close(fd);
		if (n > 20 + 28 + 3 * 40 && strncmp(lib + 20 + 28 + 80, ".bss", 4) == 0) {
			*(long *)(lib + 20 + 28 + 80 + 16) = 0x40000000;
			fd = open("/tmp/aux_biglib", O_WRONLY | O_CREAT | O_TRUNC, 0755);
			write(fd, lib, n);
			close(fd);
			relib("/aux/bin/sleep", "/tmp/aux_bigbss", "/tmp/aux_biglib");
			r = tryexec("/tmp/aux_bigbss", &how);
			t_check("lib_mapfail", how == 0 && WIFSIGNALED(r) && WTERMSIG(r) == SIGKILL,
			    "exec %s %#x", how == 1 ? "errno" : "status", r);
		}
	}
	unlink("/tmp/aux_nolib");
	unlink("/tmp/aux_rellib");
	unlink("/tmp/aux_badlib");
	unlink("/tmp/aux_biglib");
	unlink("/tmp/aux_bigbss");
}

static void
modules(pass, what)
int pass;
char *what;
{
	struct modstatus st;
	int id = 1, n = 0, refs = -1;

	while (modstat(id, &st, 1) == 0) {
		if (strcmp(st.ms_name, "auxcore") == 0 || strcmp(st.ms_name, "auxexec") == 0 ||
		    strcmp(st.ms_name, "guestcore") == 0)
			n++;
		if (strcmp(st.ms_name, "auxcore") == 0)
			refs = st.ms_refcnt;
		id = st.ms_id + 1;
	}
	t_check(what, n == 3 && refs == 0, "%d of 3 guest modules loaded, auxcore refs %d", n, refs);
}

int
main()
{
	struct mod_mreg reg;
	struct mod_execreg er;
	struct stat sb;
	char *argv[4];
	unsigned long v, info;
	int st, e;

	t_init("aux", 240);
	if (t_kmem("guest_loading") == -1) {
		t_skip("all", "kernel has no guest support");
		return t_done();
	}
	if (stat(SH, &sb) < 0 || stat(MD "/auxexec", &sb) < 0) {
		t_skip("all", "no A/UX binaries or guest modules on this root");
		return t_done();
	}
	e = modpath(MD) < 0 ? errno : 0;
	strcpy(reg.md_modname, "auxexec");
	reg.md_typedata = (caddr_t)&er;
	er.er_magic = 0x150;
	er.er_flags = EXF_FIRST;
	if (!t_check("register", e == 0 && modadm(MOD_TY_EXEC, MOD_C_MREG, &reg) == 0,
	    "modpath %d, modadm: %s", e, T_ERR))
		return t_done();

	argv[0] = SH;
	argv[1] = "-c";
	argv[2] = "echo hello";
	argv[3] = 0;
	st = run(argv, 30);
	t_check("sh_c", st == 0 && strcmp(out, "hello\n") == 0,
	    "status %#x, output '%s'", st, shown());
	modules("autoload", "auxexec auto-loaded with its dependencies");

	st = script("basic", s_basic, 90);
	t_check("sh_script", st == 0 && strcmp(out, x_basic) == 0,
	    "status %#x, output '%s'", st, shown());
	st = script("sig", s_sig, 60);
	t_check("sh_signals", st == 0 && strcmp(out, x_sig) == 0,
	    "status %#x, output '%s'", st, shown());
	st = script("ls", s_ls, 60);
	t_check("ls_l", st == 0 && strncmp(out, "-rw-r--r--", 10) == 0 &&
	    strstr(out, " 11 ") && strstr(out, "/tmp/aux_g\n") &&
	    strstr(out, "aux_nonexistent") && strstr(out, "status ") &&
	    !strstr(out, "status 0\n"),
	    "status %#x, output '%s'", st, shown());

	argv[0] = "/aux/bin/ls";
	argv[1] = "-d";
	argv[2] = "/tests/aux";
	argv[3] = 0;
	st = run(argv, 30);
	t_check("ls_exec", st == 0 && strcmp(out, "/tests/aux\n") == 0,
	    "status %#x, output '%s'", st, shown());
	argv[0] = "/aux/bin/rm";
	argv[1] = "/tmp/aux_nonexistent";
	argv[2] = 0;
	st = run(argv, 30);
	t_check("errno", WIFEXITED(st) && WEXITSTATUS(st) != 0 &&
	    strstr(out, "aux_nonexistent"), "status %#x, output '%s'", st, shown());
	if (getksym("aux_select", &v, &info) < 0)
		t_skip("m2", "auxcore without the shared-library loader and BSD calls");
	else {
		test_abi();
		test_libs();
		test_sleep();
		test_more();
		test_vi();
	}
	modules("released", "all A/UX processes gone: auxcore not held");
	return t_done();
}
