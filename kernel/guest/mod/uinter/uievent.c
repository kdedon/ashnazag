/*
 * uievent.c -- the layer's input: the event queue behind GetOSEvent,
 * keyboard and mouse from the display session, and the tick work in
 * Mac memory (Ticks, Time, mouse and key globals, the cursor).
 *
 * Input arrives at interrupt level and touches only the queue and the
 * kernel's copy of the state.  Mac memory is written by ui_update, in
 * the task itself, as its tick signal is delivered.
 *
 * K&R C.
 */

#include "uinter.h"
#include "sys/poll.h"
#include "dsio.h"
#define	printf	ds_printf	/* ds.h declares it int */
#include "ds.h"
#undef	printf

#define	NEV		32
#define	MACEPOCH	2082844800L	/* 1904 to 1970 */
#define	ADBKBD		(2L << 16)	/* keyboard address in the message */

/* Mac low memory */
#define	LM_TICKS	0x16a
#define	LM_MBSTATE	0x172
#define	LM_KEYMAP	0x174
#define	LM_KEYTHRESH	0x18e		/* and KeyRepThresh */
#define	LM_TIME		0x20c
#define	LM_MTEMP	0x828		/* MTemp, RawMouse, Mouse */
#define	LM_CRSRPIN	0x834
#define	LM_THECRSR	0x844		/* data, mask */
#define	LM_CRSRVIS	0x8cc		/* Vis, Busy, New, Couple, State.w, Obscure */
#define	LM_MMASK	0x8d6		/* MouseMask, MouseOffset */
#define	LM_SHIELD	0xd4c
#define	LM_MICKEY	0xd6a		/* MickeyBytes */

/* ui page */
#define	UP_MX		0x00
#define	UP_CX		0x08
#define	UP_HPX		0x20
#define	UP_DATA		0x4c
#define	UP_BUTTON	0x458

#define	IN_MOVED	4	/* deltas pending */
#define	IN_SETPOS	32	/* in_mh, in_mv to Mac memory as they are */
#define	IN_KEYS		8
#define	IN_BTN		16

struct uiev {
	short		what;
	unsigned short	mods;
	long		msg, when, where;
};

static struct uiev ev_q[NEV];
static int ev_first, ev_n, ev_wait;
static short ev_rect[4];		/* the sleeper's mouse rect */

static int in_flags;
static unsigned char in_keys[16];	/* KeyMap order */
static unsigned char in_raw[16];	/* ADB codes down */
static int in_btn;
static int in_btnw = -1;		/* in_btn as last written to the Mac */
static int in_mh, in_mv;		/* Mouse, as last read or moved */
static int in_dh, in_dv;		/* deltas not yet in MTemp */
static short in_pin[4];			/* top, left, bottom, right */
static int in_down = -1;		/* repeating key */
static long in_rpt, in_thresh = 20, in_rate = 4;
static long in_sec;

/* where the cursor was drawn and what it covers */
static int cur_h, cur_v, cur_hx, cur_hy, cur_d = 1;
static int cur_on;			/* cur_sv holds what is under it */
static unsigned char cur_sv[16 * 16 * 4];

long	uin_mouse;		/* v << 16 | h */
long	uin_nev, uin_nget, uin_nkey, uin_nbtn, uin_ncur, uin_ndown, uin_nlost;
long	uin_nbtnin;		/* button changes from the device */

extern timestruc_t hrestime;
extern int ui_scrgeom();

#define	KEY(c)	(in_keys[(c) >> 3] & (1 << ((c) & 7)))
#define	RAW(c)	(in_raw[(c) >> 3] & (1 << ((c) & 7)))

static int
ui_mods()
{
	int m = in_btn ? 0 : 0x80;

	if (KEY(0x37))
		m |= 0x100;
	if (KEY(0x38))
		m |= 0x200;
	if (KEY(0x39))
		m |= 0x400;
	if (KEY(0x3a))
		m |= 0x800;
	if (KEY(0x3b))
		m |= 0x1000;
	return m;
}

/* KCHR: modifier byte -> table, table[key] -> character */
static int
ui_char(c, m)
	int c, m;
{
	unsigned char *k = ui.l_kchr;
	int t;

	if (ui.l_kchrlen < 260)
		return 0;
	t = k[2 + ((m >> 8) & 0xff)];
	if (t >= G16(k + 258) || 260 + 128 * (t + 1) > ui.l_kchrlen)
		return 0;
	return k[260 + 128 * t + c];
}

