/*
 * t_mac.c -- the Mac environment of A/UX: /dev/uinter0 (the uinter
 * module) and startmac.
 *
 * Registers auxexec for magic 0x150 and uinter for character major 54
 * (modules in /tests/aux/mod.d), then:
 *   - native: the first open loads uinter, GETVERSION is 5, layer
 *     commands refuse a native process, the module is held while open;
 *   - macabi, an A/UX program that makes itself a Mac task: its lines
 *     become results (A-line frames, privileged instructions, the
 *     virtual IPL, the tick, the read-only ROM, low memory, PRAM);
 *   - startmac with TBVERBOSE, TBWARN and the System Folder
 *     /mac/sys/Sys7: its output is relayed to the console ("mac| "),
 *     the kernel records the task's uinter commands and opens in its
 *     trace ring, which goes to the log afterwards ("ktrace| "); checks:
 *     the Mac side passed doDispatch (its Memory Manager reports
 *     SysZone and ApplZone) and opened the System file after UI_SET;
 *     its display session comes to front and, on a direct boot with
 *     the host line, the screen dump shows the grey desktop
 *     (display/mac_desktop.png); Control-Option-Command-0 brings the
 *     console back, -1 the Mac, and its exit the console.
 * Skips without guest support, without the uinter module or without
 * the A/UX and ROM files, which are local only.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mkdev.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <termio.h>
#include "sys/mod.h"
#include "dsio.h"
#include "t.h"

extern int getksym();

#define	MD	"/tests/aux/mod.d"
#define	UIMAJ	54
#define	TLOW	0x744c4f57		/* 'tLOW', the Mac RAM segment */
#define	LOGMAX	65536
#define	TBUF	8192		/* AUX_TBUF */
#define	PRAMF	"/etc/aux/pram"

static char *macenv[] = {
	"PATH=/aux/bin:/usr/bin:/sbin", "HOME=/tmp", "TBVERBOSE=1", "TBWARN=1",
	"TBSYSTEM=/mac/sys/Sys7", "TBMEMORY=8M", 0
};
static char out[8192];
static char klog[LOGMAX + 1];
static int zones;		/* 1: SysZone, 2: ApplZone reported */
static int novideo;		/* "No Suitable Video" seen */
static long macid;		/* the Mac's session, once in front */
static long mact;		/* when it came to front, ms */
static int hfd = -1, hseq;	/* host line */
static char hrep[128];
static int scrw = 800, scrh = 600;	/* the Mac's screen */

#define	SETTLE	25		/* s from the Mac in front to the screen dump */

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

/* the session in front, 0 the console, -1 no display */
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

static int
waitfront(id, secs)
long id;
int secs;
{
	long t0 = t_now_ms();

	while (t_now_ms() - t0 < secs * 1000L) {
		if (front() == id)
			return 1;
		poll((struct pollfd *)0, 0L, 200);
	}
	return 0;
}

/* a kernel or module long through /dev/kmem */
static int
kmemrw(addr, val, wr)
unsigned long addr;
long *val;
int wr;
{
	int fd = open("/dev/kmem", wr ? O_RDWR : O_RDONLY), ok;

	if (fd < 0)
		return -1;
	ok = lseek(fd, (off_t)addr, 0) != -1 &&
	    (wr ? write(fd, (char *)val, 4) : read(fd, (char *)val, 4)) == 4;
	close(fd);
	return ok ? 0 : -1;
}

static int
setsym(name, v)
char *name;
long v;
{
	unsigned long a = 0, info;	/* 0: look up by name */

	if (getksym(name, &a, &info) < 0)
		return -1;
	return kmemrw(a, &v, 1);
}

static long
getsym(name)
char *name;
{
	unsigned long a = 0, info;
	long v;

	if (getksym(name, &a, &info) < 0 || kmemrw(a, &v, 0) < 0)
		return -1;
	return v;
}

/* the kernel trace ring (aux_tbuf), oldest first, into klog */
static long klen;

