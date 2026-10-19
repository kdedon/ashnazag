/*
 * t_lisa: Lisa OS from the user's ProFile image on SCSI disk 6, loaded by
 * startlisa: the volume is read, the screen shows text, nothing faulted, and
 * in steady state the guest takes no faults on hardware pages.
 * Skips without the image.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "sys/mod.h"
#include <sys/wait.h>
#include <sys/termio.h>
#include <poll.h>
#include <stdlib.h>
#include <signal.h>
#include "t.h"

extern int modload();

#define	MD	"/tests/aux/mod.d"
#define	DEV	"/dev/dsk/c6d0s0"
#define	MNT	"/lisa"
#define	LOG	"/tmp/lisa.log"
#define	ROOT	"/tmp/lisaenv"		/* the environment's folder */
#define	RUNS	240

static int hfd = -1;
static int hseq;
static char hrep[128];

/* a host request (tests/display/hostio.py); the answer, 0 if none in 20 s */
static char *
host(req)
	char *req;
{
	char buf[160], line[256], *s;
	struct pollfd p;
	int n, len = 0, seq;
	long t0;

	if (hfd < 0)
		return 0;
	sprintf(buf, "@@ %d %s\n", ++hseq, req);
	write(hfd, buf, strlen(buf));
	t0 = t_now_ms();
	p.fd = hfd;
	p.events = POLLIN;
	while (t_now_ms() - t0 < 20000) {
		if (poll(&p, 1, 500) <= 0 || (n = read(hfd, line + len, sizeof line - 1 - len)) <= 0)
			continue;
		len += n;
		line[len] = 0;
		if ((s = strstr(line, "@@ok ")) != 0 && strchr(s, '\n') &&
		    sscanf(s + 5, "%d", &seq) == 1 && seq == hseq) {
			s = strchr(s + 5, ' ');
			strncpy(hrep, s ? s + 1 : "", sizeof hrep - 1);
			hrep[strcspn(hrep, "\r\n")] = 0;
			return hrep;
		}
		if (strchr(line, '\n') && strstr(line, "@@err "))
			return 0;
		if (strchr(line, '\n'))
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
		t_info("host", "no answer on /dev/term/b");
		close(hfd);
		hfd = -1;
	}
}

static char *
readlog()
{
	static char buf[65536];
	int fd, n = 0;

	if ((fd = open(LOG, O_RDONLY)) >= 0) {
		n = read(fd, buf, sizeof buf - 1);
		if (n == sizeof buf - 1 && lseek(fd, -(long)(sizeof buf / 2), 2) >= 0)
			n = read(fd, buf, sizeof buf / 2);	/* the tail */
		close(fd);
	}
	buf[n > 0 ? n : 0] = 0;
	return buf;
}

/* a screen dump; with the Lisa's screen up, most of it is lit */
static void
shot(name, drawn)
	char *name;
	int drawn;
{
	char req[64];
	long lit;

	sprintf(req, "shot %s", name);
	if (host(req) == 0)
		return;
	sprintf(req, "lit %s", name);
	if (host(req) == 0 || !drawn)
		return;
	lit = atol(hrep);
	t_check(name, lit > 20000, "%ld pixels lit", lit);
}

/*
 * New log lines to the console as they come, so a run cut short still
 * shows them; progress lines every 20 s.
 */
static void
follow()
{
	static long off, last;
	char buf[4096], *l, *e;
	int fd, n;

	if ((fd = open(LOG, O_RDONLY)) < 0)
		return;
	lseek(fd, off, 0);
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	for (l = buf; (e = strchr(l, '\n')) != 0; l = e + 1) {
		*e = 0;
		if (strncmp(l, "lisa: pc ", 9) || t_now_ms() - last >= 20000) {
			if (!strncmp(l, "lisa: pc ", 9))
				last = t_now_ms();
			t_info("log", "%s", l);
		}
	}
	off += l - buf;
}

/* waits up to secs for the log to show what; 1 seen */
static int
await(pid, what, secs)
	int pid, secs;
	char *what;
{
	long t0 = t_now_ms();

	while (t_now_ms() - t0 < secs * 1000L) {
		follow();
		if (strstr(readlog(), what))
			return 1;
		if (kill(pid, 0) < 0)
			break;
		sleep(1);
	}
	return strstr(readlog(), what) != 0;
}