/* append; a full queue loses its oldest event.  Interrupts off. */
static void
ev_add(what, msg, when, where, mods)
	int what, mods;
	long msg, when, where;
{
	struct uiev *e;

	if (ev_n == NEV) {
		ev_first = (ev_first + 1) % NEV;
		ev_n--;
		uin_nlost++;
	}
	e = &ev_q[(ev_first + ev_n++) % NEV];
	e->what = what;
	e->msg = msg;
	e->when = when;
	e->where = where;
	e->mods = mods;
	uin_nev++;
	if (ev_wait)
		wakeup((caddr_t)ev_q);
}

/* an input event, if the layer takes input and SysEvtMask has it */
static void
ev_input(what, msg)
	int what;
	long msg;
{
	if ((in_flags & IN_DEV) && (ui.l_evmask & (1 << what)))
		ev_add(what, msg, (long)lbolt, uin_mouse, ui_mods());
}

static int
ui_isnew(r)
	short *r;
{
	return r[2] > r[0] && r[3] > r[1] &&
	    (in_mv < r[0] || in_mh < r[1] || in_mv >= r[2] || in_mh >= r[3]);
}

static void
ui_key(c, down)
	int c, down;
{
	if (c == 0x7f)			/* power */
		return;
	if (c == 0x38)			/* left and right share a code */
		down = RAW(0x38) || RAW(0x7b);
	else if (c == 0x3a)
		down = RAW(0x3a) || RAW(0x7c);
	else if (c == 0x3b)
		down = RAW(0x36) || RAW(0x7d);
	if (down)
		in_keys[c >> 3] |= 1 << (c & 7);
	else
		in_keys[c >> 3] &= ~(1 << (c & 7));
	in_flags |= IN_KEYS;
	if (c >= 0x37 && c <= 0x3b)
		return;			/* modifiers post nothing */
	if (down) {
		in_down = c;
		in_rpt = lbolt + in_thresh;
	} else if (in_down == c)
		in_down = -1;
	ev_input(down ? 3 : 4, ADBKBD | c << 8 | ui_char(c, ui_mods()));
}

/* a mouse delta: pending for the tick, and a provisional position meanwhile */
static void
ui_move(dh, dv)
	long dh, dv;
{
	in_dh += dh;
	in_dv += dv;
	in_mh += dh;
	in_mv += dv;
	if (in_mh < in_pin[1])
		in_mh = in_pin[1];
	if (in_mh >= in_pin[3])
		in_mh = in_pin[3] - 1;
	if (in_mv < in_pin[0])
		in_mv = in_pin[0];
	if (in_mv >= in_pin[2])
		in_mv = in_pin[2] - 1;
	uin_mouse = (long)in_mv << 16 | (in_mh & 0xffff);
	in_flags |= IN_MOVED;
	if (ev_wait && ui_isnew(ev_rect))
		wakeup((caddr_t)ev_q);
}

/* ADB code to the Mac's virtual key code, as the standard KMAP does */
static int
ui_vkey(c)
	int c;
{
	switch (c) {
	case 0x36:	return 0x3b;	/* control */
	case 0x3b:	return 0x7b;	/* arrows */
	case 0x3c:	return 0x7c;
	case 0x3d:	return 0x7d;
	case 0x3e:	return 0x7e;
	case 0x7b:	return 0x38;	/* right shift, option, control */
	case 0x7c:	return 0x3a;
	case 0x7d:	return 0x3b;
	}
	return c;
}

/* the session's reader, at the display's interrupt level */
void
ui_kin(s, type, code, value)
	struct dssess *s;
	int type, code;
	long value;
{
	switch (type) {
	case IE_KEY:
		code &= 0x7f;
		if (value)
			in_raw[code >> 3] |= 1 << (code & 7);
		else
			in_raw[code >> 3] &= ~(1 << (code & 7));
		ui_key(ui_vkey(code), value != 0);
		break;
	case IE_REL:
		if (code == IE_RELX)
			ui_move(value, 0L);
		else if (code == IE_RELY)
			ui_move(0L, value);
		break;
	case IE_BTN:
		if (code == 1 && (value != 0) != in_btn) {
			uin_nbtnin++;
			in_btn = value != 0;
			in_flags |= IN_BTN;
			ev_input(in_btn ? 1 : 2, 0L);
		}
		break;
	}
}

