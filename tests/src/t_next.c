/*
 * t_next: the nextguest module (registered, loaded, opened), then the
 * user's kernel from the OPENSTEP CD on SCSI disk 6 (tests/next/mkvol.sh),
 * loaded and entered by the native host without a ROM: it must print its
 * version line on the host console, stop at its missing root by returning to
 * the monitor (a host call), with its board routines made host calls and
 * no access to an address nothing backs once it runs.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termio.h>
#include <time.h>
#include "sys/mod.h"
#include "next/profile.h"
#include "t.h"
#include "tflush.h"

#define	MD	"/tests/aux/mod.d"
#define	DEV	"/dev/dsk/c6d0s0"
#define	MNT	"/next"
#define	ROOT	"/tmp/next"
#define	FULLROOT	MNT "/fs"
#define	LOG	"/tmp/next.log"
#define	ERRLOG	"/tmp/next.err"

static int dev = -1;
static int full;			/* the volume holds the whole CD tree */
static char *root = ROOT;

/* the host console (the machine's text) and the launcher's own lines, kept apart so neither splits the other */
static char logbuf[1048576], errbuf[1048576];

static void
readlast(path, buf, size)
	char *path, *buf;
	int size;
{
	int fd, n = 0;

	if ((fd = open(path, O_RDONLY)) >= 0) {
		off_t end = lseek(fd, (off_t)0, 2);

		lseek(fd, end > (off_t)size - 1 ? end - (off_t)size + 1 : (off_t)0, 0);
		n = read(fd, buf, size - 1);
		close(fd);
	}
	buf[n > 0 ? n : 0] = 0;
}

/* the launcher's first lines, which a long run's host-call lines push out of the tail */
static char errhead[262144];

static void
logread()
{
	int fd, n = 0;

	readlast(LOG, logbuf, (int)sizeof logbuf);
	readlast(ERRLOG, errbuf, (int)sizeof errbuf);
	if ((fd = open(ERRLOG, O_RDONLY)) >= 0) {
		n = read(fd, errhead, sizeof errhead - 1);
		close(fd);
	}
	errhead[n > 0 ? n : 0] = 0;
}

/* the last lines of the log, without the register census and host calls; host calls without loops */
static void
logtail(hfs)
	int hfs;
{
	static char c[sizeof logbuf];
	char *p, *e, *l[100];
	int i, j, k = 0;

	strcpy(c, hfs ? errbuf : logbuf);
	for (p = c; *p; p = e) {
		e = strchr(p, 10);
		if (e)
			*e++ = 0;
		else
			e = p + strlen(p);
		for (j = 1; hfs && j <= 16 && j <= k && strcmp(l[(k - j) % 100], p); j++)
			;
		if (hfs && j <= 16 && j <= k)
			continue;
		if (!strstr(p, "census") && (hfs ? strstr(p, "hfs op") != 0 : !strstr(p, "hfs op")))
			l[k++ % 100] = p;
	}
	for (i = k > (hfs ? 40 : 100) ? k - (hfs ? 40 : 100) : 0; i < k; i++)
		t_info("log", "%s", l[i % 100]);
}

static off_t errmark;		/* the launcher's log size at the logout */
static off_t sndmark;		/* and before the console's commands */

/* whether the launcher's lines since the logout hold the text */
static int
errsince(text)
	char *text;
{
	static char b[65536 + 64];
	int fd, n, k = 0, found = 0;

	if ((fd = open(ERRLOG, O_RDONLY)) < 0)
		return 0;
	lseek(fd, errmark, 0);
	while (!found && (n = read(fd, b + k, sizeof b - 1 - k)) > 0) {
		b[k + n] = 0;
		found = strstr(b, text) != 0;
		/* keep a tail so the text may span two reads */
		k = k + n > 63 ? 63 : k + n;
		memmove(b, b + strlen(b) - k, k);
	}
	close(fd);
	return found;
}

/* the names the guest looked up since the mark, a line of them at a time */
static void
lookups()
{
	static char b[4096];
	char line[200], *p, *e, *n;
	FILE *f;
	int k = 0;

	if ((f = fopen(ERRLOG, "r")) == 0)
		return;
	fseek(f, (long)errmark, 0);
	line[0] = 0;
	while (k < 60 && fgets(b, sizeof b, f)) {
		if (!strstr(b, "hfs op 2 ") || !strstr(b, "error 0 ") || (p = strstr(b, "result 0 ")) == 0)
			continue;
		n = p + 9;
		if ((e = strchr(n, 10)) != 0)
			*e = 0;
		if (strlen(line) + strlen(n) > 150) {
			t_info("lookups", "%s", line);
			line[0] = 0;
			k++;
		}
		strcat(line, " ");
		strcat(line, n);
	}
	if (line[0])
		t_info("lookups", "%s", line);
	fclose(f);
}

