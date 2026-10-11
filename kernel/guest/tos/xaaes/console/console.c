/*
 * console.c -- CONSOLE.PRG: windows for the TOS and MiNT command-line
 * programs XaAES starts.  XaAES writes "program tail" to the pipe
 * u:\pipe\tosrun; each command gets a window with a VT52 screen on a
 * terminal pair (u:\pipe\ttycN), the program's handles 0-2.  Keys typed
 * in the window go to the program.  The window stays after the program
 * ends, its title saying how; the close box ends a program still
 * running.
 */
#include <gem.h>
#include <mint/osbind.h>
#include <mint/mintbind.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define	COLS	80
#define	ROWS	24
#define	NSESS	8
#define	KIND	(NAME | CLOSER | MOVER)

struct sess {
	short	wh;		/* 0: free */
	short	fd;		/* the master; -1 when the program has ended */
	long	pid;
	char	scr[ROWS][COLS + 1];
	short	cx, cy, scx, scy;
	short	esc;		/* VT52 escape: bytes seen */
	char	escc;
	short	dirty;
	char	title[64];
};

static struct sess ss[NSESS];
static short vh, cw, chh, nwin;

static void
clear(struct sess *s, int r0, int c0, int r1, int c1)
{
	int r, c;

	for (r = r0; r <= r1; r++)
		for (c = (r == r0 ? c0 : 0); c < (r == r1 ? c1 : COLS); c++)
			s->scr[r][c] = ' ';
}

static void
scroll(struct sess *s)
{
	memmove(s->scr[0], s->scr[1], (ROWS - 1) * sizeof s->scr[0]);
	memset(s->scr[ROWS - 1], ' ', COLS);
}

/* one byte of the program's output: VT52 */
static void
put(struct sess *s, unsigned char c)
{
	if (s->esc == 1) {
		s->esc = 0;
		switch (c) {
		case 'A': if (s->cy > 0) s->cy--; break;
		case 'B': if (s->cy < ROWS - 1) s->cy++; break;
		case 'C': if (s->cx < COLS - 1) s->cx++; break;
		case 'D': if (s->cx > 0) s->cx--; break;
		case 'E': clear(s, 0, 0, ROWS - 1, COLS); /* fall through */
		case 'H': s->cx = s->cy = 0; break;
		case 'I': if (s->cy > 0) s->cy--; else { memmove(s->scr[1], s->scr[0], (ROWS - 1) * sizeof s->scr[0]); memset(s->scr[0], ' ', COLS); } break;
		case 'J': clear(s, s->cy, s->cx, ROWS - 1, COLS); break;
		case 'K': clear(s, s->cy, s->cx, s->cy, COLS); break;
		case 'L': memmove(s->scr[s->cy + 1], s->scr[s->cy], (ROWS - 1 - s->cy) * sizeof s->scr[0]); memset(s->scr[s->cy], ' ', COLS); s->cx = 0; break;
		case 'M': memmove(s->scr[s->cy], s->scr[s->cy + 1], (ROWS - 1 - s->cy) * sizeof s->scr[0]); memset(s->scr[ROWS - 1], ' ', COLS); s->cx = 0; break;
		case 'd': clear(s, 0, 0, s->cy, s->cx + 1); break;
		case 'j': s->scx = s->cx; s->scy = s->cy; break;
		case 'k': s->cx = s->scx; s->cy = s->scy; break;
		case 'l': clear(s, s->cy, 0, s->cy, COLS); s->cx = 0; break;
		case 'o': clear(s, s->cy, 0, s->cy, s->cx + 1); break;
		case 'Y': s->esc = 2; break;
		case 'b': case 'c': s->esc = 4; break;
		}
		return;
	}
	if (s->esc == 2) {
		s->escc = c;
		s->esc = 3;
		return;
	}
	if (s->esc == 3) {
		s->esc = 0;
		if (s->escc - 32 >= 0 && s->escc - 32 < ROWS)
			s->cy = s->escc - 32;
		if (c - 32 >= 0 && c - 32 < COLS)
			s->cx = c - 32;
		return;
	}
	if (s->esc == 4) {
		s->esc = 0;
		return;
	}
	switch (c) {
	case 27: s->esc = 1; return;
	case '\r': s->cx = 0; return;
	case '\n': case 11: case 12:
		if (++s->cy == ROWS) {
			scroll(s);
			s->cy = ROWS - 1;
		}
		return;
	case 8: if (s->cx > 0) s->cx--; return;
	case 9: s->cx = (s->cx + 8) & ~7; if (s->cx >= COLS) s->cx = COLS - 1; return;
	case 7: case 0: return;
	}
	if (s->cx >= COLS) {
		s->cx = 0;
		if (++s->cy == ROWS) {
			scroll(s);
			s->cy = ROWS - 1;
		}
	}
	s->scr[s->cy][s->cx++] = c;
}