/* each tick, from the layer's timer: auto-key */
void
ui_evtick()
{
	int s = DS_SPL(DS_HI);

	if (in_down >= 0 && (long)(lbolt - in_rpt) >= 0) {
		in_rpt = lbolt + in_rate;
		ev_input(5, ADBKBD | in_down << 8 | ui_char(in_down, ui_mods()));
	}
	if (ev_wait)
		wakeup((caddr_t)ev_q);
	DS_SPLX(s);
}

/* a new screen: mouse in its middle, pinned to it */
void
ui_inscreen(w, h)
	int w, h;
{
	int s = DS_SPL(DS_HI);

	in_pin[0] = in_pin[1] = 0;
	in_pin[2] = h;
	in_pin[3] = w;
	in_mh = w / 2;
	in_mv = h / 2;
	in_dh = in_dv = 0;
	uin_mouse = (long)in_mv << 16 | in_mh;
	in_flags |= IN_SETPOS | IN_BTN;
	cur_on = 0;
	DS_SPLX(s);
}

void
ui_inreset()
{
	int s = DS_SPL(DS_HI);

	ev_first = ev_n = 0;
	in_flags = 0;
	bzero((caddr_t)in_keys, sizeof in_keys);
	bzero((caddr_t)in_raw, sizeof in_raw);
	cur_on = 0;
	in_btn = 0;
	in_btnw = -1;
	in_dh = in_dv = 0;
	in_down = -1;
	in_sec = 0;
	ui.l_kchrlen = 0;
	DS_SPLX(s);
}

void
ui_inflags(f, on)
	int f, on;
{
	int s = DS_SPL(DS_HI);

	if (on)
		in_flags |= f;
	else
		in_flags &= ~f;
	DS_SPLX(s);
}

/*
 * MBState and the ui page's copy of it, in process context.  Also written
 * as events are taken: a mouseDown can wake the task before its tick, and
 * Button() must already say down.
 */
static void
ui_btnout(btn)
	int btn;
{
	caddr_t up = ui_uipof(u.u_procp);

	in_btnw = btn;
	(void)subyte((caddr_t)LM_MBSTATE, btn ? 0 : 0x80);
	if (up)
		(void)subyte(up + UP_BUTTON, btn ? 0 : 0x80);
}

static void
ev_put(b, e)
	char *b;
	struct uiev *e;
{
	P16(b, e->what);
	P32(b + 2, e->msg);
	P32(b + 6, e->when);
	P32(b + 10, e->where);
	P16(b + 14, e->mods);
}

/* UI_GETOSEVENT {blocking, aux, mask, EventRecord, timeOut, mouseRect} */
int
ui_getosevent(b)
	char *b;
{
	struct uiev *e, n;
	int blk = b[0], mask = G16(b + 2), s, i, j;
	long end = lbolt + (long)G32(b + 20);

	s = DS_SPL(DS_HI);
	for (;;) {
		for (i = 0; i < ev_n; i++) {
			e = &ev_q[(ev_first + i) % NEV];
			if (mask & (1 << e->what))
				break;
		}
		if (i < ev_n) {
			ev_put(b + 4, e);
			if (!(blk & 2)) {
				if (e->what >= 3 && e->what <= 5)
					uin_nkey++;
				else if (e->what == 1 || e->what == 2)
					uin_nbtn++;
				if (e->what == 1)
					uin_ndown++;
				for (j = i; j > 0; j--)
					ev_q[(ev_first + j) % NEV] = ev_q[(ev_first + j - 1) % NEV];
				ev_first = (ev_first + 1) % NEV;
				ev_n--;
				uin_nget++;
			}
			DS_SPLX(s);
			if (in_btn != in_btnw)
				ui_btnout(in_btn);
			return 0;
		}
		bcopy(b + 24, (caddr_t)ev_rect, sizeof ev_rect);
		if (!(blk & 1) || (long)(lbolt - end) >= 0 || ui_isnew(ev_rect))
			break;
		ev_wait = 1;
		i = sleep((caddr_t)ev_q, (PZERO + 1) | PCATCH);
		ev_wait = 0;
		if (i)
			break;
	}
	n.what = 0;
	n.msg = 0;
	n.when = lbolt;
	n.where = uin_mouse;
	n.mods = ui_mods();
	ev_put(b + 4, &n);
	DS_SPLX(s);
	if (in_btn != in_btnw)
		ui_btnout(in_btn);
	return 0;
}