static int
readtrace()
{
	unsigned long a = 0, info;
	long pos, n, from;
	int fd;

	if (getksym("aux_tbuf", &a, &info) < 0 || (pos = getsym("aux_tpos")) < 0 ||
	    (fd = open("/dev/kmem", O_RDONLY)) < 0)
		return -1;
	from = pos > TBUF ? pos % TBUF : 0;
	n = pos > TBUF ? TBUF - from : pos;
	klen = 0;
	if (lseek(fd, (off_t)(a + from), 0) != -1 && read(fd, klog, n) == n)
		klen = n;
	if (pos > TBUF && lseek(fd, (off_t)a, 0) != -1 && read(fd, klog + klen, from) == from)
		klen += from;
	close(fd);
	klog[klen] = 0;
	return klen > 0 ? 0 : -1;
}

static void
native()
{
	struct modstatus st;
	int fd, id = 1, refs = -1, major = -1;

	fd = open("/dev/uinter0", O_RDWR);
	if (!t_check("native_open", fd >= 0, "%s", T_ERR))
		return;
	t_check("native_version", ioctl(fd, 0x20005100, 0) == 5, "%s", T_ERR);
	t_check("native_refused", ioctl(fd, 0x20005115, 0) == -1 && errno == EINVAL,
	    "CREATELAYER from a native process: %s", T_ERR);
	while (modstat(id, &st, 1) == 0) {
		if (strcmp(st.ms_name, "uinter") == 0) {
			refs = st.ms_refcnt;
			if (st.ms_msinfo[0].mss_type == MOD_TY_CDEV)
				major = st.ms_msinfo[0].mss_p1[0];
		}
		id = st.ms_id + 1;
	}
	t_check("module", refs == 1 && major == UIMAJ,
	    "uinter refs %d, major %d", refs, major);
	close(fd);
}

