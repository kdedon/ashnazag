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

/* a guest long through /proc; 0 if not written */
static int
poke(a, v)
	long a, v;
{
	char path[32];
	unsigned char b[4];
	int fd, ok;

	sprintf(path, "/proc/%05ld", (long)tpid);
	if ((fd = open(path, O_RDWR)) < 0)
		return 0;
	b[0] = v >> 24;
	b[1] = v >> 16;
	b[2] = v >> 8;
	b[3] = v;
	ok = lseek(fd, a, 0) == a && write(fd, (char *)b, 4) == 4;
	close(fd);
	return ok;
}

#define	PVF(f)	(TOSPV + (long)&((struct tospv *)0)->f)

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

#define	UDIR	"/tmp/tosu"
#define	CDIR	"/tmp/tosc"		/* EmuTOS's C:; TOS 3.06 boots from /tos/sys */
#define	FVDIDIR	"/tos/fvdi"		/* fVDI's files for C: */

static int fvdi;			/* C: holds fVDI */
static char *fvdifiles[] = { "FVDI.SYS", "ASHFB.SYS", "AUTO/FVDI.PRG" };
#define	GDIR	"/tmp/tosg"

static char *utree[] = {
	"sub/new.txt", "sub/renamed.txt", "result.txt", "readme.txt", "hello.prg",
	"Long Name File.txt", "gone.txt", "in", "abs", "out", "up", 0
};

static void
wfile(p, s)
	char *p, *s;
{
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd >= 0) {
		write(fd, s, strlen(s));
		close(fd);
	}
}

/* two 201-character directories, a 250-character name and ok.txt in the inner one */
static void
deep(make)
	int make;
{
	char d[1024], *e;

	strcpy(d, UDIR "/d");
	memset(d + strlen(d), 'x', 200);
	e = d + strlen(UDIR) + 202;
	strcpy(e, "/e");
	memset(e + 2, 'x', 200);
	e[202] = 0;
	if (!make) {
		strcpy(e + 202, "/ok.txt");
		unlink(d);
		e[202] = '/';
		memset(e + 203, 'y', 250);
		e[453] = 0;
		unlink(d);
		e[202] = 0;
		rmdir(d);
		*e = 0;
		rmdir(d);
		return;
	}
	*e = 0;
	mkdir(d, 0755);
	*e = '/';
	mkdir(d, 0755);
	strcpy(e + 202, "/ok.txt");
	wfile(d, "ok\n");
	e[202] = '/';
	memset(e + 203, 'y', 250);
	e[453] = 0;
	wfile(d, "long\n");
}

/* U:'s directory: files, links inside and out, a program that exits with 42 */
static void
umake()
{
	static unsigned char hello[] = {
		0x60, 0x1a, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0,
		0x3f, 0x3c, 0, 42, 0x3f, 0x3c, 0, 0x4c, 0x4e, 0x41,	/* Pterm(42) */
		0, 0, 0, 0
	};
	char p[64];
	int i, fd;

	for (i = 0; utree[i]; i++) {
		sprintf(p, "%s/%s", UDIR, utree[i]);
		unlink(p);
	}
	rmdir(UDIR "/newdir");
	deep(0);
	rmdir(UDIR "/sub");
	rmdir(UDIR);
	mkdir(UDIR, 0755);
	mkdir(UDIR "/sub", 0755);
	wfile(UDIR "/readme.txt", "hello from unix\n");
	wfile(UDIR "/Long Name File.txt", "long\n");
	if ((fd = open(UDIR "/hello.prg", O_WRONLY | O_CREAT | O_TRUNC, 0755)) >= 0) {
		write(fd, (char *)hello, sizeof hello);
		close(fd);
	}
	symlink("sub", UDIR "/in");
	symlink("/sub", UDIR "/abs");
	symlink("/etc", UDIR "/out");
	symlink("../..", UDIR "/up");
	deep(1);
}