/* UI_FLUSHEVENTS {eventMask, stopMask} */
void
ui_flushevents(b)
	char *b;
{
	int mask = G16(b), stop = G16(b + 2), s, i, k, w;
	struct uiev *e;

	s = DS_SPL(DS_HI);
	for (i = k = 0; i < ev_n; i++) {
		e = &ev_q[(ev_first + i) % NEV];
		w = 1 << e->what;
		if (w & stop) {
			for (; i < ev_n; i++)
				ev_q[(ev_first + k++) % NEV] = ev_q[(ev_first + i) % NEV];
			break;
		}
		if (!(w & mask))
			ev_q[(ev_first + k++) % NEV] = *e;
	}
	ev_n = k;
	DS_SPLX(s);
}

/* UI_POSTEVENT {code, msg}: 0; UI_POST_MOD: 1; UI_POST_EVTREC: 2 */
int
ui_postevent(b, how)
	char *b;
	int how;
{
	int s, what = (short)G16(b);

	if (what < 0 || what > 31)
		return EINVAL;
	s = DS_SPL(DS_HI);
	if (how == 0)
		ev_add(what, (long)G32(b + 2), (long)lbolt, uin_mouse, ui_mods());
	else if (how == 1)
		ev_add(what, (long)G32(b + 2), (long)lbolt, uin_mouse, G16(b + 14));
	else
		ev_add(what, (long)G32(b + 2), (long)G32(b + 6), (long)G32(b + 10), G16(b + 14));
	DS_SPLX(s);
	return 0;
}

/* UI_GETKEYS: a byte a key, 0xff up */
int
ui_getkeys(a)
	caddr_t a;
{
	char k[128];
	int i;

	for (i = 0; i < 128; i++)
		k[i] = KEY(i) ? 0 : 0xff;
	return copyout(k, a, sizeof k) ? EFAULT : 0;
}

/* ---- the mouse and the cursor ---- */

/* pixel i of depth d in a big-endian row */
static unsigned long
pxget(p, i, d)
	unsigned char *p;
	int i, d;
{
	int b = i * d;

	if (d == 32)
		return G32(p + (b >> 3));
	if (d == 16)
		return G16(p + (b >> 3));
	return p[b >> 3] >> (8 - d - (b & 7)) & ((1 << d) - 1);
}

static void
pxput(p, i, d, v)
	unsigned char *p;
	int i, d;
	unsigned long v;
{
	int b = i * d, sh, m;

	if (d == 32)
		P32(p + (b >> 3), v);
	else if (d == 16)
		P16(p + (b >> 3), v);
	else {
		sh = 8 - d - (b & 7);
		m = ((1 << d) - 1) << sh;
		p[b >> 3] = (p[b >> 3] & ~m) | (v << sh & m);
	}
}

/*
 * Draw (save != 0) or erase the cursor with its hot spot at h, v, as
 * the Mac's own erase expects: 16 saved pixels a row, packed at the
 * screen's depth, starting with the first row on screen.  At most lim
 * rows on screen.
 */