/* counter k (" faults ", " a-line ") in the last progress line, and its growth since the one before; -1 none */
static long
steady(log, k, all)
	char *log, *k;
	long *all;
{
	long f[2];
	int n = 0;
	char *s;

	*all = -1;
	for (s = log; (s = strstr(s, k)) != 0; s++) {
		f[0] = f[1];
		if (sscanf(s + strlen(k), "%ld", &f[1]) == 1)
			n++;
	}
	if (n)
		*all = f[1];
	return n < 2 ? -1 : f[1] - f[0];
}

static void
nap(ms)
	int ms;
{
	poll((struct pollfd *)0, 0, ms);
}

/* the pointer's top left on the host screen: what changes when it moves 1 right and down, alone */
static int
pointer(x, y)
	int *x, *y;
{
	int x1, y1, k;

	for (k = 0; k < 10; k++) {
		host("shot lp0");
		host("move 1 1");
		nap(700);
		host("shot lp1");
		if (host("box lp0 lp1") && sscanf(hrep, "%d %d %d %d", x, y, &x1, &y1) == 4 && x1 - *x < 40 &&
		    y1 - *y < 40)
			return 1;
		nap(1000);
	}
	return 0;
}

/* half of d, 0 within 2 */
static int
step(d)
	int d;
{
	if (d >= -2 && d <= 2)
		return 0;
	return d > 0 ? (d + 1) / 2 : -((1 - d) / 2);
}

/* the pointer to host x, y, half the way at a time whatever the scaling */
static int
moveto(x, y)
	int x, y;
{
	char req[40];
	int k, px, py;

	for (k = 0; k < 30 && pointer(&px, &py); k++) {
		if (abs(x - px) <= 2 && abs(y - py) <= 2)
			return 1;
		sprintf(req, "move %d %d 40 1", step(x - px), step(y - py));
		host(req);
		nap(500);
	}
	return 0;
}

/* what changed between two shots, "none" if nothing */
static char *
changed(a, b)
	char *a, *b;
{
	char req[64];

	sprintf(req, "box %s %s", a, b);
	return host(req) ? hrep : "?";
}

/*
 * The Lisa's mouse and keyboard on the desktop, the screen scaled to the
 * host's width and centred: the pointer moves, a click on Cancel closes the
 * micro diskette dialog, a click on the volume's icon and typing renames it,
 * and Apple-O (a beep) keeps it running.
 */
#define	LX(x)	((w - sw) / 2 + (x) * sw / 720)	/* a Lisa screen point on the host's */
#define	LY(y)	(top + (y) * sh / 364)

static void
input()
{
	char *log = readlog(), *s;
	int x, y, w = 0, h = 0, sw = 0, sh = 0, top;

	if ((s = strstr(log, "lisa: display ")) != 0)
		sscanf(s, "lisa: display %dx%d, the screen at %dx%d", &w, &h, &sw, &sh);
	t_check("display", w > 0 && sw == (w < 720 ? w : 720) && sh == (h < 364 ? h : 364), "%dx%d, the screen at %dx%d", w, h, sw, sh);
	if (w <= 0)
		return;
	top = (h - sh) / 2;
	t_check("mouse", pointer(&x, &y), "the pointer did not move");
	if (t_check("to_cancel", moveto(LX(596), LY(70)), "pointer not at the dialog's Cancel")) {
		host("shot li0");
		host("click 1");
		nap(5000);
		host("shot li1");
		t_check("cancel", strcmp(changed("li0", "li1"), "none") != 0, "no change");
	}
	/* the first click may only bring the desktop forward */
	moveto(LX(675), LY(352));
	host("click 1");
	nap(3000);
	host("click 1");
	nap(3000);
	host("shot li2");
	host("key a");
	host("key b");
	nap(3000);
	host("shot li3");
	s = changed("li2", "li3");
	/* the new name shows under the icon */
	t_check("keyboard", sscanf(s, "%*d %*d %d %d", &x, &y) == 2 && y >= LY(345), "typing changed %s", s);
	host("key meta_l+o");
	nap(5000);
	shot("lisa_desk", 1);
}