/* the macabi lines as results */
static void
macabi()
{
	static char *av[] = { "/aux/bin/macabi", 0 };
	char sub[100], *l, *e, *why, pram[256];
	int p[2], st, n, got = 0, done = 0, fd;
	struct pollfd pf;
	long t0 = t_now_ms();
	pid_t pid;

	t_rearm(90);
	/* a PRAM file for the session to load: marked, 24-bit addressing asked */
	memset(pram, 0, sizeof pram);
	memcpy(pram + 0xfc, "PRAM", 4);
	if ((fd = open(PRAMF, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
		write(fd, pram, sizeof pram);
		close(fd);
	}
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		execve(av[0], av, macenv);
		_exit(127);
	}
	close(p[1]);
	pf.fd = p[0];
	pf.events = POLLIN;
	/* a hung macabi still reports how far it got */
	while (pid > 0 && got < sizeof out - 1 && t_now_ms() - t0 < 60000L) {
		if (poll(&pf, 1L, 500) <= 0)
			continue;
		if ((n = read(p[0], out + got, sizeof out - 1 - got)) <= 0)
			break;
		got += n;
	}
	if (pid > 0 && t_now_ms() - t0 >= 60000L)
		kill(pid, SIGKILL);
	out[got] = 0;
	close(p[0]);
	if (pid < 0 || t_waitchild(pid, &st, 60) < 0)
		st = -1;
	for (l = out; *l; l = *e ? e + 1 : e) {
		for (e = l; *e && *e != '\n'; e++)
			;
		if (e - l == 4 && strncmp(l, "done", 4) == 0)
			done = 1;
		if (e - l < 3 || e - l >= sizeof sub - 8 || l[1] != ' ')
			continue;
		sprintf(sub, "macabi_%.*s", (int)(e - l - 2), l + 2);
		why = strchr(sub, ':');
		if (l[0] == 'I') {
			if ((why = strchr(sub, ' ')) != 0)
				*why++ = 0;
			t_info(sub, "%s", why ? why : "");
			continue;
		}
		if (why) {
			*why++ = 0;
			while (*why == ' ')
				why++;
		} else
			why = "";
		if (l[0] == 'P')
			t_pass(sub);
		else
			t_fail(sub, "%s", why);
	}
	t_check("macabi", st == 0 && done, "status %#x, done %d", st, done);
	/* written back as the session ended; startmac then starts from its own */
	n = -1;
	if ((fd = open(PRAMF, O_RDONLY)) >= 0) {
		n = read(fd, pram, sizeof pram);
		close(fd);
	}
	t_check("pram_file", n == 256 && memcmp(pram + 0xfc, "Mac!", 4) == 0,
	    "read %d", n);
	unlink(PRAMF);
}

/* startmac's output to the console, one "mac| " line each */
static void
relay(fd, secs)
int fd, secs;
{
	char b[512], l[600];
	struct pollfd pf;
	int n, k = 0, i;
	long t0 = t_now_ms();

	pf.fd = fd;
	pf.events = POLLIN;
	while (t_now_ms() - t0 < secs * 1000L) {
		pf.revents = 0;
		if (macid == 0 && (macid = front()) > 0) {
			mact = t_now_ms();
			/* what follows would overrun the trace ring */
			setsym("aux_trace", 0L);
			setsym("uinter_trace", 0L);
		}
		else if (macid < 0)
			macid = 0;
		if (macid && t_now_ms() - mact > SETTLE * 1000L)
			break;
		if (poll(&pf, 1L, 100) <= 0)
			continue;
		if ((n = read(fd, b, sizeof b)) <= 0)
			break;
		for (i = 0; i < n; i++) {
			if (b[i] == '\n' || k == sizeof l - 8) {
				l[k] = 0;
				/* the run stops on this word; keep it out */
				if (strstr(l, "SysZone"))
					zones |= 1;
				if (strstr(l, "ApplZone"))
					zones |= 2;
				if (strstr(l, "No Suitable Video"))
					novideo = 1;
				if (strstr(l, "anic"))
					strcpy(l, "(line withheld)");
				printf("mac| %s\n", l);
				fflush(stdout);
				k = 0;
			}
			if (b[i] != '\n' && b[i] >= ' ' && b[i] < 0x7f)
				l[k++] = b[i];
		}
	}
}

static pid_t macpid;

static void
pause_ms(ms)
long ms;
{
	poll((struct pollfd *)0, 0L, ms);
}

#define	L32(a)	((long)lm[a] << 24 | (long)lm[(a) + 1] << 16 | lm[(a) + 2] << 8 | lm[(a) + 3])

/* the running Mac: Ticks, Time, mouse, cursor and keys from the host */
static void
input()
{
	unsigned char *lm;
	long t0, d, m0, m1, c0, e0, k0, t1;
	int id, n = -1, chk = 0;
	char *r;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!t_check("mac_lowmem", lm && lm != (unsigned char *)-1, "shmat: %s", T_ERR))
		return;
	t0 = L32(0x16a);
	pause_ms(1000L);
	d = L32(0x16a) - t0;
	t_check("mac_ticks_60hz", d >= 50 && d <= 70, "%ld ticks in a second", d);
	d = L32(0x20c) - ((long)time((time_t *)0) + 2082844800L);
	t_check("mac_time", d >= -2 && d <= 2, "Time off by %ld s", d);
	host("shot mac_cur1");
	m0 = getsym("uin_mouse");
	c0 = getsym("uin_ncur");
	host("move 40 30");
	pause_ms(300L);
	m1 = getsym("uin_mouse");
	t_check("mouse_moves", (short)m1 > (short)m0 && (m1 >> 16) > (m0 >> 16),
	    "%#lx -> %#lx", m0, m1);
	t_check("mouse_lowmem", L32(0x830) == m1, "Mouse %#lx, kernel %#lx", L32(0x830), m1);
	t_check("cursor_drawn", getsym("uin_ncur") > c0, "draws %ld", getsym("uin_ncur") - c0);
	host("shot mac_cur2");
	r = host("cmp mac_cur1 mac_cur2");
	if (r && sscanf(r, "diff %d", &n) != 1)
		n = 0;
	/* the old cursor erased, the new one drawn */
	t_check("cursor_on_screen", n > 0 && n <= 1000, "%s", r ? r : "no answer");
	host("down a");
	pause_ms(300L);
	t_check("keymap_down", lm[0x174] & 1, "KeyMap %#x", lm[0x174]);
	host("up a");
	pause_ms(300L);
	t_check("keymap_up", !(lm[0x174] & 1), "KeyMap %#x", lm[0x174]);
	e0 = getsym("uin_nev");
	k0 = getsym("uin_nkey");
	host("key b");
	for (d = 0; d < 30 && getsym("uin_nkey") == k0; d++)
		pause_ms(100L);
	t_check("key_event_posted", getsym("uin_nev") > e0, "posted %ld", getsym("uin_nev") - e0);
	t_check("key_event_taken", getsym("uin_nkey") > k0, "taken %ld", getsym("uin_nkey") - k0);
	/* a click beside the Finder's alert: taken, and the Mac stays */
	k0 = getsym("uin_nbtn");
	host("button 1");
	host("button 0");
	for (d = 0; d < 30 && getsym("uin_nbtn") - k0 < 2; d++)
		pause_ms(100L);
	t_check("click_taken", getsym("uin_nbtn") - k0 >= 2, "button events taken %ld",
	    getsym("uin_nbtn") - k0);
	pause_ms(3000L);
	t0 = L32(0x16a);
	pause_ms(500L);
	t1 = L32(0x16a);
	t_check("mac_alive", kill(macpid, 0) == 0 && t1 - t0 >= 20, "ticks %ld", t1 - t0);
	shmdt((char *)lm);
	host("shot mac_after");
	r = host("mac mac_after");
	if (r)
		sscanf(r, "checker %d", &chk);
	t_check("mac_desktop_stays", chk >= 500, "checker %d per mille", chk);
}

