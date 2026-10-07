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
 *   - macjoin: a second A/UX process joins a session (UI_SYNC,
 *     UI_ATTACHLAYER, UI_SWITCH): one runs Mac code at a time, the tick
 *     and signals go to the one running, a joiner killed waiting or
 *     running hands the Mac back, and joiners go with the session;
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
 *
 * Built with SYS76 (t_mac76.c), the startmac run uses the Mac OS 7.6.1
 * System Folder /macsys/S761, on a ufs volume (SCSI disk 1), as a
 * Quadra 800 (box flag 29) and checks 'boot' 3's second _AUXDispatch(36)
 * (it makes $HOME/.mac/unix, 0700), the Finder desktop, SysVersion $0761, the
 * About This Computer window, SimpleText opened from the desktop and
 * quit, the disk window and root's Shut Down, which halts.  Before it a
 * user's session (/macsys/S761u) shows Log Out in the Special menu, then
 * a root session; both end with the Apple menu's Log Out.
 * Built with SYS81 (t_mac81.c), the same with Mac OS 8.1 (/macsys/S81),
 * without the user's session.
 * Built with SYS6 (t_mac6.c), A/UX 2.0.1's startmac runs System 6.0.7
 * from its own root, /a201, on the IIci's ROM in 4 MB; checks as t_mac's
 * up to the System file, then the Finder's desktop without an alert,
 * SysVersion $0607, the Apple and Special menus, and the startup disk's
 * window opened and closed.  The session ends from the Finder: the first
 * run in a boot by Special > Logout, the next by the Apple menu's Log Out.
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
#include <sys/resource.h>
#include <sys/procfs.h>
#include <dirent.h>
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
#define	OTHERUID "101"		/* restart's user, group display */