static int
cur_blit(h, v, save, cr, lim)
	int h, v, save, lim;
	unsigned char *cr;
{
	unsigned char row[64], *sv;
	caddr_t base, p;
	int rb, w, ht, d, x0, y0, rows, cs, ce, r, di, c, k, nb, done;
	int mr, dr;
	unsigned long px, old, blk, wht, ones;

	if (ui_scrgeom(&base, &rb, &w, &ht, &d) || d < 1 || d > 32 || (d & (d - 1)))
		return 0;
	cur_d = d;
	ones = d == 32 ? 0xffffffL : d == 16 ? 0x7fffL : (1L << d) - 1;
	blk = d <= 8 ? ones : 0;
	wht = d <= 8 ? 0 : ones;
	w = rb * 8 / d;
	x0 = h - cur_hx;
	y0 = v - cur_hy;
	rows = ht - y0 < 16 ? ht - y0 : 16;
	if (rows <= 0 || x0 >= w || x0 <= -16)
		return 1;
	cs = x0 < 0 ? -x0 : 0;
	ce = w - x0 < 16 ? w - x0 : 16;
	/* the bytes the visible columns span; k: the first one's pixel in them */
	nb = (((x0 + ce) * d + 7) >> 3) - ((x0 + cs) * d >> 3);
	k = ((x0 + cs) * d & 7) / d;
	done = 0;
	for (r = di = 0; r < rows && di < lim; r++) {
		if (di == 0 && x0 + (long)(y0 + r) * w < 0)
			continue;
		sv = cur_sv + di++ * 2 * d;
		p = base + (long)(y0 + r) * rb + ((x0 + cs) * d >> 3);
		if (copyin(p, (caddr_t)row, nb)) {
			/* take back the rows drawn so far */
			if (save && done)
				(void)cur_blit(h, v, 0, cr, done);
			return 0;
		}
		mr = G16(cr + 32 + 2 * r) << cs;
		dr = G16(cr + 2 * r) << cs;
		for (c = cs; c < ce; c++, mr <<= 1, dr <<= 1) {
			if (!save) {
				pxput(row, k + c - cs, d, pxget(sv, c, d));
				continue;
			}
			old = pxget(row, k + c - cs, d);
			pxput(sv, c, d, old);
			px = mr & 0x8000 ? (dr & 0x8000 ? blk : wht) : dr & 0x8000 ? old ^ ones : old;
			pxput(row, k + c - cs, d, px);
		}
		(void)copyout((caddr_t)row, p, nb);
		done++;
	}
	return 1;
}

/*
 * The Mac's mouse acceleration, kept in its MickeyBytes block: count
 * and limit, the last seven magnitudes, a remainder, then thresholds.
 * A delta is scaled by its smoothed magnitude, times one more for each
 * threshold that magnitude passes.
 */
static void
ui_accel(dp, fresh)
	int *dp, fresh;
{
	unsigned char mb[28];
	long pa;
	int ah, av, mag, n, i, sum, t, avg, e, sc, step;

	if (copyin((caddr_t)LM_MICKEY, (caddr_t)mb, 4) || (pa = G32(mb)) == 0 ||
	    copyin((caddr_t)pa, (caddr_t)mb, sizeof mb))
		return;
	ah = dp[0] < 0 ? -dp[0] : dp[0];
	av = dp[1] < 0 ? -dp[1] : dp[1];
	mag = ah > av ? ah + av / 2 : av + ah / 2;
	if (fresh || mag == 0) {
		/* the first move after a pause starts a new history */
		P16(mb, 1);
		P16(mb + 18, 0);
		if (mag == 0) {
			(void)copyout((caddr_t)mb, (caddr_t)pa, 20);
			return;
		}
	}
	n = G16(mb);
	if (n < G16(mb + 2))
		P16(mb, n + 1);
	sum = mag;
	if (n >= 1 && n <= 8) {
		for (i = 8 - n; i < 7; i++) {
			sum += (short)G16(mb + 4 + 2 * i);
			if (i > 0)
				P16(mb + 2 + 2 * i, G16(mb + 4 + 2 * i));
		}
		P16(mb + 16, mag);
	}
	if (n == 0)
		n = 1;
	t = sum + n;
	t = t < 0 ? -(-t >> 1) : t >> 1;
	avg = (short)(t / n);
	e = mag - avg + (short)G16(mb + 18);
	e = e == -1 ? 0 : e < 0 ? -(-e >> 1) : e >> 1;
	P16(mb + 18, e);
	sc = step = e + avg;
	if (avg > 255)
		avg = 255;
	for (i = 20; i < 28 && avg > mb[i]; i++)
		sc += step;
	for (i = 0; i < 2; i++) {
		t = (short)(dp[i] * sc / mag);
		dp[i] = t < 0 ? (short)(t | 0xff80) : t & 0x7f;
	}
	(void)copyout((caddr_t)mb, (caddr_t)pa, 20);
}

static void
ui_pin(p, pin)
	int *p;
	short *pin;
{
	if (p[1] < pin[1])
		p[1] = pin[1];
	if (p[1] >= pin[3])
		p[1] = pin[3] - 1;
	if (p[0] < pin[0])
		p[0] = pin[0];
	if (p[0] >= pin[2])
		p[0] = pin[2] - 1;
}

