/*
 * t_xaaes.c -- XaAES in the TOS environment, over the MiNT layer.
 *
 * Makes a C: folder with fVDI, XAMINT.PRG, XaAES, TeraDesk, the session
 * accessory and XATICK.PRG, runs starttos on it and checks: the layer
 * starts XaAES (its log), XaAES draws its desktop under a menu bar (host
 * screen dumps), TeraDesk runs as its shell beside the accessory and
 * XATICK.PRG (the layer's process log), the pointer pulls TeraDesk's Desk
 * menu down, a double click opens drive C:'s window, XATICK.PRG keeps
 * counting meanwhile and, topped, gets a key typed on the host, and it
 * catches a signal; the guest's CPU use while all wait.  XAMISC.PRG's
 * Pfork, Pvfork, Fpoll and socket results; XABUSY.PRG spinning without
 * system calls while XATICK.PRG counts on, and its alarm; a TOS program
 * and a MiNT one (a Unix process) started from XATICK.PRG, each in a
 * console window taking typed lines.  Without the host line only the
 * checks needing no screen, pointer or keys run.  The layer's and
 * XaAES's logs go to the console.  Skips without guest support, the module, EmuTOS, fVDI
 * or the XaAES build.
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
#include <dirent.h>
#include <termio.h>
#include <sys/procfs.h>
#include "sys/mod.h"
#include "dsio.h"
#include "tosio.h"
#include "t.h"

#define	EMUTOS	"/etc/tos/emutos.img"
#define	XADIR	"/tos/xaaes"
#define	FVDIDIR	"/tos/fvdi"
#define	TICKPRG	"/tos/xatick/XATICK.PRG"
#define	ENVF	"id=emutos\nname=emutos\nbase=emutos\ntemplate=/tos/xaaes\ndesktop=xaaes\n"
#define	RUNTICK	"run c:\\xatick.prg\r\nrun c:\\xabusy.prg\r\nrun c:\\xamisc.prg\r\n"
#define	XTDIR	"/tos/xatick"
#define	XHOME	"/tmp/xah"
#define	CDIR	XHOME "/TOS"
#define	UDIR	"/tmp/xau"
#define	LOG	CDIR "/MINT/XAMINT.LOG"
#define	XALOG	CDIR "/MINT/XAAES.LOG"
#define	TICKS	CDIR "/XATICK.TXT"	/* XATICK.PRG's count */
#define	KEYS	CDIR "/XAKEY.TXT"	/* the last key it got */
#define	SIGS	CDIR "/XASIG.TXT"	/* the signal it caught */
#define	BUSY	CDIR "/XABUSY.TXT"	/* 1: XABUSY.PRG spins */
#define	ALRM	CDIR "/XAALRM.TXT"	/* its alarm's signal */
#define	MISC	CDIR "/XAMISC.TXT"
#define	HELLO	CDIR "/XAHELLO.TXT"	/* the line XAHELLO.TOS read */
#define	MPID	CDIR "/XAMINTP.PID"	/* XAMINTP.TTP's pid */
#define	MLINE	CDIR "/XAMINTP.TXT"	/* the line it read */

/* the test root's files, else a disk image's TOS environment */
static char *md = "/tests/aux/mod.d", *starttos = "/tos/bin/starttos";
static char *tddir = "/tos/teradesk/TERADESK", *accprg = "/tos/acc/SESSION.ACC";

static int hfd = -1, hseq, tfd = -1, ofd = -1, outn;
static char hrep[128], out[4096];
static pid_t tpid;
static long logoff, xalogoff;

static void
nap(ms)
	int ms;
{
	poll((struct pollfd *)0, 0L, ms);
}

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

static int
shot(name)
	char *name;
{
	char req[64];

	sprintf(req, "shot xaaes_%s", name);
	return host(req) != 0;
}

/*
 * the dump's look: 1 when a desktop of one colour fills the screen
 * under a menu bar of another
 */
static int
desktop(name, info)
	char *name;
	int info;
{
	char req[64], *r, dc[8], mc[8];
	int x, y, w, h, menu = 0, desk = 0;

	if (!shot(name))
		return 0;
	sprintf(req, "tos xaaes_%s", name);
	if ((r = host(req)) == 0)
		return 0;
	dc[0] = 0;
	sscanf(r, "box %d %d %d %d menu %d desk %d %7s", &x, &y, &w, &h, &menu, &desk, dc);
	if (info)
		t_info("screen", "%s", r);
	sprintf(req, "pix xaaes_%s 400 8", name);
	if ((r = host(req)) == 0)
		return 0;
	strncpy(mc, r, sizeof mc - 1);
	mc[sizeof mc - 1] = 0;
	if (info)
		t_info("menubar", "%s", mc);
	return w >= 600 && h >= 380 && desk >= 500 && strcmp(mc, dc) != 0 && strcmp(mc, "000000") != 0;
}