#ifdef SYS6
#define	ROOT6	"/a201"
#define	SYSDIR	ROOT6 "/mac/sys/Sys6"
#define	SYS6DIR	ROOT6 "/mac/sys/System Folder"
#define	TBSYS	"/mac/sys/System Folder"
#define	SYSVER	0x0607
#define	TNAME	"mac6"
#define	SHOT	"mac6_"
#define	TBMEM	"TBMEMORY=4M"
#define	STARTMAC ROOT6 "/mac/bin/startmac"
#else
#define	STARTMAC "/mac/bin/startmac"
#endif
#ifndef TBSYS
#define	TBSYS	SYSDIR
#endif
#define	MACVOL	"/macsys"
#define	MACDEV	"/dev/dsk/c1d0s0"
#ifdef SYS6
#else
#ifdef SYS81
#define	SYS76
#define	SYSDIR	MACVOL "/S81"
#define	SYSVER	0x0810
#define	TNAME	"mac81"
#define	SHOT	"mac81_"
#define	TBMEM	"TBMEMORY=16M"
#else
#ifdef SYS76
#define	SYSDIR	MACVOL "/S761"
#define	USERSYS	MACVOL "/S761u"
#define	SYSVER	0x0761
#define	TNAME	"mac76"
#define	SHOT	"mac76_"
#define	TBMEM	"TBMEMORY=16M"
#else
#define	SYSDIR	"/mac/sys/Sys7"
#define	TBMEM	"TBMEMORY=8M"
#endif
#endif
#endif
static char *macenv[] = {
	"PATH=/aux/bin:/usr/bin:/sbin", "HOME=/tmp", "TBVERBOSE=1", "TBWARN=1",
	"TBSYSTEM=" TBSYS, TBMEM, 0
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

#ifndef SYS76
/* a ROM for UI_ROM: the image file, or the host's at ROMBase */
static int
haverom()
{
	struct stat sb;
	long v;

	if (stat("/etc/aux/rom", &sb) == 0)
		return 1;
	return getsym("mac_rombase") == 0x40800000L &&
	    kmemrw(0x52800008L, &v, 0) == 0 && (v >> 16 & 0xffff) >= 0x67c;
}
#endif

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

/* out's "P name", "F name: why", "I name value" lines as results; 1 if it says done */
static int
results(pfx)
	char *pfx;
{
	char sub[100], *l, *e, *why;
	int done = 0;

	for (l = out; *l; l = *e ? e + 1 : e) {
		for (e = l; *e && *e != '\n'; e++)
			;
		if (e - l == 4 && strncmp(l, "done", 4) == 0)
			done = 1;
		if (e - l < 3 || e - l >= sizeof sub - 12 || l[1] != ' ' || l[0] == 'K')
			continue;
		sprintf(sub, "%s%.*s", pfx, (int)(e - l - 2), l + 2);
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
	return done;
}

/* macjoin m, output to fd; vm: Mac memory past the default VM limit */
static pid_t
joinrun(m, fd, vm)
	char *m;
	int fd, vm;
{
	static char *av[] = { "/aux/bin/macjoin", 0, 0 };
	struct rlimit rl;
	pid_t pid;

	av[1] = m;
	if ((pid = fork()) == 0) {
		if (vm && getrlimit(RLIMIT_VMEM, &rl) == 0) {
			rl.rlim_cur = rl.rlim_max;
			setrlimit(RLIMIT_VMEM, &rl);
		}
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
		}
		execve(av[0], av, macenv);
		_exit(127);
	}
	return pid;
}

/*
 * The joiner starts first, from outside the session, with the default
 * VM limit; the session (macjoin c) a second later.  When the creator
 * says K pid, it is killed: that joiner must go with it and the layer
 * be free.
 */
static void
macjoin()
{
	int p[2], st, jst, n, got = 0, i;
	long t0 = t_now_ms(), w = 0;
	struct pollfd pf;
	pid_t jp, cp;
	char *k;

	t_rearm(90);
	if (pipe(p) < 0)
		return;
	jp = joinrun("j", p[1], 0);
	sleep(2);
	cp = joinrun("c", p[1], 1);
	close(p[1]);
	pf.fd = p[0];
	pf.events = POLLIN;
	while (got < sizeof out - 1 && t_now_ms() - t0 < 60000L) {
		out[got] = 0;
		if ((k = strstr(out, "\nK ")) != 0 && strchr(k + 1, '\n')) {
			w = atol(k + 3);
			break;
		}
		if (poll(&pf, 1L, 500) <= 0)
			continue;
		if ((n = read(p[0], out + got, sizeof out - 1 - got)) <= 0)
			break;
		got += n;
	}
	out[got] = 0;
	if (cp > 0)
		kill(cp, SIGKILL);
	if (cp < 0 || t_waitchild(cp, &st, 10) < 0)
		st = -1;
	if (jp > 0 && t_waitchild(jp, &jst, 10) < 0) {
		kill(jp, SIGKILL);
		jst = -1;
	}
	close(p[0]);
	t_check("macjoin", results("macjoin_") && w > 0, "creator status %#x, K %ld", st, w);
	t_check("macjoin_joiner_status", jp > 0 && jst == 0, "status %#x", jst);
	for (i = 0; w > 0 && i < 50 && (kill((pid_t)w, 0) == 0 || errno != ESRCH); i++)
		poll(0, 0L, 100);
	t_check("macjoin_joiner_goes_with_session", w > 0 && i < 50, "pid %ld still there", w);
	n = joinrun("t", -1, 0);
	t_check("macjoin_layer_free", n > 0 && t_waitchild(n, &st, 10) == n && st == 0,
	    "status %#x", st);
}

/* the macabi lines as results */
static void
macabi()
{
	static char *av[] = { "/aux/bin/macabi", 0, 0 };
	char pram[256], hw[16];
	unsigned long a = 0, info;
	int p[2], st, n, got = 0, done = 0, fd;
	struct pollfd pf;
	long t0 = t_now_ms();
	pid_t pid;

	t_rearm(90);
	/* where uinter keeps the host ROM's address, for macabi to compare */
	if (getksym("ui_hwrom", &a, &info) == 0) {
		sprintf(hw, "%lx", a);
		av[1] = hw;
	}
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
	done = results("macabi_");
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

/* startmac's output read and dropped after the relay, so its writes never block */
static pid_t
drain(fd)
int fd;
{
	char b[512];
	pid_t p;

	if ((p = fork()) == 0) {
		while (read(fd, b, sizeof b) > 0)
			;
		_exit(0);
	}
	return p;
}

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

/* until the kernel has had c button changes from the device, at most 2 s */
static void
btnwait(c)
long c;
{
	int i;

	for (i = 0; i < 400 && getsym("uin_nbtnin") < c; i++)
		pause_ms(5L);
}

/*
 * n clicks held hold ms, gap ms apart: mouseDowns the Mac took.  The
 * event queue keeps 32 events and drops the oldest, as the Mac's, so
 * the clicks go CHUNK at a time, each chunk taken before the next.
 * seen: each change waits until the kernel has it, as the mouse is
 * sampled in the host's time, which a busy host stretches past hold.
 */
#define	CHUNK	10

static long
clicks(n, hold, gap, seen)
int n, hold, gap, seen;
{
	char r[40];
	long d0 = getsym("uin_ndown"), e0, g0, b;
	int i, j, k, m;

	for (k = 0; k < n; k += m) {
		m = n - k < CHUNK ? n - k : CHUNK;
		e0 = getsym("uin_nev");
		g0 = getsym("uin_nget");
		sprintf(r, "clicks %d %d %d", m, hold, gap);
		for (j = 0; seen && j < m; j++) {
			b = getsym("uin_nbtnin");
			host("button 1 0");
			btnwait(b + 1);
			pause_ms((long)hold);
			host("button 0 0");
			btnwait(b + 2);
			pause_ms((long)gap);
		}
		if (!seen)
			host(r);
		/* until all are down or, after 1 s, all that came are taken */
		for (i = 0; i < 200 && getsym("uin_ndown") - d0 < k + m &&
		    (i < 10 || getsym("uin_nget") - g0 < getsym("uin_nev") - e0); i++)
			pause_ms(100L);
	}
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
	long m0, m1, want, n, dbl = -1, l0, b0;
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
	l0 = getsym("uin_nlost");
	b0 = getsym("uin_nbtnin");
	n = clicks(20, 30, 100, 1);
	t_check("clicks_fast", n == 20,
	    "20 clicks held 30 ms: %ld mouseDowns, %ld button changes, %ld events lost",
	    n, getsym("uin_nbtnin") - b0, getsym("uin_nlost") - l0);
	/* the mouse reports every 20 ms: shorter clicks may not reach the kernel */
	t_info("clicks_short", "20 clicks held 8 ms: %ld mouseDowns", clicks(20, 8, 60, 0));
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
static int fiddkill();

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
#ifdef SYS76
	/* the System Folders' volume, once */
	if (stat(MACVOL "/S81", &sb) == 0 || stat(MACVOL "/S761", &sb) == 0) {
		char l[120];
		int on = 0;

		if ((f = fopen("/etc/mtab", "r")) != 0) {
			while (fgets(l, sizeof l, f))
				on |= strstr(l, " " MACVOL " ") != 0;
			fclose(f);
		}
		if (!on && (f = fopen("/etc/mtab", "a")) != 0) {
			fprintf(f, MACDEV " " MACVOL " 4.2 rw,noquota,dev=%x 1 1\n",
			    (int)(major(sb.st_dev) << 8 | minor(sb.st_dev)));
			fclose(f);
		}
	}
#endif
	fiddkill();
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

/*
 * Kill every fidd: the daemon forks into its own process group, and one
 * left over serves the next run's fidop messages.  0 once none is left.
 */
static int
fiddkill()
{
	char path[32];
	prpsinfo_t ps;
	struct dirent *de;
	DIR *d;
	int fd, i, n = 0, left = 1;

	for (i = 0; i < 50 && left; i++) {
		if (i)
			pause_ms(100L);
		left = 0;
		if ((d = opendir("/proc")) == 0)
			return -1;
		while ((de = readdir(d)) != 0) {
			if (de->d_name[0] == '.')
				continue;
			sprintf(path, "/proc/%s", de->d_name);
			if ((fd = open(path, O_RDONLY)) < 0)
				continue;
			if (ioctl(fd, PIOCPSINFO, &ps) == 0 && ps.pr_zomb == 0 &&
			    strcmp(ps.pr_fname, "fidd") == 0) {
				kill(ps.pr_pid, SIGKILL);
				left++;
				n++;
			}
			close(fd);
		}
		closedir(d);
	}
	t_info("fidd_killed", "%d", n);
	return left;
}

static void
fiddlog()
{
	char b[160];
	FILE *f;
	int n = 0;

	if (fiddpg > 0)
		kill(-fiddpg, SIGTERM);
	t_check("fidd_stopped", fiddkill() == 0, "a fidd outlived SIGKILL");
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
	pid_t pid, dp;

	t_rearm(240);
	/* opens and commands from here on; the ring keeps the first 8 KB */
	setsym("aux_tpos", 0L);
	setsym("uinter_trace", 1L);
#ifdef SYS6
	setsym("aux_trace", 4L | 8L | 16L);
#else
	setsym("aux_trace", 4L | 16L);
#endif
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
#ifdef SYS6
		if (chroot(ROOT6) < 0 || chdir("/") < 0)
			_exit(126);
#endif
		execve(av[0], av, macenv);
		_exit(127);
	}
	close(p[1]);
	macpid = pid;
	printf("INFO mac.startmac_pid %d\n", (int)pid);
	fflush(stdout);
	relay(p[0], 90);
	dp = drain(p[0]);
#ifdef SYS6
	if (macid)
		screen6();
#else
#ifdef SYS76
	if (macid)
		screen76();
#else
	if (macid)
		screen();
#endif
#endif
#ifdef SYS6
	/* a hangup, as a closed login line sends; then the kill */
	kill(-pid, SIGHUP);
	st = 0;
	t_info("hup_status", "%d %#x", t_waitchild(pid, &st, 20), st);
#endif
	kill(-pid, SIGKILL);
	close(p[0]);
	st = 0;
	if (t_waitchild(pid, &st, 20) < 0)
		st = -1;
	if (dp > 0) {
		kill(dp, SIGKILL);
		waitpid(dp, (int *)0, 0);
	}
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
	/* killed: Mac RAM goes with it, or it blocks the next startmac */
	t_check("mac_ram_freed", shmget(TLOW, 0, 0) < 0, "'tLOW' left after SIGKILL");
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
		if (z && z - 6 >= s && strncmp(z - 6, "System", 6) == 0 &&
		    (z[-7] == '/' || (z[-7] == '%' && z[-8] == '/')) && atoi(z + 1) >= 0) {
			o = s;
			break;
		}
	}
	t_check("system_opened", o != 0, "no open of the System file after UI_SET");
	if (o)
		t_info("system_open", "%.*s", (int)(strchr(o, '\n') - o), o);
}

/* pid holding a write lock on path, 0 if none, -1 on error */
static long
holder(path)
	char *path;
{
	struct flock fl;
	int fd = open(path, O_RDWR);

	if (fd < 0)
		return -1;
	memset((char *)&fl, 0, sizeof fl);
	fl.l_type = F_WRLCK;
	if (fcntl(fd, F_GETLK, &fl) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return fl.l_type == F_UNLCK ? 0 : (long)fl.l_pid;
}

#define	ENVSH	"/tests/startmac.sh"	/* the startmac script, with -e */
#define	ENVF	"/tmp/Mac/r1/.env"
#define	ENVSTAMP "/mac/lib/System Folder/.stamp"

/* environment r1 for the script: its stamp current, the System in TBSYSTEM */
static void
envmake()
{
	system("/usr/bin/mkdir -p '/mac/lib/System Folder' '/tmp/Mac/r1/System Folder'");
	system("[ -f '" ENVSTAMP "' ] || echo test > '" ENVSTAMP "'");
	system("/usr/bin/cp '" ENVSTAMP "' '/tmp/Mac/r1/System Folder/.stamp'");
	system("echo > '/tmp/Mac/r1/System Folder/System'");
}

/* startmac -e r1 to completion; its exit status */
static int
envsecond()
{
	static char *av[] = { "/sbin/sh", ENVSH, "-e", "r1", 0 };
	int fd, st;
	pid_t p;

	if ((p = fork()) == 0) {
		fd = open("/tmp/envmac.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
		dup2(fd, 1);
		dup2(fd, 2);
		execve(av[0], av, macenv);
		_exit(127);
	}
	if (p < 0 || t_waitchild(p, &st, 30) < 0 || !WIFEXITED(st))
		return -1;
	return WEXITSTATUS(st);
}

/*
 * PRAM kept for the next session; startmac again, as another user
 * (group display) after the killed one: the desktop, no alert.
 */
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
	/* the user's System Folder and console, as after a login */
	system("/usr/bin/chown -R " OTHERUID " " SYSDIR " /dev/console");
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		setpgid(0, 0);	/* its own group, the console still its terminal */
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		for (fd = 3; fd < 20; fd++)
			close(fd);
		if (setgid(25) == 0 && setuid(atoi(OTHERUID)) == 0)
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
	system("/usr/bin/chown -R 0 " SYSDIR " /dev/console");
	t_check("restart_console", waitfront(0L, 5), "front %ld", front());
}

/* the startmac script with -e: the session holds the lock, a second is refused */
static void
envrun()
{
	static char *av[] = { "/sbin/sh", ENVSH, "-e", "r1", 0 };
	char b[256];
	int p[2], st, fd, n;
	pid_t pid;

	t_rearm(240);
	envmake();
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
	t_check("env_front", macid > 0, "the Mac's session never came to front");
	t_check("env_held", holder(ENVF) > 0, "holder %ld", holder(ENVF));
	st = envsecond();
	b[0] = 0;
	if ((fd = open("/tmp/envmac.log", O_RDONLY)) >= 0) {
		n = read(fd, b, sizeof b - 1);
		b[n > 0 ? n : 0] = 0;
		close(fd);
	}
	t_check("env_second", st == 1 && strstr(b, "in use") != 0, "status %d: %s", st, b);
	kill(-pid, SIGHUP);
	if (t_waitchild(pid, &st, 20) < 0)
		st = -1;
	close(p[0]);
	for (n = 0; n < 50 && holder(ENVF) > 0; n++)
		pause_ms(200L);
	t_check("env_released", holder(ENVF) == 0, "holder %ld", holder(ENVF));
	t_check("env_console", waitfront(0L, 5), "front %ld", front());
	if ((n = shmget(TLOW, 0, 0)) >= 0)
		shmctl(n, IPC_RMID, (struct shmid_ds *)0);
	system("/usr/bin/rm -rf /tmp/Mac");
}

#ifdef SYS76
/* the model Patch.067C keeps for Gestalt 'mach' (its own + 6), read from the task */
static long
patchbox()
{
	char path[32];
	long v = -1;
	int fd;

	sprintf(path, "/proc/%05d", (int)macpid);
	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	if (lseek(fd, 0x87cdcL, 0) == -1 || read(fd, (char *)&v, 4) != 4)
		v = -1;
	close(fd);
	return v;
}
#endif

#if defined(SYS76) || defined(SYS6)
/* pixels changed between two dumps, -1 without an answer */
static int
shotdiff(a, b)
char *a, *b;
{
	char req[64], *r;
	int n = -1;

	sprintf(req, "cmp %s %s", a, b);
	if ((r = host(req)) != 0 && sscanf(r, "diff %d", &n) != 1)
		n = strcmp(r, "same") == 0 ? 0 : -1;
	return n;
}

/* the Mac still running: its ticks advance */
static int
alive76()
{
	unsigned char *lm;
	long t0, t1;
	int id;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!lm || lm == (unsigned char *)-1)
		return 0;
	t0 = L32(0x16a);
	pause_ms(500L);
	t1 = L32(0x16a);
	shmdt((char *)lm);
	return kill(macpid, 0) == 0 && t1 - t0 >= 20;
}

/*
 * A dump name taken until it differs from ref by lo to hi pixels or secs
 * pass, then again once the screen has settled: the pixels changed.
 */
static int
waitshot(name, ref, lo, hi, secs)
char *name, *ref;
int lo, hi, secs;
{
	char req[40];
	int i, n;

	sprintf(req, "shot %s", name);
	for (i = 0; i < secs; i += 2) {
		host(req);
		n = shotdiff(ref, name);
		if (n >= lo && n <= hi)
			break;
		pause_ms(2000L);
	}
	pause_ms(2000L);
	host(req);
	return shotdiff(ref, name);
}

/* a menu item chosen: the menu held open at (h, v), released over (ih, iv) */
static void
menuitem(h, v, ih, iv, shot)
int h, v, ih, iv;
char *shot;
{
	char req[40];

	/* a halt recorded, the session killed in its place */
	setsym("uinter_adcall", 0L);
	setsym("uinter_adtest", 1L);
	moveto(h, v);
	host("button 1");
	pause_ms(800L);
	moveto(ih, iv);
	pause_ms(500L);
	sprintf(req, "shot %s", shot);
	host(req);
	host("button 0");
}

/* the session ended from the Mac: startmac exits by itself, nothing halts */
static void
loggedout(pfx)
char *pfx;
{
	char n[40];
	long t0;
	int st = 0;
	pid_t r = 0;

	t_rearm(120);
	for (t0 = t_now_ms(); t_now_ms() - t0 < 60000L; pause_ms(500L))
		if ((r = waitpid(macpid, &st, WNOHANG)) != 0)
			break;
	sprintf(n, "%s_exits", pfx);
	t_check(n, r == macpid, "startmac still running after %ld ms", t_now_ms() - t0);
	sprintf(n, "%s_clean", pfx);
	/* doLogout's _exit(0); a fault or any other exit is not a Log Out */
	t_check(n, r == macpid && WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %#x", st);
	if (r == macpid) {
		sprintf(n, "%s_status", pfx);
		t_info(n, "%#x after %ld ms", st, t_now_ms() - t0);
		macpid = 0;
	}
	sprintf(n, "%s_no_halt", pfx);
	t_check(n, getsym("uinter_adcall") == 0, "uadmin %#lx", getsym("uinter_adcall"));
	setsym("uinter_adtest", 0L);
	sprintf(n, "%s_console", pfx);
	t_check(n, waitfront(0L, 10), "front %ld", front());
}
#endif

#ifdef SYS76

/* n bytes of Mac memory at a, as an INFO line */
static void
macdump(name, a, n)
char *name;
long a;
int n;
{
	unsigned char *lm;
	char b[200];
	int id, i;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!lm || lm == (unsigned char *)-1)
		return;
	for (i = 0; i < n && i < 64; i++)
		sprintf(b + 2 * i, "%02x", lm[a + i]);
	t_info(name, "%lx %s", a, b);
	shmdt((char *)lm);
}

#define	BE32(p)	((long)(p)[0] << 24 | (long)(p)[1] << 16 | (p)[2] << 8 | (p)[3])


/* the shutdown queue (SDHeader at $5AF40): each procedure, its flags and first bytes */
static void
sdqueue()
{
	struct shmid_ds ds;
	unsigned char *lm, *e;
	long q, pr[8];
	char b[40];
	int id, i, n;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!lm || lm == (unsigned char *)-1)
		return;
	shmctl(id, IPC_STAT, &ds);
	for (q = BE32(lm + 0x5af42), n = 0; q > 0 && q + 10 < ds.shm_segsz && n < 8;
	    q = BE32(e), n++) {
		e = lm + q;
		pr[n] = BE32(e + 6);
		sprintf(b, "sdqueue_%d_flags", n);
		t_info(b, "%d", e[4] << 8 | e[5]);
	}
	shmdt((char *)lm);
	for (i = 0; i < n; i++) {
		sprintf(b, "sdqueue_%d", i);
		macdump(b, pr[i], 32);
	}
}