/* the mouse to h, v on the Mac's screen, through the host; 1 when there */
static int
moveto(h, v)
int h, v;
{
	char r[40];
	long m;
	int i, dh, dv;

	for (i = 0; i < 30; i++) {
		m = getsym("uin_mouse");
		dh = h - (short)m;
		dv = v - (short)(m >> 16);
		if (dh >= -2 && dh <= 2 && dv >= -2 && dv <= 2)
			return 1;
		/* half way each time: accelerated steps still close in */
		dh = dh / 2 ? dh / 2 : dh;
		dv = dv / 2 ? dv / 2 : dv;
		dh = dh > 40 ? 40 : dh < -40 ? -40 : dh;
		dv = dv > 40 ? 40 : dv < -40 ? -40 : dv;
		sprintf(r, "move %d %d", dh, dv);
		host(r);
		pause_ms(100L);
	}
	return 0;
}

/* n clicks held hold ms, gap ms apart: mouseDowns the Mac took */
static long
clicks(n, hold, gap)
int n, hold, gap;
{
	char r[40];
	long d0 = getsym("uin_ndown");
	int i;

	sprintf(r, "clicks %d %d %d", n, hold, gap);
	host(r);
	for (i = 0; i < 20 && getsym("uin_ndown") - d0 < n; i++)
		pause_ms(100L);
	return getsym("uin_ndown") - d0;
}

/*
 * The Apple menu, held: it drops down only when MBState says the button
 * is down as the Mac takes the mouseDown.
 */
static void
menus()
{
	char *r;
	int i, ok = 0, bad = -1, diff;
	long d0 = getsym("uin_ndown");

	if (!t_check("menu_reached", moveto(20, 9), "mouse %#lx", getsym("uin_mouse")))
		return;
	for (i = 0; i < 20; i++) {
		host("button 1");
		pause_ms(i & 1 ? 100L : 500L);
		host("shot menu");
		host("button 0");
		pause_ms(i & 2 ? 1500L : 300L);
		host("shot nomenu");
		diff = 0;
		if ((r = host("cmp menu nomenu")) != 0)
			sscanf(r, "diff %d", &diff);
		if (diff > 1000)
			ok++;
		else if (bad < 0)
			bad = i;
	}
	t_check("menu_opens", ok == 20, "%d of 20, first miss %d, mouseDowns %ld", ok, bad,
	    getsym("uin_ndown") - d0);
}

