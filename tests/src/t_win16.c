/*
 * t_win16.c -- the Win16 environment: /usr/bin/startwin on its volume
 * (SCSI disk 3, /win).  The test programs run with C: their own
 * directory; exit codes and RESULT.TXT are checked as on the build host.
 * With the user's Windows 3.11 disks and Wabi 2.2 on the volume,
 * `startwin -install' sets up HOME=/win/home, Program Manager starts,
 * redraws after the session comes back to the front, runs programs from
 * File Run and exits, then starts again; startwin's CPU time for each
 * step is logged.
 * Screen dumps go to the host (display/win16_*.png).
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
#include <termio.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/procfs.h>
#include "dsio.h"
#include "t.h"

#define	START	"/usr/bin/startwin"
#define	DEV	"/dev/dsk/c3d0s0"
#define	W	"/win"
#define	LOG	W "/t/startwin.log"

static int hfd = -1, hseq;
static char hrep[128];

/* a host request; its answer, 0 if none in 60 s */
static char *
host(req)
	char *req;
{
	char buf[160], line[256], *nl, *s;
	struct pollfd p;
	int n, len = 0, seq;
	long t0 = t_now_ms();

	if (hfd < 0)
		return 0;
	sprintf(buf, "@@ %d %s\n", ++hseq, req);
	write(hfd, buf, strlen(buf));
	p.fd = hfd;
	p.events = POLLIN;
	while (t_now_ms() - t0 < 60000) {
		if (poll(&p, 1L, 500) <= 0 || (n = read(hfd, line + len, sizeof line - 1 - len)) <= 0)
			continue;
		len += n;
		line[len] = 0;
		while ((nl = strchr(line, '\n')) != 0) {
			*nl = 0;
			if ((s = strstr(line, "@@ok ")) != 0 && sscanf(s + 5, "%d", &seq) == 1 &&
			    seq == hseq) {
				s = strchr(s + 5, ' ');
				strncpy(hrep, s ? s + 1 : "", sizeof hrep - 1);
				hrep[strcspn(hrep, "\r")] = 0;
				return hrep;
			}
			if ((s = strstr(line, "@@err ")) != 0 && sscanf(s + 6, "%d", &seq) == 1 &&
			    seq == hseq)
				return strcpy(hrep, "error");
			len -= nl + 1 - line;
			memmove(line, nl + 1, len + 1);
		}
		if (len > 200)
			len = 0;
	}
	return 0;
}

static void
hostopen()
{
	struct termio t;

	if ((hfd = open("/dev/term/b", O_RDWR | O_NOCTTY)) < 0)
		return;
	if (ioctl(hfd, TCGETA, &t) == 0) {
		t.c_iflag = IGNCR;
		t.c_oflag = 0;
		t.c_lflag = ICANON;
		t.c_cflag |= CREAD | CLOCAL;
		ioctl(hfd, TCSETAF, &t);
	}
	if (host("ping") == 0 || strcmp(hrep, "pong") != 0) {
		close(hfd);
		hfd = -1;
	}
}

static void
nap(ms)
	int ms;
{
	poll((struct pollfd *)0, 0L, ms);
}

static void
shot(name)
	char *name;
{
	char req[64];

	sprintf(req, "shot win16_%s", name);
	host(req);
}

/* keys typed: letters, digits, '.', '\\', ':' */
static void
type(s)
	char *s;
{
	char req[40];

	for (; *s; s++) {
		if (*s == '.')
			strcpy(req, "key dot");
		else if (*s == '\\')
			strcpy(req, "key backslash");
		else if (*s == ':')
			strcpy(req, "key shift+semicolon");
		else
			sprintf(req, "key %c", *s);
		host(req);
	}
}

static long
front()
{
	struct fbstate st;
	int fd = open("/dev/fb0", O_RDWR);

	st.st_front = -1;
	if (fd >= 0) {
		if (ioctl(fd, FBIOGSTATE, &st) < 0)
			st.st_front = -1;
		close(fd);
	}
	return st.st_front;
}

/*
 * The host's PC samples for SECS s: ms of work (not the idle loop) and
 * ms to the end of the last busy 100 ms; 0 if none.
 */