/* Patch.067C's LAP Manager call at $38244 goes through its checked dispatcher */
static int
lapredirect()
{
	static unsigned char want[10] = {
		0x43, 0xfa, 0xe1, 0xea, 0x70, 0x19, 0x4e, 0xba, 0xe2, 0x0e
	};
	unsigned char *lm;
	int id, ok;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!lm || lm == (unsigned char *)-1)
		return 0;
	ok = memcmp(lm + 0x38244, want, sizeof want) == 0;
	shmdt((char *)lm);
	return ok;
}

/* ShutDwnPower's and ShutDwnStart's shutDownDialog calls: 1 both skipped, 0 both kept */
static int
sdskipped()
{
	unsigned char *lm;
	int id, n = 0;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!lm || lm == (unsigned char *)-1)
		return -1;
	n += memcmp(lm + 0xd16a, "\x70\x00\x4e\x71", 4) == 0;
	n += memcmp(lm + 0xd1d2, "\x70\x00\x4e\x71", 4) == 0;
	shmdt((char *)lm);
	return n == 2 ? 1 : n == 0 ? 0 : -1;
}

/* A/UX's resources, under the name with a space Shut Down looks for */
#define	AUXRES		"/mac/lib/Resources/%AUXResources"
#define	AUXRESSP	"/mac/lib/Resources/%AUX Resources"