/* the desktop without an alert; the Mac setting the mouse; the disk opened */
static void
desk()
{
	unsigned char *lm;
	char *r;
	long m0, m1, want, n, dbl = -1;
	int id, w0 = 0, w1 = 0, chk = 0;

	r = host("mac mac_after 220 120 580 230");
	if (r)
		sscanf(r, "checker %d", &chk);
	t_check("no_alert", chk >= 900, "middle of the screen: checker %d per mille", chk);
	/* a fast move goes farther than it went, as the Mac's mouse speed says */
	moveto(scrw / 4, scrh / 2);
	m0 = getsym("uin_mouse");
	host("move 60 0");
	pause_ms(300L);
	m1 = getsym("uin_mouse");
	t_info("mouse_accel", "moved 60, cursor %d", (int)((short)m1 - (short)m0));
	/* RawMouse and MTemp written with CrsrNew set: the Mac moves the mouse */
	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, 0);
	if (lm && lm != (unsigned char *)-1) {
		want = 200L << 16 | 300;
		*(long *)(lm + 0x828) = want;
		*(long *)(lm + 0x82c) = want;
		lm[0x8ce] = 1;
		pause_ms(300L);
		t_check("mouse_set_by_mac", getsym("uin_mouse") == want && L32(0x830) == want,
		    "kernel %#lx, Mouse %#lx", getsym("uin_mouse"), L32(0x830));
		dbl = L32(0x2f0);
		shmdt((char *)lm);
	}
	/* the startup disk, top right: a double click opens its window */
	r = host("mac mac_after");
	if (r)
		sscanf(r, "checker %d", &w0);
	t_check("disk_icon_reached", moveto(scrw - 40, 48), "mouse %#lx", getsym("uin_mouse"));
	n = clicks(20, 30, 100);
	t_check("clicks_fast", n == 20, "20 clicks held 30 ms: %ld mouseDowns", n);
	/* the mouse reports every 20 ms: shorter clicks may not reach the kernel */
	t_info("clicks_short", "20 clicks held 8 ms: %ld mouseDowns", clicks(20, 8, 60));
	menus();
	m0 = getsym("uin_nbtn");
	host("click 2");
	t_info("dblclick", "DoubleTime %ld, button events taken %ld", dbl,
	    getsym("uin_nbtn") - m0);
	pause_ms(4000L);
	host("shot mac_window");
	r = host("mac mac_window");
	if (r)
		sscanf(r, "checker %d", &w1);
	/* a window covers a good part of the desktop pattern */
	t_check("disk_window", w0 - w1 >= 100, "checker %d -> %d per mille", w0, w1);
}

/* the Mac's screen, in front: dump, console by hotkey and back */
static void
screen()
{
	char *r;
	int chk = 0, white = 0, top = 0;

	hostopen();
	if (hfd < 0) {
		t_skip("mac_screen", "no host line");
		return;
	}
	r = host("shot mac_desktop");
	t_check("mac_shot", r && strcmp(r, "error") != 0, "%s", r ? r : "no answer");
	if (r)
		sscanf(r, "%d %d", &scrw, &scrh);
	r = host("mac mac_desktop");
	if (r)
		sscanf(r, "checker %d white %d top %d", &chk, &white, &top);
	t_info("mac_screen_stats", "%s", r ? r : "none");
	/* the grey desktop: most of the screen a 50% dither */
	t_check("mac_screen", chk >= 500, "checker %d per mille", chk);
	host("key ctrl+alt+meta_l+0");
	t_check("hotkey_console", waitfront(0L, 5), "front %ld", front());
	host("shot mac_console");
	host("key ctrl+alt+meta_l+1");
	t_check("hotkey_mac", waitfront(macid, 5), "front %ld", front());
	input();
	desk();
	close(hfd);
	hfd = -1;
}

#define	FIDD	"/etc/aux/fidd"
static pid_t fiddpg;

