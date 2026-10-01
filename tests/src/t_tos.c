/*
 * t_tos.c -- Atari TOS in a container: starttos with the user's ROM.
 *
 * Registers tosguest for character major 56 (modules in
 * /tests/aux/mod.d), runs /tos/bin/starttos and checks: its display
 * session comes to front; TOS's own 200 Hz and VBL counters (_hz_200,
 * _frclock in guest memory, through /proc) advance at their rates;
 * the GEM desktop is on screen (host screen dump: a white menu bar
 * over a desktop of one colour); a Shift key reaches TOS's kbshift;
 * the mouse moves the pointer and opens a menu; SIGTERM ends TOS and
 * the console comes back.  The kernel's counters are logged.
 * Skips without guest support, the module or the local ROM.
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
#include <termio.h>
#include <sys/signal.h>
#include <sys/fault.h>
#include <sys/syscall.h>
#include <sys/procfs.h>
#include <dirent.h>
#include "sys/mod.h"
#include "dsio.h"
#include "tosio.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	EMUTOS	"/etc/tos/emutos.img"
#define	USERROM	"/etc/tos/rom"
#define	HZ200	0x4ba
#define	FRCLOCK	0x466

static char *pf;			/* profile: result and screen dump prefix */
static int hfd = -1, hseq;
static char hrep[128];
static pid_t tpid;
static int tfd = -1;
static long sess;
static char out[4096];
static int outn, ofd = -1;