/* Special > Shut Down as root: the Mac ends by itself, with no panic */
static void
shutdown76()
{
	long t0;
	int st = 0;
	pid_t r = 0;

	t_rearm(120);
	/* Shut Down's dialog, without which it logs out */
	link(AUXRES, AUXRESSP);
	/* the halt recorded, the session killed in its place */
	setsym("uinter_adcall", 0L);
	setsym("uinter_adtest", 1L);
	moveto(232, 9);
	host("button 1");
	pause_ms(800L);
	moveto(250, 139);
	pause_ms(500L);
	host("shot " SHOT "shutdown");
	host("button 0");
	for (t0 = t_now_ms(); t_now_ms() - t0 < 60000L; pause_ms(500L))
		if ((r = waitpid(macpid, &st, WNOHANG)) != 0)
			break;
	t_check("shutdown_exits", r == macpid, "startmac still running after %ld ms",
	    t_now_ms() - t0);
	if (r == macpid) {
		t_info("shutdown_status", "%#x after %ld ms", st, t_now_ms() - t0);
		macpid = 0;
	}
	/* a fault (PC $17) ends it with SIGILL before the halt */
	t_check("shutdown_uadmin", getsym("uinter_adcall") == 0x200 &&
	    WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "uadmin %#lx, status %#x",
	    getsym("uinter_adcall"), st);
	setsym("uinter_adtest", 0L);
	unlink(AUXRESSP);
}