/*
 * The mouse, as the Mac's cursor task keeps it.  Deltas go into MTemp.
 * With CrsrNew set and the cursor not busy, the change from RawMouse
 * is accelerated and pinned into RawMouse and MTemp, masked into Mouse.
 * A position the Mac sets (RawMouse, MTemp, CrsrNew) is taken as it is.
 * lm: CrsrVis to MouseOffset.  Returns 1 when Mouse was written.
 */
static int
ui_track(lm, f)
	unsigned char *lm;
	int f;
{
	static int tracking;
	unsigned char b[20];
	int raw[2], d[2], dh, dv, s, i;
	short pin[4];
	long m, o;

	s = DS_SPL(DS_HI);
	dh = in_dh;
	dv = in_dv;
	in_dh = in_dv = 0;
	for (i = 0; i < 4; i++)
		pin[i] = in_pin[i];
	DS_SPLX(s);
	if (f & IN_SETPOS) {
		for (i = 0; i < 12; i += 4) {
			P16(b + i, in_mv);
			P16(b + i + 2, in_mh);
		}
		(void)copyout((caddr_t)b, (caddr_t)LM_MTEMP, 12);
	}
	if ((!dh && !dv && !lm[2]) || lm[1]) {
		if (dh || dv) {
			/* busy: the deltas wait */
			s = DS_SPL(DS_HI);
			in_dh += dh;
			in_dv += dv;
			in_flags |= IN_MOVED;
			DS_SPLX(s);
		}
		tracking = 0;
		return (f & IN_SETPOS) != 0;
	}
	if (copyin((caddr_t)LM_MTEMP, (caddr_t)b, 20))
		return 0;
	if (dh || dv) {
		P16(b, (short)G16(b) + dv);
		P16(b + 2, (short)G16(b + 2) + dh);
		lm[2] = 1;
	}
	if (!lm[3]) {
		/* uncoupled: MTemp only */
		(void)copyout((caddr_t)b, (caddr_t)LM_MTEMP, 4);
		return 0;
	}
	if ((short)G16(b + 16) > (short)G16(b + 12) && (short)G16(b + 18) > (short)G16(b + 14))
		for (i = 0; i < 4; i++)
			pin[i] = G16(b + 12 + 2 * i);
	raw[0] = (short)G16(b + 4);
	raw[1] = (short)G16(b + 6);
	d[0] = (short)G16(b + 2) - raw[1];
	d[1] = (short)G16(b) - raw[0];
	ui_accel(d, !tracking);
	tracking = 1;
	raw[1] += d[0];
	raw[0] += d[1];
	ui_pin(raw, pin);
	P16(b, raw[0]);
	P16(b + 2, raw[1]);
	P16(b + 4, raw[0]);
	P16(b + 6, raw[1]);
	m = G32(lm + LM_MMASK - LM_CRSRVIS);
	o = G32(lm + LM_MMASK + 4 - LM_CRSRVIS);
	if (m == 0)
		m = -1;
	m &= (long)raw[0] << 16 | (raw[1] & 0xffff);
	if (o) {
		raw[0] = (short)(m >> 16) + (short)(o >> 16);
		raw[1] = (short)m + (short)o;
		ui_pin(raw, pin);
		m = (long)raw[0] << 16 | (raw[1] & 0xffff);
	}
	P32(b + 8, m);
	(void)copyout((caddr_t)b, (caddr_t)LM_MTEMP, 12);
	if (lm[4] & 0x80) {
		/* hidden: nothing to draw until it is shown, which sets CrsrNew again */
		lm[2] = 0;
		(void)subyte((caddr_t)LM_CRSRVIS + 2, 0);
	}
	s = DS_SPL(DS_HI);
	in_mv = (short)(m >> 16) + in_dv;
	in_mh = (short)m + in_dh;
	for (i = 0; i < 4; i++)
		in_pin[i] = pin[i];
	uin_mouse = (long)in_mv << 16 | (in_mh & 0xffff);
	DS_SPLX(s);
	return 1;
}