/* a copy of a file, 0 on success */
static int
fcopy(from, to)
	char *from, *to;
{
	char b[4096];
	int i, o, n = -1;

	if ((i = open(from, O_RDONLY)) < 0)
		return -1;
	if ((o = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
		while ((n = read(i, b, sizeof b)) > 0)
			if (write(o, b, n) != n) {
				n = -1;
				break;
			}
		close(o);
	}
	close(i);
	return n;
}

/* C:: AUTO\UTEST.PRG, H: in the user's drive table; G: and I: on one directory */
static void
cmake()
{
	char a[64], b[64];
	int i;

	unlink(CDIR "/AUTO/UTEST.PRG");
	for (i = 0; i < 3; i++) {
		sprintf(b, "%s/%s", CDIR, fvdifiles[i]);
		unlink(b);
	}
	rmdir(CDIR "/AUTO");
	unlink(CDIR "/drives");
	unlink(CDIR "/ctest.txt");
	rmdir(CDIR);
	mkdir(CDIR, 0755);
	mkdir(CDIR "/AUTO", 0755);
	fcopy("/tos/sys/AUTO/UTEST.PRG", CDIR "/AUTO/UTEST.PRG");
	for (i = 0; fvdi && i < 3; i++) {
		sprintf(a, "%s/%s", FVDIDIR, fvdifiles[i]);
		sprintf(b, "%s/%s", CDIR, fvdifiles[i]);
		fcopy(a, b);
	}
	wfile(CDIR "/drives", "# user drives\nH " UDIR "/sub\n");
	unlink(GDIR "/w.txt");
	unlink(GDIR "/readme.txt");
	rmdir(GDIR);
	mkdir(GDIR, 0755);
	wfile(GDIR "/readme.txt", "drive\n");
}

/* what the program in C:\AUTO found on U: */
static void
ucheck()
{
	static char *names[] = {
		"drvmap", "list", "read", "long_name", "write", "rename", "delete", "mkdir",
		"cwd", "attrib", "dfree", "pexec", "links_inside", "escape_dotdot",
		"escape_link", "escape_uplink", "escape_cwd", "long_path", "c_auto", "drive_g",
		"drive_cwd", "c_rw", "drive_d", "drive_h", 0
	};
	char b[2048], n[40], why[40], *l, *nm;
	int fd, i, ok, len = 0;

	t_check(N("hostfs_drvbits"), (peek(0x4c2L, 4) & 0x100000L) != 0, "_drvbits %lx",
	    peek(0x4c2L, 4));
	if ((fd = open(UDIR "/result.txt", O_RDONLY)) >= 0) {
		len = read(fd, b, sizeof b - 1);
		close(fd);
	}
	b[len > 0 ? len : 0] = 0;
	for (i = 0; names[i]; i++) {
		/* TOS 3.06's C: is the read-only system folder, without the user's drives */
		nm = names[i];
		if (strcmp(pf, "tos306") == 0) {
			if (strcmp(nm, "drive_d") == 0 || strcmp(nm, "drive_h") == 0)
				continue;
			if (strcmp(nm, "c_rw") == 0)
				nm = "c_ro";
		}
		sprintf(n, "PASS %s\r", nm);
		ok = strstr(b, n) != 0;
		sprintf(n, "FAIL %s ", nm);
		if ((l = strstr(b, n)) != 0)
			sscanf(l, "%*s %*s %39s", why);
		else
			strcpy(why, "no result");
		sprintf(n, "hostfs_%s", nm);
		t_check(N(n), ok, "%s", why);
	}
	len = 0;
	if ((fd = open(UDIR "/sub/renamed.txt", O_RDONLY)) >= 0) {
		len = read(fd, b, sizeof b - 1);
		close(fd);
	}
	t_check(N("hostfs_unix_side"), len == 15 && strncmp(b, "written by tos\n", 15) == 0,
	    "sub/renamed.txt: %d bytes", len);
	if (strcmp(pf, "emutos") == 0) {
		len = 0;
		if ((fd = open(CDIR "/ctest.txt", O_RDONLY)) >= 0) {
			len = read(fd, b, sizeof b - 1);
			close(fd);
		}
		t_check(N("hostfs_c_unix_side"), len == 15, "ctest.txt: %d bytes", len);
	}
}

static void
start(rom)
	char *rom;
{
	int p[2];

	umake();
	cmake();
	pipe(p);
	if ((tpid = fork()) == 0) {
		setpgrp();
		close(p[0]);
		dup2(p[1], 1);
		dup2(p[1], 2);
		if (rom && fvdi)
			execl("/tos/bin/starttos", "starttos", "-v", "-u", UDIR, "-C", CDIR,
			    "-rom", rom, (char *)0);
		else if (rom) {
			/* no ~/TOS: C: is /tos/sys, read-only */
			putenv("HOME=" GDIR);
			execl("/tos/bin/starttos", "starttos", "-v", "-u", UDIR, "-rom", rom, (char *)0);
		} else
			execl("/tos/bin/starttos", "starttos", "-v", "-u", UDIR, "-C", CDIR,
			    "-D", "I=" GDIR, (char *)0);
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

/*
 * In the background TOS sleeps and gets no input; a key held when it
 * left is released for it.
 */
static void
background()
{
	long kbs = peek(0xe00024L, 4), h0, h1, sh;
	unsigned long v0, v1, k0;

	host("down shift");
	nap(300);
	host("key ctrl+alt+meta_l+0");
	nap(1000);
	t_check(N("hotkey_console"), front() == 0, "front %ld", front());
	host("up shift");
	ioctl(tfd, TOSIOC_STAT, &st);
	v0 = st.ts_vbl;
	k0 = st.ts_kbin;
	h0 = peek(PVF(pv_head), 4);
	host("move 30 30");
	nap(1000);
	ioctl(tfd, TOSIOC_STAT, &st);
	v1 = st.ts_vbl;
	h1 = peek(PVF(pv_head), 4);
	t_check(N("background_sleeps"), v1 - v0 <= 2, "%lu VBLs in 1 s", v1 - v0);
	t_check(N("background_no_input"), h1 == h0 && st.ts_kbin == k0,
	    "events %ld -> %ld, IKBD bytes %lu -> %lu", h0, h1, k0, st.ts_kbin);
	host("key ctrl+alt+meta_l+1");
	nap(1000);
	t_check(N("hotkey_tos"), front() == sess, "front %ld, TOS %ld", front(), sess);
	sh = kbs > 0 && kbs < 0x100000 ? peek(kbs, 1) : 0;
	t_check(N("held_key_released"), sh >= 0 && !(sh & 3), "kbshift %lx", sh);
}

/* fVDI's cookie in the guest's jar: 1 when its driver owns the screen */
static int
fvdion()
{
	long jar = peek(0x5a0L, 4), v;
	int i;

	for (i = 0; jar > 0 && i < 64; i++, jar += 8)
		if ((v = peek(jar, 4)) == 0 || v == -1)
			break;
		else if (v == 0x41736846L)
			return peek(peek(jar + 4, 4) + 4, 4) == 1;
	return 0;
}

static int
diff(a, b)
	char *a, *b;
{
	char *r = cmp2(a, b);
	int n = -1;

	if (r && strcmp(r, "same") == 0)
		n = 0;
	else if (r)
		sscanf(r, "diff %d", &n);
	return n;
}

/* GEM through fVDI: a drive window opens and closes, the screen survives a switch */
static void
fvdichecks()
{
	long t0;
	int n;

	t_check(N("fvdi_on"), fvdion(), "no fVDI driver cookie");
	shot(S("f0"));
	t0 = t_now_ms();
	host("key alt+c");
	nap(3000);
	shot(S("f1"));
	n = diff(S("f0"), S("f1"));
	t_check(N("window_open"), n > 2000, "diff %d", n);
	t_info(N("window_open_ms"), "%ld (with a 3 s wait)", t_now_ms() - t0);
	if (strcmp(pf, "tos306_fvdi") == 0) {
		host("move -1000 0");
		host("move 0 -800");
		host("move 8 121");
		host("click 1");
	} else
		host("key ctrl+u");
	nap(3000);
	shot(S("f2"));
	n = diff(S("f0"), S("f2"));
	t_check(N("window_close"), n >= 0 && n < 400, "diff %d", n);
	host("key ctrl+alt+meta_l+0");
	nap(1000);
	host("key ctrl+alt+meta_l+1");
	nap(1500);
	shot(S("f3"));
	n = diff(S("f2"), S("f3"));
	t_check(N("switch_keeps_screen"), n == 0, "diff %d", n);
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

/* ms from t0 until two screen dumps in a row match, -1 if they never do */
static long
settle(t0, tag)
	long t0;
	char *tag;
{
	char a[48], b[48];
	long t = 0;
	int i;

	sprintf(a, "%s_s0", N(tag));
	shot(a);
	for (i = 1; i < 30; i++) {
		sprintf(b, "%s_s%d", N(tag), i & 1);
		shot(b);
		if (diff(a, b) == 0)
			return t;
		t = t_now_ms() - t0;
		strcpy(a, b);
	}
	return -1;
}

/* drive C:'s window: open, drag by the title bar, close; time to a still screen and CPU */
static void
perf(tx, ty, cdx)
	int tx, ty;		/* a point on the window's title bar */
	int cdx;		/* its close box from there, 0: close with ^U */
{
	char req[40];
	long t0, s, g0, g1, d0, d1;

	cputimes(&g0, &d0);
	t0 = t_now_ms();
	host("key alt+c");
	s = settle(t0, "p_open");
	cputimes(&g1, &d1);
	t_info(N("perf_open"), "%ld ms, CPU ms guest %ld display %ld", s, g1 - g0, d1 - d0);
	host("move -1000 0");
	host("move 0 -800");
	sprintf(req, "move %d %d", tx, ty);
	host(req);
	host("button 1");
	cputimes(&g0, &d0);
	t0 = t_now_ms();
	host("glide 60 2 2 20");
	nap(1500);
	host("button 0");
	s = settle(t0, "p_drag");
	cputimes(&g1, &d1);
	t_info(N("perf_drag"), "%ld ms, CPU ms guest %ld display %ld", s, g1 - g0, d1 - d0);
	if (cdx) {
		sprintf(req, "move %d 0", cdx);
		host(req);
	}
	cputimes(&g0, &d0);
	t0 = t_now_ms();
	host(cdx ? "click 1" : "key ctrl+u");
	s = settle(t0, "p_close");
	cputimes(&g1, &d1);
	t_info(N("perf_close"), "%ld ms, CPU ms guest %ld display %ld", s, g1 - g0, d1 - d0);
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

/*
 * A left click on empty desktop while the guest is stopped and the IKBD
 * FIFO full: it still arrives.  Left, because TOS 3.06 keeps a right
 * click nobody asked for and lets it eat the next left click.
 */
static void
fullfifo()
{
	struct tosinput in;
	unsigned long r0;
	int i;

	memset((char *)&in, 0, sizeof in);
	host("move -400 0");
	host("move 0 -300");
	host("move 300 250");
	nap(500);
	ioctl(tfd, TOSIOC_STAT, &st);
	r0 = st.ts_kbrd;
	kill(tpid, SIGSTOP);
	nap(300);
	for (i = 0; i < sizeof in.ti_b; i++)
		in.ti_b[i] = 0xb9;		/* Space released */
	in.ti_n = sizeof in.ti_b;
	for (i = 0; i < 5; i++)
		ioctl(tfd, TOSIOC_INPUT, &in);
	in.ti_n = 0;
	in.ti_btn = 2;
	ioctl(tfd, TOSIOC_INPUT, &in);
	in.ti_btn = 0;
	ioctl(tfd, TOSIOC_INPUT, &in);
	kill(tpid, SIGCONT);
	nap(2000);
	ioctl(tfd, TOSIOC_STAT, &st);
	t_check(N("click_full_fifo"), st.ts_kbrd - r0 == 256 + 6,
	    "%lu IKBD bytes read, 262 expected", st.ts_kbrd - r0);
}

/* kbshift: bit 1 is the left Shift key */
static void
keys()
{
	long kbs = peek(0xe00024L, 4), a, b, c, k0 = peek(PVF(pv_nkey), 4);
	unsigned long n0;

	if (kbs <= 0 || kbs >= 0x100000) {
		t_skip(N("kbshift"), "no kbshift address in the ROM header");
		return;
	}
	ioctl(tfd, TOSIOC_STAT, &st);
	n0 = st.ts_kbrd;
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
	if (k0 >= 0 && (peek(PVF(pv_on), 4) & PV_KEYS))
		t_check(N("keys_direct"), peek(PVF(pv_nkey), 4) >= k0 + 2 && st.ts_kbrd == n0,
		    "keys to kbdvec %ld -> %ld, IKBD bytes %lu -> %lu", k0, peek(PVF(pv_nkey), 4),
		    n0, st.ts_kbrd);
	else
		t_check(N("ikbd_read"), st.ts_kbrd >= n0 + 2, "IKBD bytes read %lu -> %lu", n0,
		    st.ts_kbrd);
}

/* on: $118 to a stub chaining to TOS's handler, so input takes the IKBD */
static int
ikbdonly(on)
	int on;
{
	static long v;
	int ok;

	if (on) {
		v = peek(0x118L, 4);
		ok = poke(0x3f0L, 0x4ef90000L | (v >> 16 & 0xffff)) &&
		    poke(0x3f4L, v << 16) && poke(0x118L, 0x3f0L);
	} else
		ok = poke(0x118L, v);
	nap(300);
	return ok;
}

/*
 * Input straight to TOS's handlers: motion reaches mousevec with no
 * IKBD byte.  With the ACIA vector taken (a stub that chains to TOS)
 * the IKBD carries it, and the direct path returns with the vector.
 */
static void
direct()
{
	long on = peek(PVF(pv_on), 4), p0 = peek(PVF(pv_npkt), 4), p1, on1, vs;
	unsigned long b0;

	ioctl(tfd, TOSIOC_STAT, &st);
	t_info(N("pv_on"), "%ld head %ld tail %ld xy %lx cxy %lx btn %ld kbin %lu", on,
	    peek(PVF(pv_head), 4), peek(PVF(pv_tail), 4), peek(PVF(pv_xy), 4),
	    peek(PVF(pv_cxy), 4), peek(PVF(pv_btn), 4), st.ts_kbin);
	if (!t_check(N("mouse_direct_on"), on >= 0 && (on & PV_MOUSE), "pv_on %ld", on))
		return;
	ioctl(tfd, TOSIOC_STAT, &st);
	b0 = st.ts_kbrd;
	host("move 20 10");
	nap(500);
	ioctl(tfd, TOSIOC_STAT, &st);
	p1 = peek(PVF(pv_npkt), 4);
	t_check(N("mouse_direct"), p1 > p0 && st.ts_kbrd == b0,
	    "mousevec packets %ld -> %ld, IKBD bytes %lu -> %lu", p0, p1, b0, st.ts_kbrd);
	if (!ikbdonly(1)) {
		t_check(N("fallback"), 0, "guest memory not writable: %s", T_ERR);
		return;
	}
	nap(300);
	on1 = peek(PVF(pv_on), 4);
	ioctl(tfd, TOSIOC_STAT, &st);
	b0 = st.ts_kbrd;
	p0 = peek(PVF(pv_npkt), 4);
	host("move -20 -10");
	nap(500);
	ioctl(tfd, TOSIOC_STAT, &st);
	p1 = peek(PVF(pv_npkt), 4);
	t_check(N("fallback"), on1 == 0 && st.ts_kbrd >= b0 + 3 && p1 == p0,
	    "pv_on %ld, IKBD bytes %lu -> %lu, mousevec packets %ld -> %ld", on1, b0,
	    st.ts_kbrd, p0, p1);
	ikbdonly(0);
	t_check(N("fallback_back"), peek(PVF(pv_on), 4) == on, "pv_on %ld", peek(PVF(pv_on), 4));
	/* with the VBL queue off the IKBD carries the motion */
	vs = peek(0x452L, 4);
	if (vs < 0 || !poke(0x452L, vs & 0xffff))
		return;
	nap(500);
	ioctl(tfd, TOSIOC_STAT, &st);
	b0 = st.ts_kbrd;
	host("move 20 10");
	nap(500);
	ioctl(tfd, TOSIOC_STAT, &st);
	poke(0x452L, (vs & 0xffff0000L) | (peek(0x454L, 2) & 0xffff));
	t_check(N("novbl"), st.ts_kbrd >= b0 + 3, "IKBD bytes %lu -> %lu", b0, st.ts_kbrd);
	nap(300);
	p0 = peek(PVF(pv_npkt), 4);
	host("move -20 -10");
	nap(500);
	p1 = peek(PVF(pv_npkt), 4);
	t_check(N("novbl_back"), p1 > p0, "mousevec packets %ld -> %ld", p0, p1);
}

/* guest memory [a, a+n) through /proc; 0 if unreadable */
static int
grab(b, a, n)
	char *b;
	long a, n;
{
	char path[32];
	int fd, ok;

	sprintf(path, "/proc/%05ld", (long)tpid);
	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	ok = lseek(fd, a, 0) == a && read(fd, b, n) == n;
	close(fd);
	return ok;
}

#define	W(b, i)	(((b)[i] & 0xff) << 8 | ((b)[(i) + 1] & 0xff))
#define	GLO	0x800L
#define	GLEN	0x80000L

/* the word pair holding the pointer's x and y: x grows on a move right, y on one down */
static long
ptrvar()
{
	char *a, *b, *c;
	long i, r = 0;

	a = malloc(GLEN);
	b = malloc(GLEN);
	c = malloc(GLEN);
	host("move -400 0");
	host("move 0 -300");
	host("move 40 40");
	nap(500);
	if (a && b && c && grab(a, GLO, GLEN)) {
		host("move 20 0");
		nap(500);
		if (grab(b, GLO, GLEN)) {
			host("move 0 20");
			nap(500);
			if (grab(c, GLO, GLEN))
				for (i = 0; i + 4 <= GLEN && !r; i += 2)
					if (W(b, i) > W(a, i) && W(b, i) < 1280 && W(c, i) == W(b, i) &&
					    W(a, i + 2) == W(b, i + 2) && W(c, i + 2) > W(b, i + 2) &&
					    W(c, i + 2) < 1024)
						r = GLO + i;
		}
	}
	free(a);
	free(b);
	free(c);
	return r;
}

static long pvar;		/* the pointer's x, y words */

/* continuous motion: pointer updates, settling time, kernel entries */
static void
glide(name)
	char *name;
{
	long a, t0, t, tl = 0, p, lp, p500 = -1, g0, g1, d0, d1;
	unsigned long e0, e1, kb0, ki0, n = 0;

	if ((a = pvar = ptrvar()) == 0) {
		t_info(N(name), "pointer variable not found");
		return;
	}
	lp = peek(a, 4);
	cputimes(&g0, &d0);
	e0 = entries();
	kb0 = st.ts_kbrd;
	ki0 = st.ts_ints[6];
	host("glide 100 3 2 10");
	t0 = t_now_ms();
	while ((t = t_now_ms() - t0) < 3000) {
		p = peek(a, 4);
		if (p != lp) {
			n++;
			tl = t;
			lp = p;
		}
		if (p500 < 0 && t >= 500)
			p500 = p;
		nap(1);
	}
	p = peek(a, 4);
	e1 = entries();
	cputimes(&g1, &d1);
	t_info(N(name), "var %lx: %lu updates, last at %ld ms, at 500 ms (%ld,%ld) end (%ld,%ld);"
	    " 3 s: entries %lu, acia ints %lu, bytes %lu, CPU ms guest %ld display %ld", a, n, tl,
	    p500 >> 16, p500 & 0xffff, p >> 16, p & 0xffff, e1 - e0, st.ts_ints[6] - ki0,
	    st.ts_kbrd - kb0, g1 - g0, d1 - d0);
}

/* ms from a host move to a new pointer position, 10 moves */
static void
latency(name)
	char *name;
{
	long t0, d, sum = 0, max = 0, p;
	int i, n = 0;

	if (pvar == 0)
		return;
	for (i = 0; i < 10; i++) {
		p = peek(pvar, 4);
		t0 = t_now_ms();
		host(i & 1 ? "move -6 -4" : "move 6 4");
		while ((d = t_now_ms() - t0) < 1000 && peek(pvar, 4) == p)
			nap(1);
		if (d < 1000) {
			n++;
			sum += d;
			max = d > max ? d : max;
		}
		nap(100);
	}
	t_info(N(name), "%d of 10 moves seen, mean %ld ms, max %ld ms (host request included)",
	    n, n ? sum / n : 0, max);
}

/*
 * Each button pressed and released: the line-A button word next to the
 * pointer (x, y, hide count, buttons) reads 1 for left, 2 for right,
 * then 0.  Last, because TOS 3.06 keeps an unclaimed right click.
 */
static void
buttons()
{
	struct tosinput in;
	long down[2], up[2];
	int i;

	if (pvar == 0) {
		t_skip(N("buttons"), "pointer variable not found");
		return;
	}
	host("move -400 0");
	host("move 0 -300");
	host("move 300 250");
	nap(500);
	memset((char *)&in, 0, sizeof in);
	for (i = 0; i < 2; i++) {
		in.ti_btn = i ? 1 : 2;		/* IKBD bits: 2 left, 1 right */
		ioctl(tfd, TOSIOC_INPUT, &in);
		nap(400);
		down[i] = peek(pvar + 6, 2);
		in.ti_btn = 0;
		ioctl(tfd, TOSIOC_INPUT, &in);
		nap(400);
		up[i] = peek(pvar + 6, 2);
	}
	t_check(N("buttons"), down[0] == 1 && up[0] == 0 && down[1] == 2 && up[1] == 0,
	    "left %ld/%ld, right %ld/%ld (down/up)", down[0], up[0], down[1], up[1]);
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
			background();
		}
		if (fvdi)
			fvdichecks();
		else {
			owner();
			ucheck();
			timers();
		}
		idle();
		ioctl(tfd, TOSIOC_STAT, &st);
		t_check(N("cache_ops"), st.ts_cache > 0, "%lu cache instructions and CACR writes",
		    st.ts_cache);
		if (hfd >= 0) {
			keys();
			direct();
			fullfifo();
			if (fvdi)
				pointer(35, rom ? 88 : 40);
			else
				pointer(35, rom ? 77 : 38);
			glide("glide");
			latency("latency");
			if (ikbdonly(1)) {
				glide("glide_ikbd");
				latency("latency_ikbd");
				ikbdonly(0);
			}
			buttons();
			(void)desktop(S("end"), 1);
			if (fvdi)
				perf(300, rom ? 121 : 110, rom ? -292 : 0);
			else
				perf(220, rom ? 60 : 106, rom ? -210 : 0);
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
	fvdi = 1;
	if (stat(FVDIDIR "/FVDI.SYS", &sb) < 0 || hfd < 0)
		t_skip("fvdi", "no fVDI build or no host line");
	else {
		pf = "emutos_fvdi";
		if (stat(EMUTOS, &sb) == 0)
			run((char *)0);
		pf = "tos306_fvdi";
		if (stat(USERROM, &sb) == 0)
			run(USERROM);
	}
	return t_done();
}