static int
inter(GRECT *a, GRECT *b)
{
	short x = a->g_x > b->g_x ? a->g_x : b->g_x;
	short y = a->g_y > b->g_y ? a->g_y : b->g_y;
	short x2 = a->g_x + a->g_w < b->g_x + b->g_w ? a->g_x + a->g_w : b->g_x + b->g_w;
	short y2 = a->g_y + a->g_h < b->g_y + b->g_h ? a->g_y + a->g_h : b->g_y + b->g_h;

	a->g_x = x;
	a->g_y = y;
	a->g_w = x2 - x;
	a->g_h = y2 - y;
	return a->g_w > 0 && a->g_h > 0;
}

/* the window's rectangles within area: the screen's text and cursor */
static void
draw(struct sess *s, GRECT *area)
{
	GRECT w, r;
	short pxy[4], r0;
	int row;

	wind_get(s->wh, WF_WORKXYWH, &w.g_x, &w.g_y, &w.g_w, &w.g_h);
	wind_update(BEG_UPDATE);
	graf_mouse(M_OFF, 0);
	wind_get(s->wh, WF_FIRSTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
	while (r.g_w && r.g_h) {
		if (inter(&r, area) && inter(&r, &w)) {
			pxy[0] = r.g_x;
			pxy[1] = r.g_y;
			pxy[2] = r.g_x + r.g_w - 1;
			pxy[3] = r.g_y + r.g_h - 1;
			vs_clip(vh, 1, pxy);
			vswr_mode(vh, MD_REPLACE);
			vsf_color(vh, 0);
			vsf_interior(vh, 1);
			v_bar(vh, pxy);
			r0 = (r.g_y - w.g_y) / chh;
			for (row = r0; row < ROWS && w.g_y + row * chh < r.g_y + r.g_h; row++)
				v_gtext(vh, w.g_x, w.g_y + row * chh, s->scr[row]);
			if (s->fd >= 0) {
				vswr_mode(vh, MD_XOR);
				vsf_color(vh, 1);
				pxy[0] = w.g_x + s->cx * cw;
				pxy[1] = w.g_y + s->cy * chh;
				pxy[2] = pxy[0] + cw - 1;
				pxy[3] = pxy[1] + chh - 1;
				v_bar(vh, pxy);
			}
			vs_clip(vh, 0, pxy);
		}
		wind_get(s->wh, WF_NEXTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
	}
	graf_mouse(M_ON, 0);
	wind_update(END_UPDATE);
}

static void
drawall(struct sess *s)
{
	GRECT a = { 0, 0, 32767, 32767 };

	draw(s, &a);
	s->dirty = 0;
}

static struct sess *
bywin(short wh)
{
	int i;

	for (i = 0; i < NSESS; i++)
		if (ss[i].wh == wh && wh > 0)
			return &ss[i];
	return 0;
}

/* a window and a terminal pair for "program tail", the program started on it */
static void
start(char *cmd)
{
	char prog[256], tail[128], name[40], dir[256], odir[256], *p, *t;
	short x, y, w, h, i, slave, save[3], odrv;
	struct sess *s = 0;

	while (*cmd == ' ')
		cmd++;
	for (t = cmd; *t && *t != ' ' && *t != '\r' && *t != '\n'; t++)
		;
	if (t == cmd)
		return;
	memcpy(prog, cmd, t - cmd);
	prog[t - cmd] = 0;
	while (*t == ' ')
		t++;
	strncpy(tail + 1, t, 125);
	tail[126] = 0;
	for (p = tail + 1; *p && *p != '\r' && *p != '\n'; p++)
		;
	*p = 0;
	tail[0] = strlen(tail + 1);
	for (i = 0; i < NSESS && ss[i].wh; i++)
		;
	if (i == NSESS)
		return;
	s = &ss[i];
	memset(s, 0, sizeof *s);
	for (i = 0; i < ROWS; i++)
		memset(s->scr[i], ' ', COLS);
	p = strrchr(prog, '\\');
	strncpy(name, p ? p + 1 : prog, sizeof name - 1);
	sprintf(s->title, " %s ", name);
	sprintf(dir, "u:\\pipe\\ttyc%d", (int)(s - ss));
	s->fd = Fcreate(dir, 0);
	if (s->fd < 0)
		return;
	slave = Fopen(dir, O_RDWR);
	if (slave < 0) {
		Fclose(s->fd);
		return;
	}
	Fcntl(s->fd, O_NDELAY | O_RDWR, F_SETFL);
	/* the program's directory is its own */
	strcpy(dir, prog);
	if ((p = strrchr(dir, '\\')) != 0)
		p[1] = 0;
	odrv = Dgetdrv();
	Dgetpath(odir, 0);
	if (dir[0] && dir[1] == ':') {
		Dsetdrv((dir[0] & ~32) - 'A');
		Dsetpath(dir + 2);
	}
	for (i = 0; i < 3; i++) {
		save[i] = Fdup(i);
		Fforce(i, slave);
	}
	s->pid = Pexec(100, prog, tail, 0L);
	for (i = 0; i < 3; i++) {
		Fforce(i, save[i]);
		Fclose(save[i]);
	}
	Fclose(slave);
	Dsetdrv(odrv);
	Dsetpath(odir);
	if (s->pid < 0) {
		sprintf(tail, "%s: error %ld\r\n", name, s->pid);
		for (p = tail; *p; p++)
			put(s, *p);
	}
	wind_calc(WC_BORDER, KIND, 0, 0, COLS * cw, ROWS * chh, &x, &y, &w, &h);
	s->wh = wind_create(KIND, 0, 0, w, h);
	if (s->wh <= 0) {
		Fclose(s->fd);
		s->wh = 0;
		return;
	}
	wind_set_str(s->wh, WF_NAME, s->title);
	nwin++;
	wind_open(s->wh, 20 + 24 * (s - ss), 40 + 24 * (s - ss), w, h);
}

/* the program's output to its screen; at its end the title says so */
static void
pump(struct sess *s)
{
	char b[512];
	long n, r;
	int i;

	if (s->fd < 0)
		return;
	while ((n = Fread(s->fd, sizeof b, b)) > 0) {
		for (i = 0; i < n; i++)
			put(s, b[i]);
		s->dirty = 1;
	}
	if (n == 0) {
		Fclose(s->fd);
		s->fd = -1;
		r = s->pid > 0 ? Pwaitpid(s->pid, 1, 0L) : 0;
		sprintf(s->title + strlen(s->title), "- exit %ld ", r > 0 ? (r & 0xffff) : 0);
		wind_set_str(s->wh, WF_NAME, s->title);
		s->dirty = 1;
	}
	if (s->dirty)
		drawall(s);
}

static void
end(struct sess *s)
{
	if (s->fd >= 0) {
		if (s->pid > 0)
			Pkill(s->pid, SIGHUP);
		Fclose(s->fd);
	}
	wind_close(s->wh);
	wind_delete(s->wh);
	s->wh = 0;
	nwin--;
}

/* a key to the program: VT52's cursor keys for the arrows */
static void
key(struct sess *s, short k)
{
	char b[2];
	int n = 1;

	if (!s || s->fd < 0)
		return;
	b[0] = k & 0xff;
	if (!b[0]) {
		b[0] = 27;
		switch ((k >> 8) & 0xff) {
		case 0x48: b[1] = 'A'; break;
		case 0x50: b[1] = 'B'; break;
		case 0x4d: b[1] = 'C'; break;
		case 0x4b: b[1] = 'D'; break;
		default: return;
		}
		n = 2;
	}
	Fwrite(s->fd, n, b);
}

int
main(void)
{
	short msg[8], ev, d, k, i, top, work_in[11], work_out[57];
	char cmd[512];
	long rfd, n;
	struct sess *s;

	if (appl_init() < 0)
		return 1;
	vh = graf_handle(&cw, &chh, &d, &d);
	for (i = 0; i < 10; i++)
		work_in[i] = 1;
	work_in[10] = 2;
	v_opnvwk(work_in, &vh, work_out);
	vst_alignment(vh, 0, 5, &d, &d);
	menu_register(gl_apid, "  Console");
	rfd = Fcreate("u:\\pipe\\tosrun", 0);
	if (rfd < 0 || vh <= 0) {
		appl_exit();
		return 1;
	}
	Fcntl((short)rfd, O_NDELAY | O_RDWR, F_SETFL);
	for (;;) {
		ev = evnt_multi(MU_MESAG | MU_TIMER | MU_KEYBD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		    0, 0, msg, 40, &d, &d, &d, &d, &k, &d);
		if (ev & MU_MESAG) {
			s = bywin(msg[3]);
			switch (msg[0]) {
			case WM_REDRAW:
				if (s)
					draw(s, (GRECT *)&msg[4]);
				break;
			case WM_TOPPED:
				wind_set(msg[3], WF_TOP, msg[3], 0, 0, 0);
				break;
			case WM_MOVED:
				wind_set(msg[3], WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
				break;
			case WM_CLOSED:
				if (s)
					end(s);
				break;
			case AP_TERM:
				for (i = 0; i < NSESS; i++)
					if (ss[i].wh)
						end(&ss[i]);
				v_clsvwk(vh);
				appl_exit();
				return 0;
			}
		}
		if (ev & MU_KEYBD) {
			wind_get(0, WF_TOP, &top, &d, &d, &d);
			key(bywin(top), k);
		}
		if ((n = Fread((short)rfd, sizeof cmd - 1, cmd)) > 0) {
			cmd[n] = 0;
			start(cmd);
		}
		for (i = 0; i < NSESS; i++)
			if (ss[i].wh)
				pump(&ss[i]);
	}
}