static int
busy(secs, b, e)
	int secs;
	long *b, *e;
{
	static unsigned long lo, hi;
	char req[64], *r;

	if (!lo) {
		FILE *f = fopen("/tests/ksyms", "r");
		char sym[64];
		unsigned long a;

		/* the idle STOP: just before waitloc, or the start of idle */
		while (f && fscanf(f, "%63s %lx", sym, &a) == 2)
			if (strcmp(sym, "waitloc") == 0)
				lo = a - 4, hi = a + 2;
			else if (strcmp(sym, "idle") == 0 && !hi)
				lo = a, hi = a + 6;
		if (f)
			fclose(f);
		if (!lo)
			lo = 1;
	}
	if (!hi)
		return 0;
	sprintf(req, "busy %d %lx %lx", secs, lo, hi);
	return (r = host(req)) != 0 && sscanf(r, "%ld %ld", b, e) == 2;
}

/* ms until the machine is idle for a 5 s window, up to LIMIT s; -1 if never */
static long
settle(limit)
	int limit;
{
	long b, e, t = 0, last = 0;

	while (t < limit * 1000L) {
		if (!busy(5, &b, &e))
			return -1;
		if (b < 250)
			return last;
		last = t + e;
		t += 5000;
		t_rearm(600);
	}
	return -1;
}

/* startwin's messages to the console */
static void
relay()
{
	char line[300];
	FILE *f = fopen(LOG, "r");
	int n = 0;

	while (f && fgets(line, sizeof line, f) && n++ < 60)
		printf("win| %s", line);
	if (f)
		fclose(f);
	fflush(stdout);
}