static void
boot()
{
	struct mod_mreg reg;
	char *log, *s;
	int pid, st = 0, alive, mj = 68, gone;
	long t0, lit = 0, all;

	if (access(MD "/lisaguest", 0) != 0) {
		t_skip("boot", "no lisaguest module on this root");
		return;
	}
	modpath(MD);
	memset(&reg, 0, sizeof reg);
	strcpy(reg.md_modname, "lisaguest");
	reg.md_typedata = (caddr_t)&mj;
	t_check("register_lisaguest", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST, "%s", T_ERR);
	if (access(DEV, 0) != 0 || access("/dev/lisa", 0) != 0 || access("/tests/startlisa", X_OK) != 0 ||
	    system("/sbin/mount -F ufs -r " DEV " " MNT) != 0) {
		t_skip("boot", "no Lisa volume or startlisa");
		return;
	}
	if (access(MNT "/lisa.rom", 0) != 0) {
		system("/sbin/umount " MNT);
		t_skip("boot", "no Lisa ROM");
		return;
	}
	if (modload("/etc/conf/mod.d/dsuser") < 0 && errno != EEXIST)
		t_info("display", "dsuser: %s", T_ERR);
	hostopen();
	unlink(LOG);
	system("rm -rf " ROOT);
	mkdir(ROOT, 0755);
	t_check("folder", symlink(MNT "/profile.dc42", ROOT "/profile.dc42") == 0 &&
	    symlink(MNT "/lisa.rom", ROOT "/lisa.rom") == 0, "%s", T_ERR);
	if ((pid = fork()) == 0) {
		int fd = open(LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		close(0);
		open("/dev/null", O_RDONLY);
		dup2(fd, 1);
		dup2(fd, 2);
		putenv("NATIVEHOST_ROOT=" ROOT);
		execl("/tests/startlisa", "startlisa", "-v", (char *)0);
		_exit(127);
	}
	t0 = t_now_ms();
	t_check("running", await(pid, "lisa: running", 30), "not running");
	t_check("volume_read", await(pid, "lisa: first volume read", 30), "no block read");
	t_check("screen_text", await(pid, "pixels lit", RUNS), "nothing drawn");
	while (t_now_ms() - t0 < RUNS * 1000L && kill(pid, 0) == 0) {
		follow();
		sleep(2);
	}
	log = readlog();
	for (s = log; (s = strstr(s, "pixels lit")) != 0; s++) {
		char *e = s;

		while (e > log && e[-1] != ' ')
			e--;
		while (e > log && e[-1] == ' ')
			e--;
		while (e > log && e[-1] != ' ')
			e--;
		lit = atol(e);
	}
	shot("lisa_end", 1);
	if (hfd >= 0 && kill(pid, 0) == 0)
		input();
	gone = waitpid(pid, &st, WNOHANG) != 0;
	alive = !gone;
	if (alive) {
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
	}
	follow();
	log = readlog();
	s = strrchr(log, '\n');
	while (s && s > log && s[-1] != '\n')
		s--;
	t_info("progress", "%s", strlen(log) > 400 ? log + strlen(log) - 400 : log);
	{
		struct stat sb;

		t_info("writes", "%ld bytes in the overlay", stat(ROOT "/profile.writes", &sb) == 0 ? (long)sb.st_size : -1L);
	}
	t_check("text_lit", lit > 500, "%ld pixels lit", lit);
	lit = steady(log, " a-line ", &all);
	t_check("aline_user", lit == 0, "%ld A-lines through supervisor mode in 2 s", lit);
	lit = steady(log, " faults ", &all);
	t_check("native", lit == 0, "%ld hardware-page faults in 2 s", lit);
	t_check("no_hw_access", !strstr(log, "hardware access"), "the guest reached a hardware page");
	/* bus errors are the OS's own segment loads */
	t_check("fault_free", alive && all == 0 && !strstr(log, "startlisa:"),
	    "%s (status 0x%x), %ld hardware-page faults; last line: %s", alive ? "running" : "exited", st, all, s ? s : "");
	system("/sbin/umount " MNT);
}

int
main()
{
	t_init("lisa", 540);
	boot();
	return t_done();
}