/* whether the host console log holds the text */
static int
logfind(text)
	char *text;
{
	return strstr(logbuf, text) != 0 || strstr(errbuf, text) != 0 || strstr(errhead, text) != 0;
}


static int scrw = 1152, scrh = 870, scrrow = 288, direct;	/* the guest's screen, from the launcher */
static pid_t machine;

/* the launcher's screen line: size, row, whether the guest draws into the host screen */
static void
geometry()
{
	char *q = strstr(errhead, "next: screen ");

	if (q && sscanf(q, "next: screen %dx%d, %d", &scrw, &scrh, &scrrow) == 3)
		direct = strncmp(q + strcspn(q, "\n") - 6, "direct", 6) == 0;
}

/* a point of the desktop in a shot: below the File Viewer, or between the dock's tiles on a small screen */
static long pix();
static long
desktop(name)
	char *name;
{
	return scrw < 1120 ? pix(name, scrw - 32, scrh - 110) : pix(name, 600, 700);
}

static int hfd = -1;		/* the host line, -1 none */
static int hseq;
static char hrep[128];

/* send a host request; the answer in hrep, 0 if none in 20 s */
static char *
host(req)
	char *req;
{
	char buf[160], line[256];
	struct pollfd p;
	int n, len = 0;
	long t0;

	if (hfd < 0)
		return 0;
	sprintf(buf, "@@ %d %s\n", ++hseq, req);
	write(hfd, buf, strlen(buf));
	t0 = t_now_ms();
	p.fd = hfd;
	p.events = POLLIN;
	while (t_now_ms() - t0 < 20000) {
		char *nl, *s;
		int seq;

		if (poll(&p, 1, 500) <= 0)
			continue;
		if ((n = read(hfd, line + len, sizeof line - 1 - len)) <= 0)
			continue;
		len += n;
		line[len] = 0;
		while ((nl = strchr(line, '\n')) != 0) {
			*nl = 0;
			s = strstr(line, "@@ok ");
			if (s && sscanf(s + 5, "%d", &seq) == 1 && seq == hseq) {
				s = strchr(s + 5, ' ');
				strncpy(hrep, s ? s + 1 : "", sizeof hrep - 1);
				hrep[strcspn(hrep, "\r")] = 0;
				return hrep;
			}
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

/* a screen dump of the NeXT surface, named in the results */
static void
shot(name)
	char *name;
{
	char req[40];

	sprintf(req, "shot %s", name);
	if (host(req))
		t_info("shot", "%s %s", name, hrep);
}

static void
nap(ms)
	int ms;
{
	poll((struct pollfd *)0, 0, ms);
}

/* the pointer's top left on the host screen: what changes when it moves 2 right and down, alone */
static int
pointer(x, y)
	int *x, *y;
{
	int x1, y1, k;

	for (k = 0; k < 10; k++) {
		host("shot np0");
		host("move 1 1");
		nap(700);
		host("shot np1");
		if (host("box np0 np1") && sscanf(hrep, "%d %d %d %d", x, y, &x1, &y1) == 4 && x1 - *x < 48 &&
		    y1 - *y < 48)
			return 1;
		nap(2000);
	}
	return 0;
}

/* d over the scale s, at least 2 (pointer() moves it 1) unless d is within 3 */
static int
step(d, s)
	int d, s;
{
	if (d >= -3 && d <= 3)
		return 0;
	d = d > 0 ? (d + s - 1) / s : -((s - 1 - d) / s);
	return d == 1 || d == -1 ? 2 * d : d;
}

/*
 * The pointer from the screen's top left to x, y.  The event driver speeds
 * up moves by how many land between its reads, so a move that overshoots
 * doubles that axis's scale.
 */
static int
moveto(x, y)
	int x, y;
{
	char req[40];
	int k, px, py, dx = 0, dy = 0, sx = 2, sy = 2;

	host("move -2000 -2000 20");
	nap(1000);
	for (k = 0; k < 40 && pointer(&px, &py); k++) {
		if (abs(x - px) <= 3 && abs(y - py) <= 3)
			return 1;
		if (dx && (x - px) * dx < 0 && sx < 64)
			sx *= 2;
		if (dy && (y - py) * dy < 0 && sy < 64)
			sy *= 2;
		dx = step(x - px, sx);
		dy = step(y - py, sy);
		sprintf(req, "move %d %d 40 1", dx, dy);
		host(req);
		nap(500);
	}
	return 0;
}

/* a pixel of the last named shot, -1 off the screen */
static long
pix(name, x, y)
	char *name;
	int x, y;
{
	char req[40];

	sprintf(req, "pix %s %d %d", name, x, y);
	return host(req) && strcmp(hrep, "none") != 0 ? strtol(hrep, (char **)0, 16) : -1;
}

/*
 * Log out the stock way: Log Out, the last item of the Workspace menu at the screen's top left,
 * and confirm.  Without the menu, double-click the Workspace's dock tile first.  Return first
 * for an alert (the first login warns of low disk space).
 */
static int
logout()
{
	long desk;
	int x0 = 0, y0 = 0, y;

	host("key ret");
	nap(5000);
	shot("next_menu");
	desk = desktop("next_menu");
	if (pix("next_menu", x0 + 4, y0 + 4) == desk) {
		if (!moveto(scrw - 32, 30))
			return 0;
		host("click 2");
		nap(30000);
		shot("next_menu");
	}
	if (pix("next_menu", x0 + 4, y0 + 4) == desk)
		return 0;
	/* items of 20 rows below the 22-row title, each with a white top edge; Log Out is the last */
	for (y = y0 + 22; pix("next_menu", x0 + 4, y) == 0xffffff && pix("next_menu", x0 + 4, y + 1) == 0xaaaaaa; y += 20)
		;
	t_info("menu", "ends at %d", y);
	if (y == y0 + 22 || !moveto(x0 + 30, y - 10))
		return 0;
	host("click 1");
	/* the confirmation panel takes a while: the Workspace is busy */
	nap(30000);
	shot("next_logout");
	host("key ret");
	return 1;
}

/* the dumps' difference in pixels, -1 if none */
static int
diff(a, b)
	char *a, *b;
{
	char req[60];
	int n = -1;

	sprintf(req, "cmp %s %s", a, b);
	if (host(req) && strcmp(hrep, "same") == 0)
		n = 0;
	else
		sscanf(hrep, "diff %d", &n);
	return n;
}

/*
 * With the console in front the running NeXT draws nothing there; back in
 * front its screen returns (its clock may have moved on).
 */
static void
switchback()
{
	int n, m;

	host("key ctrl+alt+meta_l+0");
	nap(2000);
	/* the console shows this test's lines: no line between the two */
	host("shot next_console0");
	nap(5000);
	host("shot next_console1");
	n = diff("next_console0", "next_console1");
	m = diff("next_workspace", "next_console0");
	t_check("back_draws_nothing", n == 0 && m > 10000, "console changed by %d, %d from the NeXT's", n, m);
	host("key ctrl+alt+meta_l+1");
	nap(3000);
	shot("next_front");
	n = diff("next_workspace", "next_front");
	t_check("front_redrawn", n >= 0 && n < 1500, "%d pixels differ from before the switch", n);
}

/* type a line on the host keyboard */
static void
typeline(text)
	char *text;
{
	char req[20];

	for (; *text; text++) {
		sprintf(req, *text >= 'A' && *text <= 'Z' ? "key shift+%c" : *text == ' ' ? "key spc" : *text == '.' ?
		    "key dot" : *text == '/' ? "key slash" : *text == '-' ? "key minus" : *text == '>' ? "key shift+dot" :
		    "key %c", *text | 040);
		host(req);
	}
	host("key ret");
}

/* the Name field's caret blinking: two shots differ in a thin upright box only */
static int
caret()
{
	int k, x0, y0, x1, y1;

	for (k = 0; k < 60; k++) {
		host("shot nc0");
		nap(300);
		host("shot nc1");
		if (host("box nc0 nc1") && sscanf(hrep, "%d %d %d %d", &x0, &y0, &x1, &y1) == 4 && x1 - x0 <= 4 &&
		    y1 - y0 >= 8 && y1 - y0 <= 30)
			return 1;
	}
	return 0;
}

/* "console" as the panel's name: the login window exits to the text console */
static void
console()
{
	/* the panel takes keys once its Name field has the caret */
	if (!caret())
		t_info("caret", "none in the panel");
	typeline("console");
	shot("next_console");
}

/* shell prompts on the console so far */
static int
prompts()
{
	char *p;
	int n = 0;

	for (p = logbuf; (p = strstr(p, "# ")) != 0; p++)
		n++;
	return n;
}

/* a directory the shell stands in, removed and made a host link: the guest must not reach the host through it */
static void
escape()
{
	static char *cmd[] = { "mkdir /d", "cd /d", "rmdir /d", "ln -s /tmp /d", "mkdir next.esc", "ln -s /bin /b",
	    "/b/echo linkok", "sndplay /NextLibrary/Sounds/Basso.snd", 0 };
	char **c;
	int n, i;

	for (c = cmd; *c; c++) {
		logread();
		n = prompts();
		typeline(*c);
		/* the next line once the shell prompts again: keys typed earlier would reach the command */
		for (i = 0; i < 150 && prompts() <= n; i++) {
			poll((struct pollfd *)0, 0, 100);
			logread();
		}
	}
}

/* a file of the root folder */
static char *
rootfile(name)
	char *name;
{
	static char path[64];

	sprintf(path, "%s/%s", root, name);
	return path;
}

/* a file beside the root folder: ROOT.name */
static char *
sidefile(name)
	char *name;
{
	static char path[64];

	sprintf(path, "%s.%s", root, name);
	return path;
}

/* the guest's screen memory, from its file; the bytes read */
static unsigned char screenmem[NX_VRAM_SIZE];

static int
screen()
{
	int fd, n;

	if ((fd = open(sidefile("vram"), O_RDONLY)) < 0)
		return -1;
	n = read(fd, screenmem, direct ? scrrow * scrh : sizeof screenmem);
	close(fd);
	return n;
}

/* bytes of the screen memory that are not zero */
static int
vramset()
{
	int n = 0, i = screen();

	if (i < 0)
		return -1;
	while (i-- > 0)
		n += screenmem[i] != 0;
	return n;
}

/* light grey pixels: loginwindow's panel on the dark grey desktop */
static int
panel()
{
	int n = 0, i = screen(), k;

	while (i-- > 0)
		if (direct)
			n += screenmem[i] >= 0x80 && screenmem[i] <= 0xaa;	/* the panel is a gradient */
		else
			for (k = 0; k < 8; k += 2)
				n += (screenmem[i] >> k & 3) == 1;
	return n > 20000;
}

/* hardware-page faults so far: accesses to addresses nothing backs */
static unsigned long
iofaults(in)
	struct nx_info *in;
{
	return in->io;
}

/* the kernel loaded and entered without the ROM: it runs and probes the board */
static void
kernel()
{
	static struct nx_info info, now;
	struct stat st;
	unsigned long io0 = 0, priv0 = 0, fill0 = 0, hcall0 = 0;
	pid_t pid;
	int i, hw, hh, gone = 0, status, banner, typed = 0, after = 0, nopanel = 0, panelkeys = 0, held = 0, again = 0;
	char *q;
	long t0 = t_now_ms(), login = 0;

	if (access(DEV, 0) != 0 || system("/sbin/mount -F ufs " DEV " " MNT) != 0) {
		t_skip("kernel", "no NeXT volume");
		return;
	}
	full = access(FULLROOT "/NextApps", 0) == 0;
	if (full)
		root = FULLROOT;
	if (access(MNT "/native", X_OK) != 0 || access(full ? FULLROOT "/mach_kernel" : MNT "/mach_kernel", R_OK) != 0) {
		t_skip("kernel", "no native or kernel on the volume");
		system("/sbin/umount " MNT);
		return;
	}
	if (!full) {
		mkdir(ROOT, 0700);
		system("cp -r " MNT "/fs/etc " MNT "/fs/usr " MNT "/fs/bin " ROOT "; cp " MNT "/fs/.meta " ROOT ".meta");
	}
	unlink(sidefile("vram"));
	unlink(sidefile("nvram"));
	unlink(rootfile("d"));
	rmdir(rootfile("d"));
	unlink(rootfile("b"));
	rmdir("/tmp/next.esc");
	unlink(LOG);
	unlink(ERRLOG);
	/* device nodes: the console, the controlling tty and null (raw = major << 8 | minor) */
	if (!full)
		system("mkdir -p " ROOT "/dev; cd " ROOT "/dev; echo 'mode=020622 raw=00000000' > console.__special__;"
		    " echo 'mode=020666 raw=00000200' > tty.__special__; echo 'mode=020666 raw=00000302' > null.__special__");
	if ((pid = fork()) == 0) {
		int fd = open(LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644), efd = open(ERRLOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		setpgrp();
		close(0);
		open("/dev/null", O_RDONLY);
		dup2(fd, 1);
		dup2(efd, 2);
		putenv(full ? "NATIVEHOST_ROOT=" FULLROOT : "NATIVEHOST_ROOT=" ROOT);
		putenv("NEXT_CENSUS=1");
		execl(MNT "/native", "native", "-k", full ? FULLROOT "/mach_kernel" : MNT "/mach_kernel", (char *)0);
		_exit(127);
	}
	machine = pid;
	/*
	 * The module forgets the machine when it ends: keep the last look.
	 * The whole tree: the first boot's language panel and its confirmation take a return each; at
	 * loginwindow's panel "console" gives the text console; log in there, run the commands, exit; back
	 * at the panel log in as root and wait for the Workspace Manager.  A boot that logs in by itself
	 * logs out through the Workspace menu first.
	 */
	/* ends before the runner's limit so the log is shown */
	for (i = 0; i < (full ? 18000 : 300) && !gone && after < (full ? 900 : 50) && t_now_ms() - t0 < 1700000; i++) {
		after += typed == (full ? 6 : 2);
		poll((struct pollfd *)0, 0, 100);
		/*
		 * loginwindow skips the automatic login while the button is down.  The window server
		 * restarts after a logout or the console's exit and learns the button from events: hold it
		 * and drag a pixel at every poll until the panel shows, before a fast restart logs in.
		 */
		if ((typed == 4 || typed == 11) && held >= 0) {
			/* the old Workspace's own lookups are not a new login */
			if (held == 0)
				errmark = stat(ERRLOG, &st) == 0 ? st.st_size : 0;
			host(held++ == 0 ? "button 1 30" : held & 1 ? "move 1 0" : "move -1 0");
		}
		if (i % 20 == 19) {
			logread();
			if (i == 19)
				geometry();
			if (full && (!typed || typed == 7 || typed == 8) && strstr(errbuf, "result 0 Workspace\n")) {
				/* the first boot, and later ones, log in by themselves */
				t_info("workspace", "after %ld s", (t_now_ms() - t0) / 1000);
				typed = 9;
				nopanel = i;
			} else if (typed == 9 && i > nopanel + 600) {
				t_rearm(900);
				typed = logout() ? 10 : 6;
				errmark = stat(ERRLOG, &st) == 0 ? st.st_size : 0;
				t_info("logout", "%s", typed == 10 ? "done" : "no Workspace menu");
				nopanel = i;
			} else if (typed == 11 && errsince("result 0 Workspace\n")) {
				host("button 0 100");
				t_info("logout", "logged in again by itself");
				typed = 6;
				again = 1;
			} else if (typed == 10 && !panel()) {
				/* the Workspace's windows are gone: the panel comes next */
				typed = 11;
				nopanel = i;
			} else if (typed == 11 && i > nopanel + 50 && panel()) {
				t_rearm(900);
				host("button 0 100");
				held = -1;
				shot("next_panel");
				console();
				typed = 1;
				nopanel = i;
			} else if (full && !typed && panel()) {
				/* the first boot's language and keyboard panel: English, USA */
				shot("next_language");
				host("key ret");
				typed = 7;
				nopanel = i;
			} else if (typed == 7 && i > nopanel + 100) {
				shot("next_confirm");
				host("key ret");
				typed = 8;
				nopanel = i;
			} else if (typed == 8 && i > nopanel + 100 && panel()) {
				shot("next_panel");
				console();
				typed = 1;
				nopanel = i;
			} else if (full && typed == 1 && i > nopanel + 3000 && !strstr(logbuf, "login: ")) {
				shot("next_noconsole");
				break;
			} else if ((full ? typed == 1 : !typed) && strstr(logbuf, "login: ")) {
				login = (t_now_ms() - t0) / 1000;
				typeline("root");
				typed = full ? 2 : 1;
			} else if ((full ? typed == 2 : typed == 1) ? strchr(strstr(logbuf, "login: "), '#') != 0 :
			    !full && !typed && logfind("\n#")) {
				/* a shell after the login, or the small root's single-user one */
				typeline("echo kbd");
				typed = full ? 3 : 2;
			} else if (typed == 3 && logfind("\r\nkbd\r\n")) {
				sndmark = stat(ERRLOG, &st) == 0 ? st.st_size : 0;
				escape();
				typeline("exit");
				typed = 4;
				nopanel = i;
				held = 0;
				errmark = stat(ERRLOG, &st) == 0 ? st.st_size : 0;
			} else if (typed == 4 && i > nopanel + 3000) {
				shot("next_nopanel");
				break;
			} else if (typed == 4 && errsince("result 0 Workspace\n")) {
				host("button 0 100");
				t_info("exit", "logged in again by itself");
				typed = 6;
				again = 1;
			} else if (typed == 4 && i > nopanel + 100 && panel() && host("shot next_panel2") &&
			    host("box next_language next_panel2") && strcmp(hrep, "none") != 0) {
				host("button 0 100");
				held = -1;
				typeline("root");
				shot("next_name");
				panelkeys = host("box next_panel2 next_name") && strcmp(hrep, "none") != 0;
				host("key ret");
				typed = 5;
			} else if (typed == 5 && errsince("result 0 Workspace\n")) {
				t_info("workspace", "after %ld s", (t_now_ms() - t0) / 1000);
				typed = 6;
			}
		}
		if (ioctl(dev, NX_INFO, &now) == 0 && now.ncensus >= info.ncensus)
			info = now;
		if (after == 1) {
			io0 = iofaults(&info);
			priv0 = info.mmu[4];
			fill0 = info.mmu[0];
			hcall0 = info.hcalls[0];
		}
		if (after == 899)
			shot("next_workspace");
		if (full ? i % 1200 == 1199 : i == 60 || i == 190) {
			char name[20];

			sprintf(name, "next_%d", i + 1);
			shot(name);
			t_rearm(600);
		}
		gone = waitpid(pid, &status, WNOHANG) != 0;
		if (gone) t_info("exit", "status %x", status);
	}
	logread();
	logtail(0);
	logtail(1);
	if (typed == 4)
		lookups();
	/* the launcher's own complaint when it ends early */
	if (gone && i < 50)
		for (q = errbuf; q && *q && q - errbuf < 2000; q = strchr(q, 10) ? strchr(q, 10) + 1 : 0)
			t_info("err", "%.100s", q);
	t_info("kernel", "gone %d ticks %lu ints %lu io %lu last %lx pc %lx level %lu stat %lx mask %lx", gone, info.ticks,
	    info.ints, info.io, info.lasta, info.lastpc, info.level, info.stat, info.mask);
	t_info("mmu", "fills %lu faults %lu space switches %lu flushes %lu, privileged %lu, ticks %lu",
	    info.mmu[0], info.mmu[1], info.mmu[2], info.mmu[3], info.mmu[4], info.ticks);
	t_info("privkinds", "SR ops %lu rte %lu from SR %lu to SR %lu usp %lu movec %lu moves %lu cache %lu MMU %lu fsave %lu frestore %lu other %lu; fast %lu",
	    info.privk[0], info.privk[1], info.privk[2], info.privk[3], info.privk[4], info.privk[5], info.privk[6],
	    info.privk[7], info.privk[8], info.privk[9], info.privk[10], info.privk[11], info.privk[12]);
	for (i = 0; i < 8; i++) {
		int k, b = -1;

		for (k = 0; k < 16; k++)
			if (info.privpc[2 * k + 1] && (b < 0 || info.privpc[2 * k + 1] > info.privpc[2 * b + 1]))
				b = k;
		if (b < 0)
			break;
		t_info("privpc", "%lx %lu", info.privpc[2 * b], info.privpc[2 * b + 1]);
		info.privpc[2 * b + 1] = 0;
	}
	t_info("space", "above the guest's space %lu, last %lx pc %lx", info.high, info.higha, info.highpc);
	for (i = 0; i < (int)info.ncensus && i < NX_NCENSUS; i++)
		t_info("io", "%s %lx x%lu pc %lx last pc %lx", info.census[i].write ? "write" : "read",
		    info.census[i].addr, info.census[i].count, info.census[i].pc, info.census[i].lastpc);
	t_info("hcalls", "hfs %lu us %lu delay %lu timer %lu soft %lu time %lu nvram %lu/%lu copyin %lu copyout %lu copyinstr %lu copyoutstr %lu (faulted in %lu) pmap %lu",
	    info.hcalls[1], info.hcalls[3], info.hcalls[4], info.hcalls[5], info.hcalls[6], info.hcalls[7], info.hcalls[8],
	    info.hcalls[9], info.hcalls[13], info.hcalls[14], info.hcalls[15], info.hcalls[16], info.chains, info.hcalls[0]);
	if (login)
		t_info("login", "prompt after %ld s", login);
	t_check("km_on_screen", vramset() > 1000, "the kernel's console drew nothing on the screen");
	geometry();
	if (host("shot next_end") && sscanf(hrep, "%d %d", &hw, &hh) == 2) {
		char want[40];

		t_check("screen_host", scrw == hw && scrh == hh, "screen %dx%d on a %dx%d host", scrw, scrh, hw, hh);
		t_check("direct", direct || hw * hh > NX_VRAM_SIZE, "a %dx%d host screen is not drawn into directly", hw, hh);
		sprintf(want, "next: console %dx%d,", hw, hh);
		t_check("console_size", logfind(want), "the kernel's console is not %dx%d", hw, hh);
	}
	if (direct)
		t_check("no_present", !logfind("next: present pass"), "the host screen was copied from a frame of its own");
	banner = logfind("NeXT Mach ");
	t_check("kernel_banner", banner, "no version line from the kernel's console");
	t_check("root_installed", logfind("next: root filesystem installed"), "the host filesystem was not installed");
	t_check("root_not_network", !logfind("no suitable root"), "the root probe stopped the kernel");
	t_check("root_mounted", logfind("root mounted from the host folder"), "the kernel did not mount the host folder as root");
	t_check("init_exec", !logfind("Load of /etc/mach_init") && logfind("hfs op 5 "),
	    "mach_init was not read and started as the first user task");
	t_info("init", "user mode: space switches %lu, fills %lu", info.mmu[2], info.mmu[0]);
	if (!full)
		t_check("rc_boot", logfind("error 0 result 0 sh\n") && logfind("error 0 result 0 rc.boot\n"),
		    "init did not start the shell on /etc/rc.boot");
	t_check("kernel_stays_up", !logfind("panic:"), "the kernel panicked");
	if (full) {
		t_check("login_panel", typed >= 1 && typed <= 6, "no login panel");
		t_check("login_prompt", logfind("login: "), "no login prompt on the console after console");
	}
	t_check("keys_reach_shell", logfind("echo kbd\r\n"), "typed keys did not reach the console's shell");
	t_check("keys_run", logfind("\r\nkbd\r\n"), "the typed command did not run");
	if (typed >= 3 && typed <= 6 && full) {
		t_check("host_confined", access("/tmp/next.esc", 0) != 0, "a guest directory made a host link reached the host");
		t_check("guest_symlink", logfind("\r\nlinkok\r\n"), "a link made in the guest did not lead to its target");
	}
	if (full) {
		t_check("panel_returns", typed >= 5 && typed <= 6 && !again, "no login panel after the console's exit");
		t_check("panel_keys", panelkeys, "typed keys did not reach the login panel");
	}
	if (full) {
		t_check("workspace", typed == 6 && !again, "no Workspace Manager after root's login");
		if (typed == 6 && after > 899) {
			long desk = desktop("next_workspace");

			/* the menu's title, black, at the host screen's top left at 1:1 */
			t_check("menu_topleft", pix("next_workspace", 4, 4) == 0 && pix("next_workspace", 4, 30) != desk,
			    "no Workspace menu at the screen's top left");
			switchback();
		}
	}
	if (after > 1) {
		t_check("io_steady", iofaults(&info) == io0, "%lu hardware-page faults in the %d s after %s",
		    iofaults(&info) - io0, (after - 1) / 10, full ? "the login" : "the command");
		/* Mach's MMU and cache instructions are still emulated: reported, not checked */
		t_info("emulated_steady", "%lu privileged instructions emulated, %lu MMU fills, %lu pmap calls in the %d s after %s",
		    info.mmu[4] - priv0, info.mmu[0] - fill0, info.hcalls[0] - hcall0, (after - 1) / 10,
		    full ? "the login" : "the command");
	}
	if (full) {
		/* rc.boot's text with today's date, then init starts /etc/rc */
		time_t now = time((time_t *)0);
		char year[8];

		/* the console ends its lines with a return */
		sprintf(year, " %d\r\n", localtime(&now)->tm_year + 1900);
		t_check("rc_boot_text", logfind("Setting up root mount entries"), "no rc.boot text on the console");
		t_check("clock", logfind(year), "the date rc.boot printed is not this year");
		t_check("rc_runs", logfind("error 0 result 0 rc\n"), "init did not start /etc/rc");
		/* no network: rc.net brings up the loopback alone and the daemons bind to it */
		t_check("loopback", logfind("lo0: address automatically set to 127.0.0.1") &&
		    logfind("Reinitializing nmserver"), "rc.net did not configure lo0");
		t_check("daemons_bind", !logfind("Can't assign requested address"), "a daemon could not bind to lo0");
		t_check("rc_done", logfind("Starting standard daemons"), "/etc/rc stopped before its last daemons");
	} else
		t_check("rc_boot_done", logfind("error 2 result 0 hostconfig\n") && logfind("error 2 result 0 utmp\n"),
		    "init did not get past rc.boot to the utmp file");
	t_check("guest_space", info.high == 0, "%lu guest accesses above its space, last %lx pc %lx", info.high,
	    info.higha, info.highpc);
	t_check("host_alive", !gone || !WIFSIGNALED(status), "the host died of signal %d", WTERMSIG(status));
	t_check("host_services", logfind("next: the board's routines are host calls"), "a board routine was not patched");
	t_check("sound_hook", !logfind("next: no sound out"), "the kernel's server calls could not be hooked");
	if ((q = strstr(errhead, "next: sound out at")) != 0 || (q = strstr(errbuf, "next: sound out at")) != 0)
		t_info("sound", "%.*s", (int)strcspn(q, "\n"), q);
	if (typed >= 3 && typed <= 6 && full) {
		errmark = sndmark;
		t_check("sound_rate", errsince("next: sound out at 22050 Hz"), "a 22050 Hz sound did not play at 22050 Hz");
	}
	t_check("timer_ints", info.ints > 0 && info.hcalls[5] > 0, "no timer interrupts");
	t_info("vram", "%d bytes set", vramset());
	if (direct && (i = screen()) > 0) {
		long h[4] = { 0, 0, 0, 0 };

		while (i-- > 0)
			h[screenmem[i] / 85 % 4] += screenmem[i] % 85 == 0;
		t_info("gray", "black %ld dark %ld light %ld white %ld", h[0], h[1], h[2], h[3]);
	}
	if (!gone)
		kill(-pid, SIGTERM);
	waitpid(pid, &status, 0);
	for (i = 0; i < 100 && kill(-pid, 0) == 0; i++)
		poll((struct pollfd *)0, 0, 100);
	unlink(sidefile("vram"));
	unlink(sidefile("nvram"));
	unlink(rootfile("d"));
	rmdir(rootfile("d"));
	unlink(rootfile("b"));
	rmdir("/tmp/next.esc");
	system("/sbin/umount " MNT);
	if (!full)
		system("rm -rf " ROOT " " ROOT ".meta");
	unlink(LOG);
	unlink(ERRLOG);
}

static int
modid(name)
	char *name;
{
	struct modstatus st;
	int id = 1;

	while (modstat(id, &st, 1) == 0) {
		if (strcmp(st.ms_name, name) == 0)
			return st.ms_id;
		id = st.ms_id + 1;
	}
	return -1;
}

int
main()
{
	struct mod_mreg reg;
	int mj = NX_MAJOR, id;
	long f[5];

	t_init("next", 600);
	if (access(MD "/nextguest", 0) < 0 || access("/dev/nextguest", 0) < 0) {
		t_skip("module", "no nextguest module on this root");
		return t_done();
	}
	modpath(MD);
	memset(&reg, 0, sizeof reg);
	strcpy(reg.md_modname, "nextguest");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "%s", T_ERR))
		return t_done();
	dev = open("/dev/nextguest", O_RDWR);
	if (!t_check("open_autoload", dev >= 0, "%s", T_ERR))
		return t_done();
	hostopen();
	t_flushes(f, 0);
	kernel();
	t_flushes(f, "NeXT boot");
	close(dev);
	id = modid("nextguest");
	t_check("loaded", id > 0, "not in modstat");
	if (id > 0)
		t_check("unload", moduload(id) == 0, "%s", T_ERR);
	return t_done();
}