static pid_t
start(argv)
	char **argv;
{
	pid_t p;
	int fd;

	if ((p = fork()) == 0) {
		putenv("HOME=" W "/home");
		putenv("W16_X86PROF=1");
		if ((fd = open(LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
		}
		execv(START, argv);
		_exit(126);
	}
	return p;
}

/* a file's text without CRs */
static int
slurp(path, buf, n)
	char *path, *buf;
	int n;
{
	int fd = open(path, O_RDONLY), k, i, j;

	if (fd < 0)
		return -1;
	k = read(fd, buf, n - 1);
	close(fd);
	if (k < 0)
		return -1;
	for (i = j = 0; i < k; i++)
		if (buf[i] != '\r')
			buf[j++] = buf[i];
	buf[j] = 0;
	return j;
}

/* a test program in a C: of its own: its exit code and RESULT.TXT */
static void
prog(name, want)
	char *name;
	int want;
{
	char dir[64], exe[80], cmd[200], res[80], exp[80], a[4096], b[4096];
	char *argv[5];
	char n[16];
	pid_t p;
	int st = -1, ok, i;
	long t0;

	sprintf(dir, W "/t/%s", name);
	sprintf(exe, "%s/%s.exe", dir, name);
	sprintf(cmd, "rm -rf %s; mkdir -p %s; cp " W "/progs/%s.exe %s", dir, dir, name, dir);
	if (system(cmd) != 0 || access(exe, 0) < 0) {
		t_skip(name, "no %s.exe", name);
		return;
	}
	argv[0] = "startwin";
	argv[1] = "-C";
	argv[2] = dir;
	argv[3] = exe;
	argv[4] = 0;
	t0 = t_now_ms();
	p = start(argv);
	/* its window is up for half a second: screens until it ends */
	for (i = 0; p > 0 && strcmp(name, "hello") == 0 && i < 20; i++) {
		nap(200);
		if (waitpid(p, &st, WNOHANG) == p) {
			p = 0;
			break;
		}
		sprintf(n, "hello%d", i);
		shot(n);
	}
	if (p > 0)
		t_waitchild(p, &st, 300);
	t_info(name, "%ld ms", t_now_ms() - t0);
	ok = WIFEXITED(st) && WEXITSTATUS(st) == want;
	sprintf(res, "%s/result.txt", dir);
	sprintf(exp, W "/expect/%s.txt", name);
	if (ok && access(exp, 0) == 0)
		ok = slurp(res, a, sizeof a) >= 0 && slurp(exp, b, sizeof b) >= 0 && strcmp(a, b) == 0;
	if (!t_check(name, ok, "status %#x (want exit %d)", st, want)) {
		relay();
		if (slurp(res, a, sizeof a) > 0)
			printf("win| result: %.1000s\n", a);
	}
	t_rearm(600);
}

static void
install()
{
	char *argv[16];
	char path[12][40];
	pid_t p;
	int st = -1, i, n = 0;
	long t0;

	argv[n++] = "startwin";
	argv[n++] = "-install";
	for (i = 1; i <= 12; i++) {
		sprintf(path[i - 1], W "/disks/DISK%d.IMG", i);
		if (access(path[i - 1], 0) == 0)
			argv[n++] = path[i - 1];
	}
	argv[n++] = W "/wabi";
	argv[n] = 0;
	t0 = t_now_ms();
	if ((p = start(argv)) > 0)
		t_waitchild(p, &st, 1200);
	t_info("install_time", "%ld ms", t_now_ms() - t0);
	relay();
	t_check("install", WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
	    access(W "/home/WIN16/windows/progman.exe", 0) == 0, "status %#x", st);
	t_rearm(600);
}

/* user and system CPU time of a process, ms; -1 unknown */
static long
cpums(p)
	pid_t p;
{
	char path[32];
	prpsinfo_t ps;
	int fd, r;

	sprintf(path, "/proc/%05ld", (long)p);
	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	r = ioctl(fd, PIOCPSINFO, &ps);
	close(fd);
	return r < 0 ? -1 : ps.pr_time.tv_sec * 1000L + ps.pr_time.tv_nsec / 1000000;
}

/* system CPU time of a process, ms; -1 unknown */
static long
sysms(p)
	pid_t p;
{
	char path[32];
	prstatus_t st;
	int fd, r;

	sprintf(path, "/proc/%05ld", (long)p);
	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	r = ioctl(fd, PIOCSTATUS, &st);
	close(fd);
	return r < 0 ? -1 : st.pr_stime.tv_sec * 1000L + st.pr_stime.tv_nsec / 1000000;
}

/* Exit Windows: File Exit, then OK */
static void
exitwin(p, name)
	pid_t p;
	char *name;
{
	int st = -1;

	host("key alt+f");
	nap(500);
	host("key x");
	settle(60);
	shot("exit");
	host("key ret");
	t_waitchild(p, &st, 120);
	relay();
	t_check(name, WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %#x", st);
}

/* Program Manager again, its groups made: to idle */
static void
again()
{
	char *argv[2];
	pid_t p;

	argv[0] = "startwin";
	argv[1] = 0;
	if ((p = start(argv)) < 0)
		return;
	host("prof 40 progman_restart");
	nap(1000);
	settle(400);
	t_info("progman_restart", "%ld ms CPU, %ld system", cpums(p), sysms(p));
	shot("progman_restart");
	exitwin(p, "progman_restart_exit");
}

/*
 * D pixels along one axis.  Moves over 6 steps count double: those go
 * in single reports of 7 to 60 steps, an odd pixel as one step after.
 */
static void
axis(d, isx)
	int d, isx;
{
	char req[40];
	int m, k, n;

	m = d >= 14 ? d / 2 : 0;
	for (k = (m + 59) / 60; m > 0; k--) {
		n = m / k;
		sprintf(req, "move %d %d", isx ? n : 0, isx ? 0 : n);
		host(req);
		m -= n;
	}
	for (d = d >= 14 ? d & 1 : d; d > 0; d -= n) {
		n = d > 6 ? 6 : d;
		sprintf(req, "move %d %d", isx ? n : 0, isx ? 0 : n);
		host(req);
	}
}

/* the pointer to (x, y) from the top left corner */
static void
pointer(x, y)
	int x, y;
{
	host("move -2000 -2000 50");
	axis(x, 1);
	axis(y, 0);
}

/* a screen dump with the pointer parked in the bottom right corner */
static void
pshot(name)
	char *name;
{
	host("move 2000 2000 50");
	nap(300);
	shot(name);
}

/* the screen differs from shot NAME (as it was before a step) */
static int
changed(name, now)
	char *name, *now;
{
	char req[64];

	pshot(now);
	sprintf(req, "cmp win16_%s win16_%s", name, now);
	return host(req) != 0 && strncmp(hrep, "diff", 4) == 0;
}

static int
same(name, now)
	char *name, *now;
{
	char req[64];

	pshot(now);
	sprintf(req, "cmp win16_%s win16_%s", name, now);
	return host(req) != 0 && strcmp(hrep, "same") == 0;
}

/*
 * Program Manager's groups at 640x480 (small fonts): the active group is
 * Main at its first start, its system box at (31, 70); a minimised
 * group's icon goes to the MDI client's first free place, (57, 325).
 */
static void
groups(p)
	pid_t p;
{
	pointer(100, 100);
	shot("pointer100");
	pshot("groups0");
	/* the system box: its menu, then a second click closes (minimises) the group */
	pointer(31, 70);
	host("click 2");
	settle(60);
	t_check("group_sysbox_close", kill(p, 0) == 0 && changed("groups0", "groups1"), "startwin ended or nothing changed");
	relay();
	/* the icon's double click restores it */
	pointer(57, 325);
	host("click 2");
	settle(60);
	t_check("group_icon_restore", kill(p, 0) == 0 && same("groups0", "groups2"), "startwin ended or not restored");
	/* Ctrl+F4 minimises, the Window menu's first entry restores */
	host("key ctrl+f4");
	settle(60);
	t_check("group_ctrlf4", kill(p, 0) == 0 && changed("groups0", "groups3"), "startwin ended or nothing changed");
	host("key alt+w");
	nap(500);
	host("key 1");
	settle(60);
	t_check("group_window_menu", kill(p, 0) == 0 && same("groups0", "groups4"), "startwin ended or not restored");
	/* the system menu by mouse: Minimize, then the icon's menu: Restore */
	pointer(31, 70);
	host("click 1");
	nap(800);
	host("key n");
	settle(60);
	pointer(57, 325);
	host("click 1");
	nap(800);
	host("key r");
	settle(60);
	t_check("group_sysmenu", kill(p, 0) == 0 && same("groups0", "groups5"), "startwin ended or not restored");
	/* Main's icon, partly under the next group, stays under it when a window over both closes */
	host("key ctrl+f4");
	settle(60);
	host("key ctrl+f6");
	settle(60);
	pshot("icons0");
	host("key alt+f");
	nap(500);
	host("key r");
	nap(1500);
	type("notepad");
	host("key ret");
	settle(300);
	host("key alt+f4");
	settle(60);
	t_check("group_icon_under", kill(p, 0) == 0 && same("icons0", "icons1"), "startwin ended or the screen differs");
	host("key alt+w");
	nap(500);
	host("key 1");
	settle(60);
}

/* a program from File Run, used and quit; Program Manager's screen as before */
static void
use(p, name, keys, save)
	pid_t p;
	char *name, *keys;
	int save;
{
	char a[24], b[24], q[24];
	long c0 = cpums(p);

	sprintf(a, "%s0", name);
	sprintf(b, "%s1", name);
	pshot(a);
	host("key alt+f");
	nap(500);
	host("key r");
	nap(1500);
	type(name);
	host("key ret");
	settle(300);
	if (keys)
		type(keys);
	/* minimised and back */
	host("key alt+spc");
	nap(500);
	host("key n");
	settle(60);
	host("key alt+spc");
	nap(500);
	host("key r");
	settle(60);
	shot(name);
	t_check(name, kill(p, 0) == 0, "startwin ended");
	host("key alt+f4");
	settle(60);
	if (save) {
		host("key n");		/* its changes not saved */
		settle(60);
	}
	t_info(name, "%ld ms CPU", cpums(p) - c0);
	sprintf(q, "%s_quit", name);
	t_check(q, kill(p, 0) == 0 && same(a, b), "startwin ended or a window left");
}

/* Program Manager: start, redraw, File Run, exit */
static void
progman()
{
	char *argv[2];
	pid_t p;
	int i;
	long t0, ms, f0, sess, c0, s0;
	char line[64];
	static char *apps[] = { "notepad", "calc", "sol", "winmine", "clock", 0 };

	f0 = front();
	argv[0] = "startwin";
	argv[1] = 0;
	t0 = t_now_ms();
	if ((p = start(argv)) < 0) {
		t_fail("progman", "fork: %s", T_ERR);
		return;
	}
	host("prof 60 progman");		/* PC samples, display/progman.prof */
	for (i = 0; i < 100 && front() == f0; i++)
		nap(100);
	t_info("progman_front", "%ld ms", t_now_ms() - t0);
	ms = settle(400);
	t_info("progman_start", "%ld ms CPU, %ld system, idle %ld ms after start", cpums(p), sysms(p), ms);
	shot("progman");
	t_check("progman_running", kill(p, 0) == 0, "exited");

	/* away to the console and back: everything repaints */
	sess = front();
	host("key ctrl+alt+meta_l+0");
	nap(1000);
	c0 = cpums(p);
	host("key ctrl+alt+meta_l+1");
	settle(60);
	t_info("progman_redraw", "%ld ms CPU", cpums(p) - c0);
	t_check("progman_front_again", front() == sess, "front %ld, ours %ld", front(), sess);
	shot("progman_redraw");

	for (i = 0; apps[i]; i++) {
		host("key alt+f");
		nap(500);
		host("key r");
		nap(1500);
		type(apps[i]);
		c0 = cpums(p);
		s0 = sysms(p);
		host("key ret");
		sprintf(line, "prof 20 %s", apps[i]);
		host(line);
		settle(300);
		t_info(apps[i], "%ld ms CPU from Run's OK to idle, %ld system", cpums(p) - c0, sysms(p) - s0);
		shot(apps[i]);
		host("key alt+f4");
		settle(60);
	}
	shot("progman_end");
	if (host("shot win16_size") != 0 && strcmp(hrep, "640 480") == 0) {
		groups(p);
		use(p, "notepad", "hello", 1);
		use(p, "write", "hello", 1);
		use(p, "pbrush", (char *)0, 0);
		use(p, "calc", (char *)0, 0);
		use(p, "winfile", (char *)0, 0);
		use(p, "control", (char *)0, 0);
	} else
		t_skip("groups", "screen not 640x480");
	exitwin(p, "progman_exit");
}

/* the 68k run loop against the C core, instruction by instruction */
static void
x86diff()
{
	char line[200];
	FILE *f;
	int st;

	if (access(W "/x86diff", 0) < 0) {
		t_skip("x86diff", "no x86diff");
		return;
	}
	t_rearm(1800);
	f = popen(access(W "/x86corpus", 0) == 0 ? W "/x86diff " W "/x86corpus 2>&1" : W "/x86diff 2>&1", "r");
	while (f && fgets(line, sizeof line, f))
		printf("x86| %s", line);
	st = f ? pclose(f) : -1;
	t_check("x86diff", st == 0, "status %#x", st);
	if ((f = popen(W "/x86diff -t 2", "r")) != 0) {	/* a loop on each core */
		while (fgets(line, sizeof line, f))
			t_info("x86speed", "%s", line);
		pclose(f);
	}
}

int
main()
{
	t_init("win16", 600);
	if (access(START, 0) < 0) {
		t_skip("all", "no %s", START);
		return t_done();
	}
	if (system("/sbin/mount -F ufs " DEV " " W) != 0) {
		t_skip("all", "no volume");
		return t_done();
	}
	hostopen();
	x86diff();
	prog("hello", 7);
	prog("fpu", 0);
	if (access("/usr/lib/tcpip.so", 0) == 0)
		prog("sock", 0);
	else
		t_skip("sock", "no TCP/IP");
	prog("meta", 0);
	prog("voice", 0);
	if (access(W "/disks/DISK1.IMG", 0) < 0)
		t_skip("install", "no Windows 3.11 disks");
	else {
		install();
		if (hfd < 0)
			t_skip("progman", "no host line");
		else if (access(W "/home/WIN16/windows/progman.exe", 0) == 0) {
			progman();
			again();
		}
	}
	system("/sbin/umount " W);
	return t_done();
}