static int
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

/* new lines of a log file to the console */
static void
tail(path, off, pfx)
	char *path, *pfx;
	long *off;
{
	char b[1024], *s, *nl;
	int fd, n;

	if ((fd = open(path, O_RDONLY)) < 0)
		return;
	lseek(fd, *off, 0);
	while ((n = read(fd, b, sizeof b - 1)) > 0) {
		b[n] = 0;
		for (s = b; (nl = strchr(s, '\n')) != 0; s = nl + 1) {
			*nl = 0;
			if (nl > s && nl[-1] == '\r')
				nl[-1] = 0;
			printf("%s| %s\n", pfx, s);
		}
		*off += s - b;
		if (s == b && n == sizeof b - 1)
			*off += n;
		lseek(fd, *off, 0);
	}
	close(fd);
	fflush(stdout);
}

static void
relay()
{
	char *nl;
	int n;

	tail(LOG, &logoff, "xamint");
	tail(XALOG, &xalogoff, "xaaes");
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

static int
fcopy(from, to)
	char *from, *to;
{
	char b[4096];
	int i, o, n;

	if ((i = open(from, O_RDONLY)) < 0)
		return -1;
	if ((o = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
		close(i);
		return -1;
	}
	while ((n = read(i, b, sizeof b)) > 0)
		write(o, b, n);
	close(i);
	close(o);
	return 0;
}

/* a directory tree: copied (to) or removed (to 0) */
static void
tree(from, to)
	char *from, *to;
{
	char a[256], b[256];
	struct dirent *de;
	struct stat sb;
	DIR *d;

	if (to)
		mkdir(to, 0755);
	if ((d = opendir(from)) == 0)
		return;
	while ((de = readdir(d)) != 0) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		sprintf(a, "%s/%s", from, de->d_name);
		if (to)
			sprintf(b, "%s/%s", to, de->d_name);
		if (lstat(a, &sb) == 0 && (sb.st_mode & S_IFMT) == S_IFDIR)
			tree(a, to ? b : (char *)0);
		else if (to)
			fcopy(a, b);
		else
			unlink(a);
	}
	closedir(d);
	if (!to)
		rmdir(from);
}

static void
cmake()
{
	int fd;

	tree(CDIR, (char *)0);
	tree(UDIR, (char *)0);
	mkdir(XHOME, 0755);
	tree(XADIR, CDIR);
	tree(FVDIDIR, CDIR);
	tree(tddir, CDIR "/TERADESK");
	fcopy(accprg, CDIR "/SESSION.ACC");
	fcopy(TICKPRG, CDIR "/XATICK.PRG");
	fcopy(XTDIR "/XABUSY.PRG", CDIR "/XABUSY.PRG");
	fcopy(XTDIR "/XAMISC.PRG", CDIR "/XAMISC.PRG");
	fcopy(XTDIR "/XAMINTP.TTP", CDIR "/XAMINTP.TTP");
	fcopy(XTDIR "/XAHELLO.TOS", CDIR "/XAHELLO.TOS");
	if ((fd = open(CDIR "/MINT/XAAES/XAAES.CNF", O_WRONLY | O_APPEND)) >= 0) {
		write(fd, RUNTICK, strlen(RUNTICK));
		close(fd);
	}
	mkdir(CDIR "/CLIPBRD", 0755);
	mkdir(UDIR, 0755);
	/* the environment's desktop setting starts XaAES */
	if ((fd = creat(CDIR "/.env", 0644)) >= 0) {
		write(fd, ENVF, strlen(ENVF));
		close(fd);
	}
}

static void
start()
{
	int p[2];

	cmake();
	pipe(p);
	if ((tpid = fork()) == 0) {
		setpgrp();
		close(p[0]);
		dup2(p[1], 1);
		dup2(p[1], 2);
		putenv("HOME=" XHOME);
		execl(starttos, "starttos", "-rom", EMUTOS, "-v", "-u", UDIR, "-e", "emutos", (char *)0);
		_exit(127);
	}
	close(p[1]);
	ofd = p[0];
	fcntl(ofd, F_SETFL, O_NDELAY);
}

/* the log holds s (under 64 characters) */
static int
logged(path, s)
	char *path, *s;
{
	char b[4096 + 64];
	int fd, n, keep = 0, sl = strlen(s), hit = 0;

	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	while (!hit && (n = read(fd, b + keep, 4096)) > 0) {
		n += keep;
		b[n] = 0;
		hit = strstr(b, s) != 0;
		keep = n < sl ? n : sl;
		memmove(b, b + n - keep, keep);
	}
	close(fd);
	return hit;
}

/* a process the layer started and saw no exit of */
static int
alive(name)
	char *name;
{
	char b[512], s[64], *p;
	FILE *f = fopen(LOG, "r");
	int pid = -1;

	if (!f)
		return 0;
	sprintf(s, "start %s pid ", name);
	while (fgets(b, sizeof b, f))
		if ((p = strstr(b, s)) != 0)
			pid = atoi(p + strlen(s));
		else if (pid > 0 && (p = strstr(b, "exit pid ")) != 0 && atoi(p + 9) == pid)
			pid = -1;
	fclose(f);
	return pid > 0;
}

/* CPU time of starttos, the guest, in ms */
static long
cputime()
{
	DIR *dp = opendir("/proc");
	struct dirent *e;
	prpsinfo_t ps;
	char path[32];
	long t = 0;
	int fd;

	while (dp && (e = readdir(dp)) != 0) {
		if (e->d_name[0] == '.')
			continue;
		sprintf(path, "/proc/%s", e->d_name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		if (ioctl(fd, PIOCPSINFO, &ps) == 0 && ps.pr_pid == tpid)
			t = ps.pr_time.tv_sec * 1000L + ps.pr_time.tv_nsec / 1000000;
		close(fd);
	}
	if (dp)
		closedir(dp);
	return t;
}

/* a number XATICK.PRG wrote, -1 for none */
static long
num(path)
	char *path;
{
	char b[32];
	int fd = open(path, O_RDONLY), n = 0;

	if (fd >= 0) {
		n = read(fd, b, sizeof b - 1);
		close(fd);
	}
	b[n > 0 ? n : 0] = 0;
	return n > 0 ? atol(b) : -1;
}

/* the number after word in XAMISC.TXT; -99 for none */
static long
misc(word)
	char *word;
{
	char b[256], *p;
	int fd = open(MISC, O_RDONLY), n = 0;

	if (fd >= 0) {
		n = read(fd, b, sizeof b - 1);
		close(fd);
	}
	b[n > 0 ? n : 0] = 0;
	for (p = b; (p = strstr(p, word)) != 0; p++)
		if ((p == b || p[-1] == '\n') && p[strlen(word)] == ' ')
			return atol(p + strlen(word) + 1);
	return -99;
}

/* the file's first line is s */
static int
holds(path, s)
	char *path, *s;
{
	char b[64];
	int fd = open(path, O_RDONLY), n = 0;

	if (fd >= 0) {
		n = read(fd, b, sizeof b - 1);
		close(fd);
	}
	b[n > 0 ? n : 0] = 0;
	b[strcspn(b, "\r\n")] = 0;
	return strcmp(b, s) == 0;
}

/* the command name of host process pid */
static char *
pname(pid)
	long pid;
{
	static prpsinfo_t ps;
	char path[32];
	int fd;

	sprintf(path, "/proc/%05ld", pid);
	ps.pr_fname[0] = 0;
	if ((fd = open(path, O_RDONLY)) >= 0) {
		if (ioctl(fd, PIOCPSINFO, &ps) < 0)
			ps.pr_fname[0] = 0;
		close(fd);
	}
	return ps.pr_fname;
}

/* host keys, one at a time */
static void
type(s)
	char *s;
{
	char req[16];

	for (; *s; s++) {
		if (*s == '\r')
			strcpy(req, "key ret");
		else
			sprintf(req, "key %c", *s);
		host(req);
	}
}

/*
 * pointer to x, y from the top left, by way of the bottom left corner:
 * crossing the menu bar would pull its menus down
 */
static void
ptrto(x, y)
	int x, y;
{
	char req[40];

	host("move -900 700 20");
	sprintf(req, "move %d %d 20", x, y - 599);
	host(req);
	nap(500);
}

/* 1 when dumps a and b differ at x, y */
static int
differ(a, b, x, y)
	char *a, *b;
	int x, y;
{
	char req[64], pa[8], *r;

	sprintf(req, "pix xaaes_%s %d %d", a, x, y);
	if ((r = host(req)) == 0)
		return 0;
	strncpy(pa, r, sizeof pa - 1);
	pa[sizeof pa - 1] = 0;
	sprintf(req, "pix xaaes_%s %d %d", b, x, y);
	if ((r = host(req)) == 0)
		return 0;
	return strcmp(pa, r) != 0;
}

static void
run()
{
	long t0, t1, n0, n1, c0;
	int i, up = 0, status;
	struct stat sb;

	start();
	t0 = t_now_ms();
	while (t_now_ms() - t0 < 10000 && front() <= 0) {
		relay();
		nap(200);
	}
	t_check("session", front() > 0, "no display session");
	for (i = 0; i < 60 && !up; i++) {
		nap(2000);
		relay();
		if (logged(LOG, "AES dispatcher"))
			up = hfd < 0 || desktop("desk", 0);
	}
	relay();
	t_check("layer_started", logged(LOG, "XaMiNT: root"), "no layer log");
	t_check("xaaes_init", logged(LOG, "AES dispatcher"), "XaAES did not register its trap");
	if (hfd >= 0)
		t_check("desktop", up && desktop("desk", 1), "no XaAES desktop after %ld s",
		    (t_now_ms() - t0) / 1000);
	for (i = 0; i < 30 && (!alive("DESKTOP") || num(TICKS) < 4); i++) {
		nap(1000);
		relay();
	}
	t_check("teradesk", alive("DESKTOP"), "TeraDesk not running");
	t_check("accessory", alive("SESSION"), "SESSION.ACC not running");
	t_check("ticker", alive("XATICK") && num(TICKS) > 0, "XATICK.PRG not running, count %ld",
	    num(TICKS));
	t_check("signal", num(SIGS) == 29, "XATICK.PRG's SIGUSR1 handler: %ld", num(SIGS));
	for (i = 0; i < 20 && misc("sock") == -99; i++) {
		nap(1000);
		relay();
	}
	t_check("pfork", misc("fork") == 43, "XAMISC.PRG fork: %ld", misc("fork"));
	t_check("pvfork", misc("vfork") == 7, "XAMISC.PRG vfork: %ld", misc("vfork"));
	t_check("fpoll", misc("poll") == 2, "XAMISC.PRG poll: %ld", misc("poll"));
	/* without TCP/IP set up, connect finds no address after bind and listen */
	if (stat("/etc/inet/strcf", &sb) < 0 && misc("sock") == -3)
		t_skip("sockets", "no network: socket, bind and listen only");
	else
		t_check("sockets", misc("sock") == 4, "XAMISC.PRG sock: %ld", misc("sock"));
	c0 = cputime();
	t1 = t_now_ms();
	nap(5000);
	c0 = (cputime() - c0) * 100 / (t_now_ms() - t1);
	t_info("cpu", "%ld%% while XaAES, TeraDesk and XATICK.PRG wait", c0);
	t_check("cpu_quiet", c0 < 50, "the guest took %ld%% of the CPU", c0);
	/* XABUSY.PRG spins: only preemption keeps the others going */
	close(creat(CDIR "/XABUSY.GO", 0644));
	for (i = 0; i < 20 && num(BUSY) != 1; i++) {
		nap(500);
		relay();
	}
	t_check("busy", num(BUSY) == 1, "XABUSY.PRG not spinning: %ld", num(BUSY));
	n0 = num(TICKS);
	nap(4000);
	relay();
	n1 = num(TICKS);
	t_check("preempt", n0 > 0 && n1 >= n0 + 8, "XATICK.PRG counted %ld to %ld beside XABUSY.PRG",
	    n0, n1);
	t_check("talarm", num(ALRM) == 14, "XABUSY.PRG's alarm: %ld", num(ALRM));
	if (hfd < 0) {
		t_skip("screen", "no host line: no screen, pointer or key checks");
		goto out;
	}
	n0 = num(TICKS);
	desktop("td", 1);
	/* Desk menu: TeraDesk's, with the accessory under it, pulled down by the pointer */
	ptrto(35, 8);
	nap(1500);
	shot("deskmenu");
	t_check("desk_menu", differ("td", "deskmenu", 80, 60) && differ("td", "deskmenu", 80, 110),
	    "no Desk menu at 80,60-110");
	ptrto(600, 560);
	host("click 1");
	nap(1500);
	shot("td2");
	/* drive C:'s window, opened from its icon */
	ptrto(195, 40);
	host("click 2");
	nap(3500);
	relay();
	shot("drive");
	t_check("drive_window", differ("td2", "drive", 300, 200) && differ("td2", "drive", 400, 300),
	    "no drive window at 300,200-400,300");
	nap(3000);
	relay();
	n1 = num(TICKS);
	t_check("multitask", n0 > 0 && n1 >= n0 + 8, "XATICK.PRG counted %ld to %ld meanwhile",
	    n0, n1);
	/* XATICK.PRG topped by its title bar takes the keyboard */
	ptrto(680, 392);
	host("click 1");
	nap(1500);
	host("key a");
	nap(2000);
	t_check("keyboard", num(KEYS) == 'a', "XATICK.PRG got key %ld", num(KEYS));
	/* a TOS program in a console window */
	shot("precon");
	host("key t");
	nap(4000);
	relay();
	shot("con");
	t_check("console_window", differ("precon", "con", 100, 420) && differ("precon", "con", 600, 430),
	    "no console window at 100,420-600,430");
	type("ok\r");
	nap(2000);
	relay();
	t_check("tos_console", holds(HELLO, "ok") && logged(LOG, "(XAHELLO) code 5"),
	    "XAHELLO.TOS read no 'ok', did not end or got a small TPA");
	/* a MiNT program, a Unix process, in another */
	ptrto(690, 392);
	host("click 1");
	nap(1500);
	host("key m");
	for (i = 0; i < 20 && num(MPID) <= 0; i++) {
		nap(500);
		relay();
	}
	t_check("mint_unix", num(MPID) > 0 && strcmp(pname(num(MPID)), "mintrun") == 0,
	    "XAMINTP.TTP: pid %ld, '%s'", num(MPID), pname(num(MPID)));
	type("hi\r");
	nap(3000);
	relay();
	t_check("mint_console", holds(MLINE, "hi") && logged(LOG, "status 300"),
	    "XAMINTP.TTP read no 'hi' or did not end with 3");
	shot("cons");
	t_check("still_running", alive("DESKTOP") && alive("XATICK") && alive("SESSION"),
	    "a GEM program ended");
	shot("end");
out:
	relay();
	kill(tpid, SIGTERM);
	t0 = t_now_ms();
	while (waitpid(tpid, &status, WNOHANG) != tpid && t_now_ms() - t0 < 5000)
		nap(100);
	t_check("ended", t_now_ms() - t0 < 5000, "starttos still running");
	relay();
	close(ofd);
	ofd = -1;
}

main()
{
	struct mod_mreg reg;
	struct stat sb;
	int mj = TOS_MAJOR;

	t_init("xaaes", 500);
	if (t_kmem("guest_loading") == -1) {
		t_skip("all", "kernel has no guest support");
		return t_done();
	}
	if (stat("/tos/bin/starttos", &sb) < 0) {
		md = "/usr/aux/lib/mod.d";
		starttos = "/usr/bin/starttos";
		tddir = "/tos/sys/TERADESK";
		accprg = "/tos/sys/SESSION.ACC";
	}
	if (stat(md, &sb) < 0 || stat("/dev/tos", &sb) < 0 ||
	    stat(starttos, &sb) < 0 || stat(EMUTOS, &sb) < 0) {
		t_skip("all", "no tosguest module, starttos or EmuTOS on this root");
		return t_done();
	}
	if (stat(XADIR "/AUTO/XAMINT.PRG", &sb) < 0 || stat(FVDIDIR "/FVDI.SYS", &sb) < 0) {
		t_skip("all", "no XaAES or fVDI build on this root");
		return t_done();
	}
	modpath(md);
	strcpy(reg.md_modname, "tosguest");
	reg.md_typedata = (caddr_t)&mj;
	/* a disk image registers its modules at boot */
	if ((tfd = open("/dev/tos", O_RDWR)) < 0 &&
	    !t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "modadm: %s", T_ERR))
		return t_done();
	if (!t_check("open", tfd >= 0 || (tfd = open("/dev/tos", O_RDWR)) >= 0, "/dev/tos: %s", T_ERR))
		return t_done();
	hostopen();
	run();
	return t_done();
}