/* draw the cursor at Mouse when it moved or CrsrNew asks; lm as ui_track's */
static void
ui_cursor(up, lm)
	caddr_t up;
	unsigned char *lm;
{
	unsigned char pg[0x28], cr[0x40], sh[2], b[4];
	int vis, h, v;

	vis = lm[0];
	/* Busy or State < 0: leave it; moving undoes Obscure */
	if (lm[1] || (lm[4] & 0x80) || copyin((caddr_t)LM_MTEMP + 8, (caddr_t)b, 4))
		return;
	v = (short)G16(b);
	h = (short)G16(b + 2);
	if (lm[6] && h == cur_h && v == cur_v)
		return;
	if (vis && !lm[2] && h == cur_h && v == cur_v)
		return;
	if (copyin((caddr_t)LM_SHIELD, (caddr_t)sh, 2) || G16(sh) ||
	    copyin(up, (caddr_t)pg, sizeof pg) ||
	    copyin((caddr_t)LM_THECRSR, (caddr_t)cr, sizeof cr))
		return;
	if (vis && cur_on)
		(void)cur_blit(cur_h, cur_v, 0, cr, 16);
	cur_h = h;
	cur_v = v;
	cur_hx = (short)G32(pg + UP_HPX) & 15;
	cur_hy = (short)G32(pg + UP_HPX + 4) & 15;
	cur_on = 0;
	if (!cur_blit(h, v, 1, cr, 16)) {
		if (vis)
			(void)subyte((caddr_t)LM_CRSRVIS, 0);
		return;
	}
	cur_on = 1;
	P32(pg + UP_CX, h);
	P32(pg + UP_CX + 4, v);
	(void)copyout((caddr_t)pg + UP_CX, up + UP_CX, 8);
	(void)copyout((caddr_t)cur_sv, up + UP_DATA, 32 * cur_d);
	lm[0] = 1;
	lm[2] = 0;
	(void)copyout((caddr_t)lm, (caddr_t)LM_CRSRVIS, 3);
	if (lm[6])
		(void)subyte((caddr_t)LM_CRSRVIS + 6, 0);
	uin_ncur++;
}

long	ui_ntaken, ui_tickpc;	/* ticks taken; the PC each interrupted */

/*
 * The task takes its tick: Ticks, Time, mouse, button and keys into
 * Mac memory, then the cursor.
 */
void
ui_update(gp)
	struct guest_proc *gp;
{
	unsigned char b[16], k[16], lm[LM_MMASK + 8 - LM_CRSRVIS];
	caddr_t up = ui_uipof(u.u_procp);
	int f, btn, s, moved;

	if (gp != ui.l_active)
		return;
	ui_ntaken++;
	ui_tickpc = GR_PC(u.u_ar0);
	P32(b, lbolt);
	(void)copyout((caddr_t)b, (caddr_t)LM_TICKS, 4);
	if (hrestime.tv_sec != in_sec) {
		in_sec = hrestime.tv_sec;
		P32(b, in_sec + MACEPOCH);
		(void)copyout((caddr_t)b, (caddr_t)LM_TIME, 4);
		/* once a second: key repeat */
		if (copyin((caddr_t)LM_KEYTHRESH, (caddr_t)b, 4) == 0) {
			in_thresh = (short)G16(b) > 0 ? (short)G16(b) : 1;
			in_rate = (short)G16(b + 2) > 0 ? (short)G16(b + 2) : 1;
		}
	}
	s = DS_SPL(DS_HI);
	f = in_flags;
	in_flags &= ~(IN_MOVED | IN_SETPOS | IN_KEYS | IN_BTN);
	btn = in_btn;
	bcopy((caddr_t)in_keys, (caddr_t)k, sizeof k);
	DS_SPLX(s);
	if (copyin((caddr_t)LM_CRSRVIS, (caddr_t)lm, sizeof lm))
		return;
	moved = ui_track(lm, f);
	if (moved && up) {
		P32(b, in_mh);
		P32(b + 4, in_mv);
		(void)copyout((caddr_t)b, up + UP_MX, 8);
	}
	if ((f & IN_BTN) || btn != in_btnw)
		ui_btnout(btn);
	if (f & IN_KEYS)
		(void)copyout((caddr_t)k, (caddr_t)LM_KEYMAP, 16);
	if ((f & IN_CUR) && up && (moved || lm[2] || !lm[0]))
		ui_cursor(up, lm);
}