/* the File ID daemon, started as A/UX does before the Mac environment */
static void
fidd()
{
	struct stat sb;
	pid_t pid;
	int st = -1;
	FILE *f;

	if (stat(FIDD, &sb) < 0) {
		t_skip("fidd", "no fidd on this root");
		return;
	}
	/* A/UX's mount table, which fidd reads: the root and its device */
	if (stat("/etc/mtab", &sb) < 0 && stat("/", &sb) == 0 &&
	    (f = fopen("/etc/mtab", "w")) != 0) {
		fprintf(f, "/dev/dsk/c0d0s0 / 4.2 rw,noquota,dev=%x 1 1\n",
		    (int)(major(sb.st_dev) << 8 | minor(sb.st_dev)));
		fclose(f);
	}
	if ((pid = fork()) == 0) {
		setpgrp();
		execl(FIDD, "fidd", "-d", (char *)0);
		_exit(127);
	}
	fiddpg = pid;
	/* it forks and the parent returns */
	t_waitchild(pid, &st, 10);
	t_check("fidd", st == 0, "status %#x", st);
}

/*
 * The startup disk's Desktop Folder, a folder at the root as A/UX
 * installs it.  Without it the Mac side makes a symbolic link after
 * reading the root and the Finder cannot find it.
 */
static void
deskfolder()
{
	t_check("desktop_folder", mkdir("/Desktop Folder", 0777) == 0 || errno == EEXIST,
	    "%s", T_ERR);
}

static void
fiddlog()
{
	char b[160];
	FILE *f;
	int n = 0;

	if (fiddpg > 0)
		kill(-fiddpg, SIGTERM);
	if ((f = fopen("/tmp/fid_log", "r")) == 0)
		return;
	while (fgets(b, sizeof b, f) && n++ < 40)
		printf("fidd| %s", b);
	fclose(f);
	fflush(stdout);
}

static void
startmac()
{
	static char *av[] = { "/mac/bin/startmac", 0 };
	char pat[80], *q1, *o, *s, *z;
	int p[2], st, id, fd;
	long npriv0, npriv;
	pid_t pid;

	t_rearm(240);
	setsym("uinter_trace", 1L);
	setsym("aux_trace", 4L);
	npriv0 = getsym("guest_npriv");
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		setpgrp();
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		for (fd = 3; fd < 20; fd++)
			close(fd);
		execve(av[0], av, macenv);
		_exit(127);
	}
	close(p[1]);
	macpid = pid;
	printf("INFO mac.startmac_pid %d\n", (int)pid);
	fflush(stdout);
	relay(p[0], 90);
	if (macid)
		screen();
	kill(-pid, SIGKILL);
	close(p[0]);
	st = 0;
	if (t_waitchild(pid, &st, 20) < 0)
		st = -1;
	setsym("aux_trace", 0L);
	setsym("uinter_trace", 0L);
	npriv = getsym("guest_npriv");
	t_info("startmac_status", "%#x", st);
	if (novideo)
		t_skip("past_dovideo", "no video device (uinter without screens)");
	else {
		t_check("past_dovideo", zones == 3, "no Memory Manager report");
		t_check("mac_front", macid > 0, "the Mac's session never came to front");
	}
	if (macid > 0)
		t_check("console_after_exit", waitfront(0L, 5), "front %ld", front());
	t_info("privileged_emulated", "%ld", npriv - npriv0);
	if ((id = shmget(TLOW, 0, 0)) >= 0)
		shmctl(id, IPC_RMID, (struct shmid_ds *)0);
	if (!t_check("trace", readtrace() == 0, "no kernel trace ring"))
		return;
	/* the trace into the log: the Mac task's opens and uinter commands */
	for (s = klog; *s; s = z) {
		for (z = s; *z && *z != '\n'; z++)
			;
		if (atoi(s) == pid)
			printf("ktrace| %.*s\n", (int)(z - s), s);
		if (*z)
			z++;
	}
	fflush(stdout);
	sprintf(pat, "%d Q01 0\n", (int)pid);
	q1 = strstr(klog, pat);
	t_check("ui_set", q1 != 0, "no UI_SET by pid %d in the trace", (int)pid);
	t_check("past_dodispatch", zones == 3, "no Memory Manager report (SysZone, ApplZone)");
	o = 0;
	sprintf(pat, "%d open ", (int)pid);
	for (s = q1 ? q1 : klog + klen; (s = strstr(s, pat)) != 0; s++) {
		z = strchr(s + strlen(pat), ' ');
		if (z && z - 6 >= s && strncmp(z - 6, "System", 6) == 0 && z[-7] == '/' &&
		    atoi(z + 1) >= 0) {
			o = s;
			break;
		}
	}
	t_check("system_opened", o != 0, "no open of the System file after UI_SET");
	if (o)
		t_info("system_open", "%.*s", (int)(strchr(o, '\n') - o), o);
}