/*
 * Root's Special > Shut Down, for real: the console's halt message must
 * follow the HALT line, with no panic between.  Only the run's last
 * stage may do this.
 */
static void
halt76()
{
	static char line[] = "HALT mac76.shutdown_halts\n";
	long t0;
	int st = 0;
	pid_t r = 0;

	t_rearm(120);
	link(AUXRES, AUXRESSP);
	setsym("uinter_adtest", 0L);
	sync();
	write(1, line, sizeof line - 1);
	moveto(232, 9);
	host("button 1");
	pause_ms(800L);
	moveto(250, 139);
	pause_ms(500L);
	host("button 0");
	for (t0 = t_now_ms(); t_now_ms() - t0 < 60000L; pause_ms(500L))
		if ((r = waitpid(macpid, &st, WNOHANG)) != 0)
			break;
	t_fail("shutdown_halts", "startmac %s, status %#x", r == macpid ? "exited" : "still running", st);
	if (r == macpid)
		macpid = 0;
	unlink(AUXRESSP);
}

/* the Mac menus' Log Out item */
#define	LOGOUTV	60

/* path and everything under it owned by uid */
static void
chownr(path, uid)
char *path;
int uid;
{
	char sub[256];
	struct dirent *de;
	DIR *d;

	chown(path, uid, -1);
	if ((d = opendir(path)) == 0)
		return;
	while ((de = readdir(d)) != 0)
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..") &&
		    strlen(path) + strlen(de->d_name) + 2 < sizeof sub) {
			sprintf(sub, "%s/%s", path, de->d_name);
			chownr(sub, uid);
		}
	closedir(d);
}

/*
 * $HOME/.mac/<host> (HOME=/tmp), the Trash and Temporary Items: mode
 * 0700, which uid other cannot open.  A file moved to the Trash and
 * back keeps its mode.
 */
#define	DOTMAC	"/tmp/.mac/unix"
static void
dotmac(n, uid, other)
char *n;
int uid, other;
{
	struct stat sb;
	char m[40];
	pid_t pid;
	int st, fd, k;

	sb.st_mode = 0;
	t_check(n, stat(DOTMAC, &sb) == 0, "no " DOTMAC " from 'boot' 3's _AUXDispatch(36, 0)");
	sprintf(m, "%s_mode", n);
	t_check(m, (sb.st_mode & 07777) == 0700 && sb.st_uid == uid, "mode %o, uid %d",
	    (int)sb.st_mode & 07777, (int)sb.st_uid);
	/* other cannot open it; uid moves a file to the Trash and back */
	for (k = 0; k < 2; k++) {
		st = -1;
		if ((pid = fork()) == 0) {
			if (setuid(k ? uid : other) < 0)
				_exit(2);
			if (k == 0)
				_exit(open(DOTMAC, O_RDONLY) < 0 && errno == EACCES ? 0 : 1);
			umask(0);
			unlink("/tmp/dmfile");
			if ((fd = open("/tmp/dmfile", O_WRONLY | O_CREAT, 0640)) < 0)
				_exit(3);
			close(fd);
			mkdir(DOTMAC "/Trash", 0700);
			if (rename("/tmp/dmfile", DOTMAC "/Trash/dmfile") < 0 ||
			    rename(DOTMAC "/Trash/dmfile", "/tmp/dmfile") < 0)
				_exit(4);
			_exit(stat("/tmp/dmfile", &sb) == 0 && (sb.st_mode & 0777) == 0640 &&
			    sb.st_uid == uid && unlink("/tmp/dmfile") == 0 ? 0 : 5);
		}
		if (pid > 0)
			t_waitchild(pid, &st, 10);
		sprintf(m, k ? "%s_roundtrip" : "%s_private", n);
		t_check(m, st == 0, "status %x", st);
	}
}

