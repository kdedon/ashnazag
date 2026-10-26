/*
 * t_lisa: Lisa OS from the files of the user's ProFile image on SCSI disk 6,
 * loaded by startlisa: the loader reads its files from the folder, the screen
 * shows text, nothing faulted, and in steady state the guest takes no faults
 * on hardware pages.  "t_lisa office", run next, boots the folder the first
 * run left and makes, saves, renames and deletes a LisaWrite document, and
 * saves and reopens a LisaCalc paper.
 * Skips without the image.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
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
#define	ROOT	MNT "/Lisa"		/* the environment's folder; the disk's writes are dropped */
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
	int fd, n, prog;

	if ((fd = open(LOG, O_RDONLY)) < 0)
		return;
	do {
		lseek(fd, off, 0);
		n = read(fd, buf, sizeof buf - 1);
		buf[n > 0 ? n : 0] = 0;
		for (l = buf; (e = strchr(l, '\n')) != 0; l = e + 1) {
			*e = 0;
			prog = !strncmp(l, "lisa: pc ", 9) || !strncmp(l, "lisa: screen ", 13) || !strncmp(l, "lisa: drew ", 11);
			if (!prog || t_now_ms() - last >= 20000) {
				if (prog)
					last = t_now_ms();
				t_info("log", "%s", l);
			}
		}
		off += l - buf;
	} while (n == sizeof buf - 1 && l > buf);
	close(fd);
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

/*
 * The pointer's top left on the host screen: what changes when it moves 1
 * right and down, alone, or besides what changes by itself (a title's
 * blinking caret).
 */