/* PRAM kept for the next session; startmac again: the desktop, no alert */
static void
restart()
{
	static char *av[] = { "/mac/bin/startmac", 0 };
	struct stat sb;
	char *r;
	int p[2], st, fd, chk = 0;
	pid_t pid;

	t_check("pram_saved", stat("/etc/aux/pram", &sb) == 0 && sb.st_size == 256,
	    "/etc/aux/pram: %s", T_ERR);
	t_rearm(240);
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		setpgrp();
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		for (fd = 3; fd < 20; fd++)
			close(fd);
		execve(av[0], av, macenv);
		_exit(127);
	}
	close(p[1]);
	macid = 0;
	relay(p[0], 90);
	t_check("restart_front", macid > 0, "the Mac's session never came to front");
	if (macid > 0) {
		hostopen();
		r = host("shot mac_restart");
		r = r ? host("mac mac_restart 220 120 580 230") : 0;
		if (r)
			sscanf(r, "checker %d", &chk);
		t_check("restart_no_alert", chk >= 900, "middle of the screen: checker %d per mille",
		    chk);
		if (hfd >= 0)
			close(hfd);
		hfd = -1;
	}
	/* quit: a hangup, as a closed login line sends */
	kill(-pid, SIGHUP);
	st = 0;
	if (t_waitchild(pid, &st, 20) < 0)
		st = -1;
	t_info("restart_quit_status", "%#x", st);
	close(p[0]);
	t_check("restart_console", waitfront(0L, 5), "front %ld", front());
}

int
main()
{
	struct mod_mreg reg;
	struct mod_execreg er;
	struct stat sb;
	int e, mj = UIMAJ;

	t_init("mac", 300);
	if (t_kmem("guest_loading") == -1) {
		t_skip("all", "kernel has no guest support");
		return t_done();
	}
	if (stat(MD "/uinter", &sb) < 0 || stat("/dev/uinter0", &sb) < 0) {
		t_skip("all", "no uinter module on this root");
		return t_done();
	}
	e = modpath(MD) < 0 ? errno : 0;
	strcpy(reg.md_modname, "auxexec");
	reg.md_typedata = (caddr_t)&er;
	er.er_magic = 0x150;
	er.er_flags = EXF_FIRST;
	if (!t_check("register_exec", e == 0 && modadm(MOD_TY_EXEC, MOD_C_MREG, &reg) == 0,
	    "modpath %d, modadm: %s", e, T_ERR))
		return t_done();
	strcpy(reg.md_modname, "uinter");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0,
	    "modadm: %s", T_ERR))
		return t_done();
	native();
	if (stat("/aux/bin/macabi", &sb) == 0)
		macabi();
	else
		t_skip("macabi", "no macabi on this root");
	if (stat("/mac/bin/startmac", &sb) < 0 || stat("/etc/aux/rom", &sb) < 0 ||
	    stat("/mac/sys/Sys7/System", &sb) < 0)
		t_skip("startmac", "no startmac, ROM or System file on this root");
	else {
		fidd();
		deskfolder();
		startmac();
		if (macid > 0)
			restart();
		fiddlog();
	}
	return t_done();
}