/*
 * A session as uid with the System Folder sys: its desktop, for a user
 * the Special menu, then the Apple menu's Log Out ends it.
 */
static void
session76(uid, sys, pfx)
int uid;
char *sys, *pfx;
{
	static char *av[] = { "/mac/bin/startmac", 0 };
	char *env[10], tb[80], n[40], shot[40], req[60], *r;
	int p[2], fd, i, st, chk = 0, white = 0, top = 0;
	pid_t pid, dp;

	t_rearm(240);
	for (i = 0; macenv[i] && i < 9; i++)
		env[i] = strncmp(macenv[i], "TBSYSTEM=", 9) ? macenv[i] : tb;
	env[i] = 0;
	sprintf(tb, "TBSYSTEM=%s", sys);
	/* the user's System Folder and console, as after a login */
	chownr(sys, uid);
	chown("/dev/console", uid, -1);
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		setpgid(0, 0);	/* its own group, the console still its terminal */
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		for (fd = 3; fd < 20; fd++)
			close(fd);
		if (uid == 0 || (setgid(25) == 0 && setuid(uid) == 0))
			execve(av[0], av, env);
		_exit(127);
	}
	close(p[1]);
	macpid = pid;
	macid = 0;
	relay(p[0], 90);
	dp = drain(p[0]);
	sprintf(n, "%s_front", pfx);
	t_check(n, macid > 0, "the Mac's session never came to front");
	hostopen();
	if (macid > 0 && hfd >= 0) {
		sprintf(shot, "%s%s_desktop", SHOT, pfx);
		sprintf(req, "shot %s", shot);
		for (i = 0; i < 8; i++) {
			t_rearm(120);
			host(req);
			sprintf(req, "mac %s", shot);
			if ((r = host(req)) != 0)
				sscanf(r, "checker %d white %d top %d", &chk, &white, &top);
			sprintf(req, "shot %s", shot);
			if (chk >= 900 && top >= 900)
				break;
			pause_ms(10000L);
		}
		sprintf(n, "%s_desktop", pfx);
		t_check(n, chk >= 900 && top >= 900, "checker %d, menu bar %d per mille", chk, top);
		if (strcmp(pfx, "halt") == 0) {
			halt76();
			goto out;
		}
		if (uid) {
			/* Log Out in place of Restart and Shut Down */
			moveto(232, 9);
			host("button 1");
			pause_ms(800L);
			sprintf(req, "shot %s%s_special", SHOT, pfx);
			host(req);
			host("button 0");
			pause_ms(1000L);
			sprintf(req, "%s%s_special", SHOT, pfx);
			sprintf(n, "%s_special_menu", pfx);
			i = shotdiff(shot, req);
			t_check(n, i > 1000, "%d pixels changed", i);
		}
		/* AtalkHk2: -1, no LAP Manager */
		macdump("atalkhk2", 0xb18L, 4);
		sdqueue();
		sprintf(n, "%s_lap_redirect", pfx);
		t_check(n, lapredirect(), "");
		/* only root's Shut Down and Restart show A/UX's dialog */
		sprintf(n, "%s_sd_dialog", pfx);
		i = sdskipped();
		t_check(n, i == (uid != 0), "skipped %d", i);
		t_rearm(120);
		/* a user leaves through Special, root through the Apple menu */
		if (uid) {
			/* with the dialog's resources Special > Log Out still logs out */
			link(AUXRES, AUXRESSP);
			sprintf(req, "%s%s_speciallogout", SHOT, pfx);
			menuitem(232, 9, 250, 123, req);
		} else {
			sprintf(req, "%s%s_apple", SHOT, pfx);
			menuitem(20, 9, 60, LOGOUTV, req);
		}
		loggedout(pfx);
		unlink(AUXRESSP);
	}
out:
	if (hfd >= 0)
		close(hfd);
	hfd = -1;
	if (macpid) {
		kill(-pid, SIGKILL);
		t_waitchild(pid, &st, 20);
		macpid = 0;
	}
	close(p[0]);
	if (dp > 0) {
		kill(dp, SIGKILL);
		waitpid(dp, (int *)0, 0);
	}
	setsym("uinter_adtest", 0L);
	chownr(sys, 0);
	chown("/dev/console", 0, -1);
	sprintf(n, "%s_ram_freed", pfx);
	t_check(n, shmget(TLOW, 0, 0) < 0, "'tLOW' left");
	if ((i = shmget(TLOW, 0, 0)) >= 0)
		shmctl(i, IPC_RMID, (struct shmid_ds *)0);
	macid = 0;
}