/* a host request (display/hostio.py); its answer, 0 if none in 20 s */
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
	while (t_now_ms() - t0 < 20000) {
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

/* a name of this profile's (two at a time) */
static char *
N(n)
	char *n;
{
	static char b[2][48];
	static int k;

	k ^= 1;
	sprintf(b[k], "%s_%s", pf, n);
	return b[k];
}

#define	S	N

static char *
shot(name)
	char *name;
{
	char req[64];

	sprintf(req, "shot %s", name);
	return host(req);
}

static char *
cmp2(a, b)
	char *a, *b;
{
	char req[100];

	sprintf(req, "cmp %s %s", a, b);
	return host(req);
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

static void
nap(ms)
	int ms;
{
	poll((struct pollfd *)0, 0L, ms);
}

/* starttos's output to the log, a line at a time */
static void
relay()
{
	int n;
	char *nl;

	if (ofd < 0)
		return;
	while ((n = read(ofd, out + outn, sizeof out - 1 - outn)) > 0) {
		outn += n;
		out[outn] = 0;
		while ((nl = strchr(out, '\n')) != 0) {
			*nl = 0;
			printf("tos| %s\n", out);
			outn -= nl + 1 - out;
			memmove(out, nl + 1, outn + 1);
		}
		if (outn > 1000)
			outn = 0;
	}
	fflush(stdout);
}

/* guest memory through /proc; -1 if unreadable */
static long
peek(a, n)
	long a;
	int n;
{
	char path[32];
	unsigned char b[4];
	int fd;
	long v = -1;

	sprintf(path, "/proc/%05ld", (long)tpid);
	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	if (lseek(fd, a, 0) == a && read(fd, (char *)b, n) == n)
		v = n == 1 ? b[0] : n == 2 ? b[0] << 8 | b[1] :
		    (long)b[0] << 24 | (long)b[1] << 16 | b[2] << 8 | b[3];
	close(fd);
	return v;
}

static struct tosstat st;

static void
stat1(tag)
	char *tag;
{
	int i;
	char b[200];

	if (ioctl(tfd, TOSIOC_STAT, &st) < 0) {
		t_info(tag, "no stat: %s", strerror(errno));
		return;
	}
	b[0] = 0;
	for (i = 0; i < 48; i++)
		if (st.ts_refl[i])
			sprintf(b + strlen(b), " %d:%lu", i, st.ts_refl[i]);
	t_info(tag, "pc %lx timerc %lu acia %lu vbl %lu io %lu absent %lu berr %lu priv %lu mmu %lu"
	    " kb %lu/%lu cmd %lu sys %lu last %lx@%lx hz200 %ld refl%s",
	    st.ts_lastpc, st.ts_ints[5], st.ts_ints[6], st.ts_vbl, st.ts_io, st.ts_absent,
	    st.ts_berr, st.ts_priv, st.ts_mmu, st.ts_kbrd, st.ts_kbin, st.ts_kbcmd, st.ts_sys,
	    st.ts_lastio, st.ts_lastiopc, peek((long)HZ200, 4), b);
}

/* what starttos processes are left, and where they wait */
static void
procs(tag)
	char *tag;
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	prpsinfo_t ps;
	char path[32];
	int fd;

	while (d && (e = readdir(d)) != 0) {
		if (e->d_name[0] == '.')
			continue;
		sprintf(path, "/proc/%s", e->d_name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		if (ioctl(fd, PIOCPSINFO, &ps) == 0 && strcmp(ps.pr_fname, "starttos") == 0)
			t_info(tag, "pid %ld ppid %ld state %c wchan %lx", (long)ps.pr_pid,
			    (long)ps.pr_ppid, ps.pr_sname, (long)ps.pr_wchan);
		close(fd);
	}
	if (d)
		closedir(d);
}

static void
start(rom)
	char *rom;
{
	int p[2];

	pipe(p);
	if ((tpid = fork()) == 0) {
		setpgrp();
		close(p[0]);
		dup2(p[1], 1);
		dup2(p[1], 2);
		if (rom)
			execl("/tos/bin/starttos", "starttos", "-v", "-rom", rom, (char *)0);
		else
			execl("/tos/bin/starttos", "starttos", "-v", (char *)0);
		_exit(127);
	}
	close(p[1]);
	ofd = p[0];
	fcntl(ofd, F_SETFL, O_NDELAY);
}

/* the screen: stats of a dump, 1 when it looks like the desktop */
static int
desktop(name, info)
	char *name;
	int info;
{
	char req[64], *r;
	int x, y, w, h, menu = 0, desk = 0;

	sprintf(req, "shot %s", name);
	if (host(req) == 0)
		return 0;
	sprintf(req, "tos %s", name);
	if ((r = host(req)) == 0)
		return 0;
	sscanf(r, "box %d %d %d %d menu %d desk %d", &x, &y, &w, &h, &menu, &desk);
	if (info)
		t_info(N("screen"), "%s", r);
	return w >= 600 && h >= 380 && menu >= 500 && desk >= 500;
}

/* one container: its owner is shown, a second ENTER is refused */
static void
owner()
{
	struct tosowner to;
	struct tosenter te;
	int e;

	to.to_pid = 0;
	ioctl(tfd, TOSIOC_OWNER, &to);
	te.te_ramsize = 4L << 20;
	te.te_flags = 0;
	e = ioctl(tfd, TOSIOC_ENTER, &te) < 0 ? errno : 0;
	t_check(N("owner"), to.to_pid == tpid && e == EBUSY, "owner pid %ld (starttos %ld), enter %d",
	    to.to_pid, (long)tpid, e);
}

static void
timers()
{
	long h0 = peek((long)HZ200, 4), f0 = peek((long)FRCLOCK, 4), t0 = t_now_ms(), h1, f1, dt;

	nap(3000);
	h1 = peek((long)HZ200, 4);
	f1 = peek((long)FRCLOCK, 4);
	dt = t_now_ms() - t0;
	t_check(N("hz200_rate"), h0 >= 0 && (h1 - h0) * 1000 / dt >= 150 && (h1 - h0) * 1000 / dt <= 250,
	    "_hz_200 %ld -> %ld in %ld ms", h0, h1, dt);
	t_check(N("vbl_rate"), f0 >= 0 && (f1 - f0) * 1000 / dt >= 40 && (f1 - f0) * 1000 / dt <= 80,
	    "_frclock %ld -> %ld in %ld ms", f0, f1, dt);
}

/* kernel entries the guest caused so far */
static unsigned long
entries()
{
	unsigned long n;
	int i;

	ioctl(tfd, TOSIOC_STAT, &st);
	n = st.ts_io + st.ts_priv + st.ts_mmu + st.ts_sys + st.ts_vbl;
	for (i = 0; i < 64; i++)
		n += st.ts_refl[i];
	for (i = 0; i < 16; i++)
		n += st.ts_ints[i];
	return n;
}

/* loop iterations a native process at nice 19 gets in 3 s: what the guest leaves */
static long
spin()
{
	volatile long n = 0;
	long t0;
	int p[2], status;
	pid_t c;

	if (pipe(p) < 0 || (c = fork()) < 0)
		return 0;
	if (c == 0) {
		nice(19);
		t0 = t_now_ms();
		while (t_now_ms() - t0 < 3000)
			n++;
		write(p[1], (char *)&n, sizeof n);
		_exit(0);
	}
	close(p[1]);
	if (read(p[0], (char *)&n, sizeof n) != sizeof n)
		n = 0;
	close(p[0]);
	waitpid(c, &status, 0);
	return n;
}

static long spin0;		/* with no guest running */

/* CPU ticks so far of starttos (guest) and of its child (display) */
static void
cputimes(g, d)
	long *g, *d;
{
	DIR *dp = opendir("/proc");
	struct dirent *e;
	prpsinfo_t ps;
	char path[32];
	int fd;

	*g = *d = 0;
	while (dp && (e = readdir(dp)) != 0) {
		if (e->d_name[0] == '.')
			continue;
		sprintf(path, "/proc/%s", e->d_name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		if (ioctl(fd, PIOCPSINFO, &ps) == 0) {
			if (ps.pr_pid == tpid)
				*g = ps.pr_time.tv_sec * 1000L + ps.pr_time.tv_nsec / 1000000;
			else if (ps.pr_ppid == tpid)
				*d = ps.pr_time.tv_sec * 1000L + ps.pr_time.tv_nsec / 1000000;
		}
		close(fd);
	}
	if (dp)
		closedir(dp);
}

/* the idle desktop: kernel entries a second, and the CPU left to others */
static void
idle()
{
	unsigned long e0, e1, s0;
	long t0, dt, n, i0, i1, hz = sysconf(_SC_CLK_TCK), g0, g1, d0, d1;

	nap(2000);
	e0 = entries();
	s0 = st.ts_stop;
	i0 = st.ts_slept;
	cputimes(&g0, &d0);
	t0 = t_now_ms();
	nap(3000);
	e1 = entries();
	i1 = st.ts_slept;
	cputimes(&g1, &d1);
	dt = t_now_ms() - t0;
	n = spin();
	t_info(N("idle"), "%ld kernel entries/s, asleep in stop %ld%%, CPU charged: guest %ld%%, display %ld%%, "
	    "%lu stop/s, pc %lx; a nice 19 loop beside it runs at %ld%%",
	    (long)((e1 - e0) * 1000 / dt), (i1 - i0) * 1000 * 100 / (dt * hz),
	    (g1 - g0) * 100 / dt, (d1 - d0) * 100 / dt, (st.ts_stop - s0) * 1000 / dt,
	    st.ts_lastpc, spin0 ? n * 100 / spin0 : 0L);
	if (st.ts_stop > s0)
		t_check(N("idle_sleeps"), (e1 - e0) * 1000 / dt < 5000 &&
		    (i1 - i0) * 1000 * 100 / (dt * hz) >= 50,
		    "%ld kernel entries/s, asleep %ld%%", (long)((e1 - e0) * 1000 / dt),
		    (i1 - i0) * 1000 * 100 / (dt * hz));
}

/* kbshift: bit 1 is the left Shift key */
static void
keys()
{
	long kbs = peek(0xe00024L, 4), a, b, c;
	unsigned long n0 = st.ts_kbrd;

	if (kbs <= 0 || kbs >= 0x100000) {
		t_skip(N("kbshift"), "no kbshift address in the ROM header");
		return;
	}
	a = peek(kbs, 1);
	host("down shift");
	nap(500);
	b = peek(kbs, 1);
	host("up shift");
	nap(500);
	c = peek(kbs, 1);
	t_check(N("kbshift"), a >= 0 && !(a & 2) && (b & 2) && !(c & 2),
	    "kbshift %lx, with Shift down %lx, up %lx", a, b, c);
	ioctl(tfd, TOSIOC_STAT, &st);
	t_check(N("ikbd_read"), st.ts_kbrd >= n0 + 2, "IKBD bytes read %lu -> %lu", n0, st.ts_kbrd);
}

/* the pointer moves; on the Desk menu title the menu drops down */
static void
pointer(cx, cy)
	int cx, cy;		/* drive C:'s icon, from the top left */
{
	char req[40];
	char *r;
	int n = 0;
	unsigned long n0;

	shot(S("m0"));
	host("move 40 30");
	nap(800);
	shot(S("m1"));
	r = cmp2(S("m0"), S("m1"));
	if (r)
		sscanf(r, "diff %d", &n);
	t_check(N("mouse_moves"), n > 0, "%s", r ? r : "no answer");
	/* to the top left along the left edge, then onto "Desk" */
	host("move -400 0");
	nap(500);
	host("move 0 -300");
	nap(500);
	shot(S("m2"));
	host("move 30 8");
	nap(1500);
	shot(S("menu"));
	r = cmp2(S("m2"), S("menu"));
	n = 0;
	if (r)
		sscanf(r, "diff %d", &n);
	t_check(N("menu_opens"), n > 2000, "%s", r ? r : "no answer");
	host("move 0 150");			/* off the menu bar, then close the menu */
	nap(500);
	host("click 1");
	nap(1000);
	/* drive C:: a double click on its icon opens a window */
	host("move -400 0");
	nap(300);
	host("move 0 -300");
	nap(300);
	sprintf(req, "move %d %d", cx, cy);
	host(req);
	nap(500);
	shot(S("c0"));
	ioctl(tfd, TOSIOC_STAT, &st);
	n0 = st.ts_sys;
	host("clicks 2 40 80");
	nap(3000);
	shot(S("c1"));
	r = cmp2(S("c0"), S("c1"));
	n = 0;
	if (r)
		sscanf(r, "diff %d", &n);
	ioctl(tfd, TOSIOC_STAT, &st);
	t_check(N("drive_c_window"), n > 5000 && st.ts_sys > 0,
	    "%s, host calls %lu -> %lu", r ? r : "no answer", n0, st.ts_sys);
}

/* one profile: start, desktop, timers, input, idle cost, end */
static void
run(rom)
	char *rom;
{
	int i, up = 0, status, stuck = 0;
	long t0, h, lasth = -1;
	unsigned long lastpc = 0;

	outn = 0;
	start(rom);
	t0 = t_now_ms();
	while (t_now_ms() - t0 < 10000 && (sess = front()) <= 0) {
		relay();
		nap(200);
	}
	t_check(N("session_front"), sess > 0, "front %ld", sess);
	/* up to 150 s for the desktop; the kernel's counters every 5 s */
	for (i = 0; i < 30 && !up && stuck < 4; i++) {
		nap(5000);
		relay();
		if (waitpid(tpid, &status, WNOHANG) == tpid) {
			t_check(N("alive"), 0, "starttos ended, status %x", status);
			tpid = 0;
			break;
		}
		stat1(N("progress"));
		h = peek((long)HZ200, 4);
		stuck = st.ts_lastpc == lastpc && h == lasth ? stuck + 1 : 0;
		lastpc = st.ts_lastpc;
		lasth = h;
		if (hfd >= 0 && (up = desktop(S("desk"), 1)) != 0)
			t_info(N("desktop_after"), "%ld s", (t_now_ms() - t0) / 1000);
	}
	if (tpid) {
		t_check(N("desktop"), up, "no desktop on screen after %ld s", (t_now_ms() - t0) / 1000);
		if (hfd >= 0) {
			host("key ctrl+alt+meta_l+0");
			nap(1000);
			t_check(N("hotkey_console"), front() == 0, "front %ld", front());
			host("key ctrl+alt+meta_l+1");
			nap(1000);
			t_check(N("hotkey_tos"), front() == sess, "front %ld, TOS %ld", front(), sess);
		}
		owner();
		timers();
		idle();
		ioctl(tfd, TOSIOC_STAT, &st);
		t_check(N("cache_ops"), st.ts_cache > 0, "%lu cache instructions and CACR writes",
		    st.ts_cache);
		if (hfd >= 0) {
			keys();
			if (rom)
				pointer(35, 77);
			else
				pointer(35, 38);
			(void)desktop(S("end"), 1);
		} else
			t_skip(N("input"), "no host line");
		stat1(N("final"));
		relay();
		kill(tpid, SIGTERM);
		t0 = t_now_ms();
		while (waitpid(tpid, &status, WNOHANG) != tpid && t_now_ms() - t0 < 5000)
			nap(100);
		t_check(N("ended"), t_now_ms() - t0 < 5000, "starttos still running");
		t0 = t_now_ms();
		while (front() != 0 && t_now_ms() - t0 < 5000)
			nap(200);
		if (!t_check(N("console_back"), front() == 0, "front %ld", front()))
			procs(N("left"));
	}
	if (ofd >= 0) {
		relay();
		close(ofd);
		ofd = -1;
	}
}

int
main()
{
	struct mod_mreg reg;
	struct stat sb;
	int mj = TOS_MAJOR;

	t_init("tos", 700);
	if (t_kmem("guest_loading") == -1) {
		t_skip("all", "kernel has no guest support");
		return t_done();
	}
	if (stat(MD "/tosguest", &sb) < 0 || stat("/dev/tos", &sb) < 0) {
		t_skip("all", "no tosguest module on this root");
		return t_done();
	}
	if (stat("/tos/bin/starttos", &sb) < 0 ||
	    (stat(EMUTOS, &sb) < 0 && stat(USERROM, &sb) < 0)) {
		t_skip("all", "no starttos, EmuTOS or TOS ROM on this root");
		return t_done();
	}
	modpath(MD);
	strcpy(reg.md_modname, "tosguest");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "modadm: %s", T_ERR))
		return t_done();
	if (!t_check("open", (tfd = open("/dev/tos", O_RDWR)) >= 0, "/dev/tos: %s", T_ERR))
		return t_done();
	hostopen();
	spin0 = spin();
	pf = "emutos";
	if (stat(EMUTOS, &sb) == 0)
		run((char *)0);
	else
		t_skip("emutos", "no EmuTOS image on this root");
	pf = "tos306";
	if (stat(USERROM, &sb) == 0)
		run(USERROM);
	else
		t_skip("tos306", "no user TOS ROM on this root");
	return t_done();
}