static int
pointer(x, y)
	int *x, *y;
{
	char req[64];
	int x1, y1, k, i, b[4], ig[4] = { 0, 0, -1, -1 };

	for (k = 0; k < 10; k++) {
		host("shot lp0");
		host("move 1 1");
		nap(700);
		host("shot lp1");
		sprintf(req, "box lp0 lp1 0 %d %d %d %d", ig[0], ig[1], ig[2], ig[3]);
		if (host(req) && sscanf(hrep, "%d %d %d %d", x, y, &x1, &y1) == 4 && x1 - *x < 40 &&
		    y1 - *y < 40)
			return 1;
		/* what changes with no input over a second */
		for (i = 0; i < 4; i++) {
			nap(300);
			host(i & 1 ? "shot lp1" : "shot lp0");
			if (i && host("box lp0 lp1") && sscanf(hrep, "%d %d %d %d", &b[0], &b[1], &b[2], &b[3]) == 4) {
				if (ig[2] < 0)
					memcpy(ig, b, sizeof ig);
				ig[0] = b[0] < ig[0] ? b[0] : ig[0];
				ig[1] = b[1] < ig[1] ? b[1] : ig[1];
				ig[2] = b[2] > ig[2] ? b[2] : ig[2];
				ig[3] = b[3] > ig[3] ? b[3] : ig[3];
			}
		}
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

/* host pixels per 16 move units on each axis, learned from the moves made */
static int gx, gy;

/* a move of d pixels at gain g; half of it until g is known */
static int
units(d, g)
	int d, g;
{
	int u;

	if (d >= -2 && d <= 2)
		return 0;
	if (!g)
		return step(d);
	u = d * 16 / g;
	return u ? u : d > 0 ? 1 : -1;
}

/* the pointer to host x, y, whatever the scaling */
static int
moveto(x, y)
	int x, y;
{
	char req[40];
	int k, px, py, mx = 0, my = 0, ox = 0, oy = 0;

	for (k = 0; k < 30 && pointer(&px, &py); k++) {
		if (k && (mx >= 4 || mx <= -4) && (px - ox) * mx > 0)
			gx = (px - ox) * 16 / mx >= 4 ? (px - ox) * 16 / mx : gx;
		if (k && (my >= 4 || my <= -4) && (py - oy) * my > 0)
			gy = (py - oy) * 16 / my >= 4 ? (py - oy) * 16 / my : gy;
		if (abs(x - px) <= 2 && abs(y - py) <= 2)
			return 1;
		mx = units(x - px, gx);
		my = units(y - py, gy);
		ox = px;
		oy = py;
		sprintf(req, "move %d %d 40 1", mx, my);
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
 * host's width and centred: the pointer moves, no micro diskette dialog
 * shows (a click where its Cancel would be changes nothing), a click on the volume's icon and typing renames it,
 * and Apple-O (a beep) keeps it running.
 */
#define	LX(x)	((w - sw) / 2 + (x) * sw / 720)	/* a Lisa screen point on the host's */
#define	LY(y)	(top + (y) * sh / 364)

static int w, h, sw, sh, top;		/* the host screen, the Lisa's on it */
static void office(), mandel(), watch(), menu(), calc();
static int offmode;			/* the office run: files made, renamed, deleted */
static int offpid, offgone;		/* its startlisa; gone after the power switch */

static void
input()
{
	char *log = readlog(), *s, *e;
	int x, y;

	if ((s = strstr(log, "lisa: display ")) != 0)
		sscanf(s, "lisa: display %dx%d, the screen at %dx%d", &w, &h, &sw, &sh);
	t_check("display", w > 0 && sw == (w < 720 ? w : 720) && sh == (h < 364 ? h : 364), "%dx%d, the screen at %dx%d", w, h, sw, sh);
	if (w <= 0)
		return;
	top = (h - sh) / 2;
	if (offmode) {
		office();
		return;
	}
	t_check("mouse", pointer(&x, &y), "the pointer did not move");
	/* screen drawing and pointer latency, with the PCs while the pointer moves */
	host("prof 4 lisa_point");
	host("glide 100 2 1 40");
	nap(5000);
	log = readlog();
	if ((s = strstr(log, "lisa: full-screen draw ")) != 0 && (e = strchr(s, '\n')) != 0)
		t_info("draw", "%.*s", (int)(e - s - 6), s + 6);
	for (s = log, e = 0; (s = strstr(s, "input to screen ")) != 0; s++)
		e = s;
	if (e)
		t_info("pointer", "%.*s", (int)strcspn(e, "\n"), e);
	if (t_check("to_cancel", moveto(LX(596), LY(70)), "pointer not at the dialog's Cancel")) {
		host("shot li0");
		host("click 1");
		nap(5000);
		host("shot li1");
		s = changed("li0", "li1");
		t_check("no_dialog", strcmp(s, "none") == 0, "a dialog closed: %s changed", s);
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

/* black at Lisa point x, y of shot name */
static int
dark(name, x, y)
	char *name;
	int x, y;
{
	char req[64];

	sprintf(req, "pix %s %d %d", name, LX(x), LY(y));
	return host(req) && !strcmp(hrep, "000000");
}

/* the system error alert's Stop sign: a black octagon, its corners and sides white */
static int
stopsign(name)
	char *name;
{
	static int in[][2] = { { 75, 93 }, { 156, 93 }, { 116, 68 }, { 116, 120 }, { 90, 104 }, { 142, 82 } };
	static int out[][2] = { { 66, 62 }, { 166, 62 }, { 66, 126 }, { 166, 126 }, { 60, 93 }, { 172, 93 } };
	int i;

	for (i = 0; i < 6; i++)
		if (!dark(name, in[i][0], in[i][1]) || dark(name, out[i][0], out[i][1]))
			return 0;
	return 1;
}

/*
 * LisaMandelbrotPort, a QuickPort program in floating point, draws its
 * set to the end: the screen stops changing, no system error alert shows
 * and the environment runs on.
 */
static void
mandel(pid)
	int pid;
{
	char req[40];
	int k, same = 0, stop = 0;

	moveto(LX(300), LY(200));
	host("click 1");
	nap(3000);
	moveto(LX(549), LY(140));
	host("click 2");
	for (k = 0; k < 60 && kill(pid, 0) == 0 && same < 3 && !stop; k++) {
		watch(10000);
		sprintf(req, "shot lm%d", k);
		host(req);
		stop = stopsign(req + 5);
		if (k > 0) {
			sprintf(req, "lm%d", k - 1);
			sprintf(req + 20, "lm%d", k);
			same = strcmp(changed(req, req + 20), "none") == 0 ? same + 1 : 0;
		}
	}
	if (!stop)
		watch(30000);
	shot("lisa_mandel", 1);
	stop |= stopsign("lisa_mandel");
	if (!t_check("mandel", kill(pid, 0) == 0 && same >= 3 && !stop, "%s after %d s",
	    stop ? "system error alert" : kill(pid, 0) ? "exited" : "still drawing", k * 10))
		return;
	/* Save & Put Away: the office run boots this desktop */
	menu(68);
	watch(30000);
	host("shot lm_away");
}

/* how often host file name holds s; -1 no such file */
static int
holds(name, s)
	char *name, *s;
{
	static char b[65536];
	char p[300];
	int fd, k, hit = 0, l = strlen(s), i;

	sprintf(p, "%s/%s", ROOT, name);
	if ((fd = open(p, O_RDONLY)) < 0)
		return -1;
	while ((k = read(fd, b, sizeof b)) > 0)
		for (i = 0; i + l <= k; i++)
			if (!memcmp(b + i, s, l))
				hit++;
	close(fd);
	return hit;
}

#define	NF	400
static char *fl[NF];			/* the folder's names before the tear-off */
static int nfl;

/* the folder's names into fl (keep) or, not in fl, into out; how many */
static int
names(keep, out, max)
	int keep, max;
	char **out;
{
	struct dirent *de;
	DIR *d = opendir(ROOT);
	int n = 0, i;

	while (d && (de = readdir(d)) != 0 && n < (keep ? NF : max)) {
		if (keep) {
			fl[n++] = strdup(de->d_name);
			continue;
		}
		for (i = 0; i < nfl && strcmp(fl[i], de->d_name); i++)
			;
		if (i == nfl)
			out[n++] = strdup(de->d_name);
	}
	if (d)
		closedir(d);
	if (keep)
		nfl = n;
	return n;
}

/* a typed key per character: lower case and space */
static void
type(s)
	char *s;
{
	char req[20];

	for (; *s; s++) {
		if (*s == ' ')
			strcpy(req, "key spc");
		else
			sprintf(req, "key %c", *s);
		host(req);
	}
}

static void watch();

/* the environment ended as a user ends it: it shuts the OS down and exits within secs; 1 it did */
static int
stop(pid, sig, secs, st)
	int pid, sig, secs, *st;
{
	long t0 = t_now_ms();

	if (sig)
		kill(pid, sig);
	while (t_now_ms() - t0 < secs * 1000L) {
		follow();
		if (waitpid(pid, st, WNOHANG) == pid)
			return 1;
		sleep(1);
	}
	kill(pid, SIGKILL);
	waitpid(pid, st, 0);
	return 0;
}

/* the File/Print menu's item at Lisa line y, the button held; pressed again while the desktop is busy */
static void
menu(y)
	int y;
{
	char req[20];
	int k, x0, y0, x1, y1;

	for (k = 0; k < 12; k++) {
		moveto(LX(103), LY(7));
		host("shot lwp");
		host("button 1 500");
		nap(1500);
		host("shot lwq");
		if (sscanf(changed("lwp", "lwq"), "%d %d %d %d", &x0, &y0, &x1, &y1) == 4 && y0 <= LY(12) &&
		    y1 - y0 > LY(60) - LY(7))
			break;
		host("button 0");
		watch(5000);
	}
	moveto(LX(103), LY(y));
	sprintf(req, "shot lwm%d", y);
	host(req);
	host("button 0");
}

/* ms of waiting, the log followed */
static void
watch(ms)
	int ms;
{
	for (; ms > 0; ms -= 2000) {
		nap(ms < 2000 ? ms : 2000);
		follow();
	}
}

/*
 * Office on the folder: LisaWrite stationery torn off and titled becomes a
 * host file on its first Open, with the title in its sidecar; text saved
 * by LisaWrite is in the file; a new title replaces the old one; the
 * Wastebasket keeps only its last document, deleting the one before, and
 * the files the OS let go are gone after power off.
 */
static void
office()
{
	char *nw[40], doc[40], doc2[40], p[100], p2[100], list[400], *bad;
	int n, k, j, t, m, figs = 0, scrap = 0, st = -1;

	names(1, (char **)0, 0);
	moveto(LX(189), LY(144));
	host("click 1");
	nap(3000);
	host("click 1");
	nap(3000);
	menu(127);			/* Tear Off Stationery */
	watch(20000);
	type("hello");
	watch(5000);
	/* a title typed early ends the selection, one still open ends at the first click: select the document again */
	moveto(LX(157), LY(140));
	host("click 1");
	nap(3000);
	host("click 1");
	nap(3000);
	menu(82);			/* Open */
	watch(90000);
	host("shot lw0");
	/* the new document {DnTm}, with its figures and a scrap of its own */
	doc[0] = list[0] = 0;
	n = names(0, nw, 40);
	for (k = 0; k < n; k++) {
		if (strlen(list) + strlen(nw[k]) < sizeof list - 2)
			strcat(strcat(list, " "), nw[k]);
		if (sscanf(nw[k], "{D%dT%d%1s", &j, &t, p) == 3 && !strcmp(p, "}") && !doc[0])
			strcpy(doc, nw[k]);
		if (strstr(nw[k], "Scrap") && nw[k][0] != '.')
			scrap++;
	}
	if (doc[0])
		for (k = 0; k < n; k++)
			if (!strncmp(nw[k], doc, strlen(doc)) && !strcmp(nw[k] + strlen(doc), ".Figures"))
				figs++;
	if (!t_check("tear_off", doc[0] && figs && scrap, "new files:%s", list))
		return;
	sprintf(p, ".%s", doc);
	type("lisa text");
	nap(3000);
	host("shot lw1");
	menu(68);			/* Save & Put Away */
	watch(30000);
	host("shot lw2");
	moveto(LX(157), LY(140));
	host("button 1 500");
	moveto(LX(80), LY(310));
	host("button 0");
	watch(20000);
	t_check("saved", holds(doc, "lisa text") > 0, "%s: no text", doc);
	/* the save's copy took the old file's place, and the old file went */
	n = names(0, nw, 40);
	for (bad = 0, k = 0; k < n; k++)
		if (strstr(nw[k], "%z"))
			bad = nw[k];
	t_check("no_strays", !bad, "%s left", bad);
	/* the desktop writes a document's label when it goes into the Wastebasket; the pointer is still on the document */
	host("button 1 500");
	moveto(LX(418), LY(337));
	host("button 0");
	watch(20000);
	t_check("title", holds(p, "68656c6c6f") > 0, "%s: no title", p);
	/* a second document, opened and put away, then renamed */
	moveto(LX(600), LY(220));
	host("click 1");
	nap(3000);
	host("shot lt0");
	moveto(LX(189), LY(144));
	host("click 1");
	nap(3000);
	menu(127);
	watch(20000);
	menu(82);
	watch(90000);
	menu(68);
	watch(30000);
	/* its icon, wherever it landed: what changed with nothing selected and the pointer parked */
	moveto(LX(600), LY(220));
	host("click 1");
	nap(3000);
	host("shot lt1");
	sprintf(p2, "box lt0 lt1 0 %d %d %d %d", LX(600) - 24, LY(220) - 24, LX(600) + 24, LY(220) + 24);
	if (sscanf(host(p2) ? hrep : "?", "%d %d %d %d", &j, &t, &n, &k) != 4)
		j = t = n = k = 0;
	j = (j + n) / 2;
	t += (k - t) / 3;
	doc2[0] = 0;
	n = names(0, nw, 40);
	for (k = 0; k < n; k++)
		if ((m = 0, sscanf(nw[k], "{D%*dT%*d}%n", &m), m) && !nw[k][m] && strcmp(nw[k], doc) && !doc2[0])
			strcpy(doc2, nw[k]);
	sprintf(p2, ".%s", doc2);
	moveto(j, t);
	host("click 1");
	nap(3000);
	type("world");
	watch(3000);
	host("shot lw3");
	moveto(j, t);
	host("button 1 500");
	moveto(LX(418), LY(337));
	host("button 0");
	watch(20000);
	t_check("rename", doc2[0] && holds(p2, "776f726c64") > 0, "%s: no new title", doc2[0] ? p2 : "no second document");
	calc();
	/* the power switch shuts down cleanly */
	host("key power");
	k = stop(offpid, 0, 150, &st);
	host("shot lw4");
	t_check("power_off", k && st == 0, "%s (status 0x%x)", k ? "exited" : "still running after 150 s", st);
	sprintf(p2, "%sNEW.Figures", doc);
	strcat(strcpy(list, doc), ".Figures");
	offgone = 1;
	k = holds(doc, "") >= 0 ? 1 : holds(p, "") >= 0 ? 2 : holds(list, "") >= 0 ? 3 : holds(p2, "") >= 0 ? 4 :
	    !doc2[0] || holds(doc2, "") < 0 ? 5 : 0;
	t_check("delete", k == 0, k == 5 ? "%s gone from the Wastebasket" : "%s still there",
	    k == 1 ? doc : k == 2 ? p : k == 3 ? list : k == 4 ? p2 : doc2);
}

/* pixels of column A, rows 2 and 3, dark in shot y and not in x; -1 unknown */
static int
cells(x, y)
	char *x, *y;
{
	char req[80];
	int n;

	sprintf(req, "glyph %s %s %d %d %d %d", x, y, LX(36), LY(89), LX(100), LY(112));
	return host(req) && sscanf(hrep, "glyph %*d %d", &n) == 1 ? n : -1;
}

/*
 * LisaCalc: a paper torn off its stationery, two cells typed, saved and
 * put away holds them; opened again, the cells show as typed.
 */
static void
calc()
{
	char *nw[40], doc[40], list[400];
	int n, k, j, a, b, c, d;

	names(1, (char **)0, 0);
	moveto(LX(71), LY(100));
	host("click 1");
	nap(3000);
	host("click 1");
	nap(3000);
	menu(127);			/* Tear Off Stationery */
	watch(20000);
	menu(82);			/* Open */
	watch(90000);
	host("shot lce");
	moveto(LX(68), LY(83));
	host("click 1");
	nap(2000);
	type("hello");
	host("key ret");
	type("4321");
	host("key ret");
	watch(5000);
	host("shot lc1");
	menu(68);			/* Save & Put Away */
	watch(30000);
	host("shot lc2");
	doc[0] = list[0] = 0;
	n = names(0, nw, 40);
	for (k = 0; k < n; k++) {
		if (strlen(list) + strlen(nw[k]) < sizeof list - 2)
			strcat(strcat(list, " "), nw[k]);
		if ((j = 0, sscanf(nw[k], "{D%*dT%*d}%n", &j), j) && !nw[k][j] && !doc[0])
			strcpy(doc, nw[k]);
	}
	if (!t_check("calc_saved", doc[0] && holds(doc, "hello") > 0, "%s:%s", doc[0] ? "no cells in it" : "no document", list)) {
		moveto(LX(596), LY(70));	/* the alert's Cancel */
		host("click 1");
		watch(5000);
		return;
	}
	/* put away, it stays selected */
	menu(82);
	watch(90000);
	host("shot lc4");
	/* column A, rows 2 and 3: lit by the typing, the same when reopened */
	a = cells("lce", "lc1");
	b = cells("lc1", "lc4");
	c = cells("lc4", "lc1");
	d = a > 20 && !b && !c;
	t_check("calc_reopen", d, "%d pixels typed, %d and %d differ reopened", a, b, c);
	menu(68);
	watch(30000);
}

static void
boot()
{
	struct mod_mreg reg;
	char *log, *s, *e;
	int pid, st = 0, alive, mj = 68, gone, osf;
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
	    system("/sbin/mount -F ufs " DEV " " MNT) != 0) {
		t_skip("boot", "no Lisa volume or startlisa");
		return;
	}
	if (access(ROOT "/lisa.rom", 0) != 0) {
		system("/sbin/umount " MNT);
		t_skip("boot", "no Lisa ROM");
		return;
	}
	if (modload("/etc/conf/mod.d/dsuser") < 0 && errno != EEXIST)
		t_info("display", "dsuser: %s", T_ERR);
	hostopen();
	unlink(LOG);
	t_check("folder", access(ROOT "/.boot", 0) == 0 && access(ROOT "/system.os", 0) == 0, "%s", T_ERR);
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
	/* the OS's own reads may still reach the ProFile through the loader */
	log = await(pid, "lisa: loader file system.os from host", 60) ? readlog() : "";
	e = strstr(log, "lisa: loader file system.os");
	s = strstr(log, "read from the volume");
	t_check("loader_files", e && strstr(log, "lisa: loader reads its files by host call") && (!s || s > e),
	    "the loader read the ProFile");
	t_check("screen_text", await(pid, "pixels lit", RUNS), "nothing drawn");
	osf = await(pid, "lisa: OS files from host", 60);
	/* the loader's code is patched as it is read, not searched for in RAM on every read */
	if (osf) {
		all = -1;
		if ((s = strstr(readlog(), "lisa: OS files from host: s-file ")) != 0)
			sscanf(s, "%*[^,], %ld", &all);
		t_check("ram_searches", all == 0, "%ld searches of the Lisa's RAM for code", all);
	}
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
	if (!offmode && kill(pid, 0) == 0) {
		long b = t_busy(30000L);

		/* an application waiting for input yields to nobody on every pass */
		t_check("idle_busy", b >= 0 && b < 100, "%ld.%ld%% busy at the desktop", b / 10, b % 10);
	}
	/* the office run boots the folder the first run left */
	if (offmode)
		t_check("reboot", kill(pid, 0) == 0 && lit > 500 && strstr(log, "lisa: catalog from host") && !strstr(log, "hardware access"),
		    "%s", strstr(log, "hardware access") ? strstr(log, "hardware access") : "no desktop");
	offpid = pid;
	if (hfd >= 0 && kill(pid, 0) == 0)
		input();
	if (!offmode && hfd >= 0 && kill(pid, 0) == 0)
		mandel(pid);
	if (offmode) {
		if (!offgone)
			stop(pid, SIGTERM, 150, &st);
		follow();
		system("/sbin/umount " MNT);
		return;
	}
	gone = waitpid(pid, &st, WNOHANG) != 0;
	alive = !gone;
	/* ended from outside, it shuts the OS down through the power switch */
	if (alive)
		t_check("power_off", stop(pid, SIGTERM, 150, &st) && st == 0 && strstr(readlog(), "lisa: power off"),
		    "status 0x%x", st);
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
	t_check("os_files", osf && !strstr(log, "in no extent") && !strstr(log, "no file's"),
	    "%s", osf ? "a page outside the files" : "the OS's file data came from the ProFile");
	{
		struct dirent *de;
		DIR *d = opendir(ROOT);
		char *n = 0;

		while (d && (de = readdir(d)) != 0)
			if (strlen(de->d_name) > 5 && !strcmp(de->d_name + strlen(de->d_name) - 5, ".dc42"))
				n = "a disk image in the folder";
		if (d)
			closedir(d);
		t_check("no_profile", !n && !strstr(log, "lisa: ProFile block") && strstr(log, "lisa: catalog from host"),
		    "%s", n ? n : strstr(log, "lisa: ProFile block") ? "a ProFile transfer" : "no catalog call");
	}
	lit = steady(log, " a-line ", &all);
	t_check("aline_user", lit == 0, "%ld A-lines through supervisor mode in 2 s", lit);
	lit = steady(log, " faults ", &all);
	t_check("native", lit == 0, "%ld hardware-page faults in 2 s", lit);
	t_check("no_hw_access", !strstr(log, "hardware access"), "the guest reached a hardware page");
	{
		unsigned long blocks = 0, pushes = 0;
		char *q, *b, *p;

		for (q = log; (q = strstr(q, "lisa: remaps ")) != 0; q++)
			if ((b = strstr(q, " blocks ")) != 0 && (p = strstr(b, " pushes ")) != 0) {
				blocks = strtoul(b + 8, (char **)0, 10);
				pushes = strtoul(p + 8, (char **)0, 10);
			}
		/* every block read may be code: each one pushes the caches */
		t_check("code_pushed", blocks > 0 && pushes >= blocks, "%lu cache pushes for %lu blocks read", pushes, blocks);
	}
	/* bus errors are the OS's own segment loads */
	t_check("fault_free", alive && all == 0 && !strstr(log, "startlisa:"),
	    "%s (status 0x%x), %ld hardware-page faults; last line: %s", alive ? "running" : "exited", st, all, s ? s : "");
	system("/sbin/umount " MNT);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	offmode = argc > 1 && !strcmp(argv[1], "office");
	t_init(offmode ? "lisaoffice" : "lisa", offmode ? 2300 : 1300);
	boot();
	return t_done();
}