/* a file's copy */
static void
copyf(a, b)
char *a, *b;
{
	char buf[4096];
	int f, g, n;

	if ((f = open(a, O_RDONLY)) < 0)
		return;
	if ((g = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
		while ((n = read(f, buf, sizeof buf)) > 0)
			write(g, buf, n);
		close(g);
	}
	close(f);
}

/* the System Folders' volume mounted (once a boot), 0 if none */
static int
macvol()
{
	struct stat sb;

	if (stat(MACDEV, &sb) < 0)
		return 0;
	if (stat(SYSDIR, &sb) == 0)
		return 1;
	return t_check("macvol_mount", system("/sbin/mount -F ufs " MACDEV " " MACVOL) == 0,
	    "mount " MACDEV " failed");
}

/* About closed, SimpleText opened from the desktop and quit, the disk window */
static void
app76()
{
	char *r;
	int n, chk = -1;

	t_rearm(120);
	host("key meta_l+w");
	n = waitshot(SHOT "closed", SHOT "desktop", 0, 1999, 20);
	t_check("about_closed", n >= 0 && n < 2000, "%d pixels from the desktop", n);
	/* typing selects the desktop icon by name; Command-O opens it */
	t_rearm(120);
	host("key s");
	pause_ms(1000L);
	host("key meta_l+o");
	n = waitshot(SHOT "simpletext", SHOT "closed", 100000, 1 << 30, 60);
	/* its untitled window, white over the middle of the desktop */
	if ((r = host("mac " SHOT "simpletext 200 150 600 450")) != 0)
		sscanf(r, "checker %d", &chk);
	t_check("simpletext_open", n >= 100000 && chk >= 0 && chk < 100,
	    "%d pixels changed, checker %d per mille", n, chk);
	t_check("simpletext_alive", alive76(), "");
	t_rearm(120);
	host("key meta_l+q");
	n = waitshot(SHOT "quit", SHOT "closed", 0, 1999, 40);
	t_check("simpletext_quit", n >= 0 && n < 2000, "%d pixels from the desktop", n);
	t_check("finder_alive", alive76(), "");
	/* the startup disk's window: the File Manager lists the root */
	t_rearm(120);
	moveto(scrw - 40, 48);
	host("click 2");
	n = waitshot(SHOT "disk", SHOT "quit", 20000, 1 << 30, 40);
	t_check("disk_window", n >= 20000, "%d pixels changed", n);
	t_rearm(120);
	host("key meta_l+w");
	n = waitshot(SHOT "diskclosed", SHOT "quit", 0, 1999, 20);
	t_check("disk_window_closed", n >= 0 && n < 2000, "%d pixels from the desktop", n);
	/* the Special menu held open */
	moveto(232, 9);
	host("button 1");
	pause_ms(800L);
	host("shot " SHOT "special");
	host("button 0");
	pause_ms(1000L);
	n = shotdiff(SHOT "diskclosed", SHOT "special");
	t_check("special_menu", n > 1000, "%d pixels changed", n);
	t_check("finder_stays", alive76(), "");
	shutdown76();
}

/* the 7.6.1 Finder: its desktop, the System version, About This Computer */
static void
screen76()
{
	unsigned char *lm;
	char *r;
	int id, n, chk = 0, white = 0, top = 0, diff = 0;

	hostopen();
	if (hfd < 0) {
		t_skip("mac_screen", "no host line");
		return;
	}
	/* the menu bar white, the rest mostly the desktop pattern */
	for (n = 0; n < 8; n++) {
		t_rearm(120);
		host("shot " SHOT "desktop");
		r = host("mac " SHOT "desktop");
		if (r)
			sscanf(r, "checker %d white %d top %d", &chk, &white, &top);
		if (chk >= 900 && top >= 900)
			break;
		pause_ms(10000L);
	}
	t_info("mac_screen_stats", "checker %d white %d top %d", chk, white, top);
	t_check("finder_desktop", chk >= 900 && top >= 900, "checker %d, menu bar %d per mille",
	    chk, top);
	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (lm && lm != (unsigned char *)-1) {
		t_check("sysversion", (lm[0x15a] << 8 | lm[0x15b]) == SYSVER, "SysVersion %#x",
		    lm[0x15a] << 8 | lm[0x15b]);
		t_check("gestalt_mach", patchbox() == 29, "Mac model %ld (BoxFlag %d)",
		    patchbox(), lm[0xcb3]);
		shmdt((char *)lm);
	}
	/* About This Computer, the Apple menu's first item */
	if (t_check("apple_menu", moveto(20, 9), "mouse %#lx", getsym("uin_mouse"))) {
		host("button 1");
		pause_ms(800L);
		moveto(60, 29);
		pause_ms(500L);
		host("button 0");
		pause_ms(5000L);
		host("shot " SHOT "about");
		if ((r = host("cmp " SHOT "desktop " SHOT "about")) != 0)
			sscanf(r, "diff %d", &diff);
		t_check("about_window", diff > 20000, "%d pixels changed", diff);
		app76();
	}
	close(hfd);
	hfd = -1;
}
#endif

#ifdef SYS6
#define	MAC6RAN	"/tmp/.t_mac6"	/* a run in this boot */

/* System 6's Finder: the desktop pattern under a white menu bar, SysVersion */
static void
screen6()
{
	struct stat sb;
	unsigned char *lm;
	char *r;
	int id, n, chk = 0, white = 0, top = 0;

	hostopen();
	if (hfd < 0) {
		t_skip("mac_screen", "no host line");
		return;
	}
	for (n = 0; n < 8; n++) {
		t_rearm(120);
		host("shot " SHOT "desktop");
		r = host("mac " SHOT "desktop");
		if (r)
			sscanf(r, "checker %d white %d top %d", &chk, &white, &top);
		if (chk >= 900 && top >= 900)
			break;
		pause_ms(10000L);
	}
	t_info("mac_screen_stats", "checker %d white %d top %d", chk, white, top);
	/* the Finder's menu bar over its desktop, no alert */
	if (!t_check("finder_desktop", chk >= 900 && top >= 900, "checker %d, menu bar %d per mille",
	    chk, top)) {
		close(hfd);
		hfd = -1;
		return;
	}
	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (lm && lm != (unsigned char *)-1) {
		t_check("sysversion", (lm[0x15a] << 8 | lm[0x15b]) == SYSVER, "SysVersion %#x",
		    lm[0x15a] << 8 | lm[0x15b]);
		shmdt((char *)lm);
	} else
		t_check("sysversion", 0, "no low memory");
	/* the Apple and Special menus held open */
	if (t_check("mouse", moveto(20, 9), "mouse %#lx", getsym("uin_mouse"))) {
		host("button 1");
		pause_ms(800L);
		host("shot " SHOT "apple");
		host("button 0");
		n = shotdiff(SHOT "desktop", SHOT "apple");
		t_check("apple_menu", n > 1000, "%d pixels changed", n);
	}
	pause_ms(1000L);
	moveto(184, 9);
	host("button 1");
	pause_ms(800L);
	host("shot " SHOT "special");
	host("button 0");
	n = shotdiff(SHOT "desktop", SHOT "special");
	t_check("special_menu", n > 1000, "%d pixels changed", n);
	/* the startup disk's window, opened from the middle of its icon and closed */
	t_rearm(120);
	pause_ms(1000L);
	moveto(scrw - 40, 51);
	host("click 2");
	n = waitshot(SHOT "disk", SHOT "desktop", 20000, 1 << 30, 40);
	t_check("disk_window", n >= 20000, "%d pixels changed", n);
	host("key meta_l+w");
	n = waitshot(SHOT "closed", SHOT "desktop", 0, 1999, 20);
	t_check("disk_window_closed", n >= 0 && n < 2000, "%d pixels from the desktop", n);
	t_check("finder_alive", alive76(), "");
	/*
	 * Logout runs the shutdown procedures, which close the Desktop
	 * Manager's files; a killed session leaves them damaged for the next
	 */
	t_rearm(120);
	if (stat(MAC6RAN, &sb) < 0) {
		close(creat(MAC6RAN, 0644));
		menuitem(184, 9, 189, 155, SHOT "logout");
	} else
		menuitem(20, 9, 60, 218, SHOT "applelogout");
	loggedout("logout");
	close(hfd);
	hfd = -1;
}

/* the System Folder's names with spaces, which the test root has without */
static char *sys6sp[] = { "%AUX Resources", "DA Handler", "Key Layout", "Scrapbook File", 0 };

static void
sys6links(on)
int on;
{
	char a[80], b[80], *s, *d;
	int i;

	for (i = 0; sys6sp[i]; i++) {
		sprintf(b, "%s/%s", SYS6DIR, sys6sp[i]);
		for (s = sys6sp[i], d = a + sprintf(a, "%s/", SYS6DIR); *s; s++)
			if (*s != ' ')
				*d++ = *s;
		*d = 0;
		if (on)
			link(a, b);
		else
			unlink(b);
	}
}
#endif

int
main(argc, argv)
int argc;
char **argv;
{
	struct mod_mreg reg;
	struct mod_execreg er;
	struct stat sb;
	int e, mj = UIMAJ;

#if defined(SYS76) || defined(SYS6)
	t_init(TNAME, 600);
#else
	t_init("mac", 300);
#endif
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
#if !defined(SYS76) && !defined(SYS6)
	native();
	e = haverom();
	if (stat("/aux/bin/macabi", &sb) < 0)
		t_skip("macabi", "no macabi on this root");
	else if (!e)
		t_skip("macabi", "no Mac ROM: no /etc/aux/rom, none at ROMBase");
	else
		macabi();
	if (stat("/aux/bin/macjoin", &sb) < 0)
		t_skip("macjoin", "no macjoin on this root");
	else if (!e)
		t_skip("macjoin", "no Mac ROM: no /etc/aux/rom, none at ROMBase");
	else
		macjoin();
#endif
#ifdef SYS76
	/* mounted for the rest of the boot */
	macvol();
#endif
#ifdef SYS6
	if (stat(STARTMAC, &sb) < 0 || stat(ROOT6 "/etc/aux/rom", &sb) < 0 ||
	    stat(SYSDIR "/System", &sb) < 0)
#else
	if (stat(STARTMAC, &sb) < 0 || stat("/etc/aux/rom", &sb) < 0 ||
	    stat(SYSDIR "/System", &sb) < 0)
#endif
		t_skip("startmac", "no startmac, ROM or System file on this root");
	else {
		fidd();
		deskfolder();
#ifdef SYS6
		/* its own ROM: the IIci's box flag loads it again */
		e = open("/dev/uinter0", O_RDWR);
		t_check("boxflag", setsym("uinter_boxflag", 5L) == 0, "no uinter_boxflag");
		t_check("sysfolder", rename(SYSDIR, SYS6DIR) == 0, "%s", T_ERR);
		sys6links(1);
		startmac();
		sys6links(0);
		rename(SYS6DIR, SYSDIR);
		setsym("uinter_boxflag", -1L);
		if (e >= 0)
			close(e);
#else
#ifdef SYS76
		/* the module loaded, then a Quadra 800 (no ProductInfo of its own) */
		e = open("/dev/uinter0", O_RDWR);
		t_check("boxflag", setsym("uinter_boxflag", 29L) == 0, "no uinter_boxflag");
#ifndef SYS81
		system("/usr/bin/rm -rf /tmp/.mac");
		/* the run's last stage: a root session that shuts down */
		if (argc > 1 && strcmp(argv[1], "halt") == 0) {
			session76(0, SYSDIR, "halt");
			return t_done();
		}
		session76(atoi(OTHERUID), USERSYS, "user");
		dotmac("user_dotmac", atoi(OTHERUID), atoi(OTHERUID) + 1);
#endif
		session76(0, SYSDIR, "root");
		/* made by _AUXDispatch(36, 0); t_mac's run leaves one */
		system("/usr/bin/rm -rf /tmp/.mac");
		/* SimpleText on the desktop, for the Finder to open */
		copyf(SYSDIR "/SimpleText", "/Desktop Folder/SimpleText");
		copyf(SYSDIR "/%SimpleText", "/Desktop Folder/%SimpleText");
		startmac();
		unlink("/Desktop Folder/SimpleText");
		unlink("/Desktop Folder/%SimpleText");
		dotmac("auxdispatch_36", 0, atoi(OTHERUID));
		setsym("uinter_boxflag", -1L);
		if (e >= 0)
			close(e);
#else
		startmac();
		if (macid > 0)
			restart();
		if (macid > 0 && stat(ENVSH, &sb) == 0)
			envrun();
#endif
#endif
		fiddlog();
	}
	return t_done();
}
