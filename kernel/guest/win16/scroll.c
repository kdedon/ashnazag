/*
 * scroll.c -- scroll bars: the ones in a window's frame (WS_VSCROLL,
 * WS_HSCROLL), the SCROLLBAR control, and the scroll-bar calls.
 * Drawn as Windows 3.1 drew them: arrow buttons at the ends, a thumb
 * button in a grey track.  Tracking is a modal loop of its own that
 * repeats while an arrow or the track is held.
 */

#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "scr.h"

extern int user_poll();

extern void user_flushpaint();

#define	SB_LINEUP	0
#define	SB_LINEDOWN	1
#define	SB_PAGEUP	2
#define	SB_PAGEDOWN	3
#define	SB_THUMBPOSITION 4
#define	SB_THUMBTRACK	5
#define	SB_ENDSCROLL	8

static void
fill(dc, l, t, r, b, color)
	struct dc *dc;
	int l, t, r, b, color;
{
	struct rect x;

	r_set(&x, l, t, r, b);
	d_fillcolor(dc, &x, pal_index(sys_color(color)));
}

/* the thumb's place along the track: its start, the track's ends in *a, *b */
static int
thumbpos(r, vert, sb, ta, tb)
	struct rect *r;
	int vert, *ta, *tb;
	struct sbinfo *sb;
{
	int len = vert ? r->b - r->t : r->r - r->l, th = vert ? r->r - r->l : r->b - r->t;
	int a = (vert ? r->t : r->l) + th - 1, b = (vert ? r->b : r->r) - th + 1, span;

	*ta = a;
	*tb = b;
	span = b - a - th;
	(void)len;
	if (span <= 0 || sb->max <= sb->min)
		return a;
	return a + (int)((long long)(sb->pos - sb->min) * span / (sb->max - sb->min));
}

void
sb_draw(dc, r, vert, sb, enabled)
	struct dc *dc;
	struct rect *r;
	int vert, enabled;
	struct sbinfo *sb;
{
	int th = vert ? r->r - r->l : r->b - r->t, a, b, t, black = 0;
	struct rect x;
	extern void draw_3dbox(), draw_arrow();

	d_frame(dc, r, black);
	t = thumbpos(r, vert, sb, &a, &b);
	if (vert) {
		fill(dc, r->l + 1, a, r->r - 1, b, COLOR_SCROLLBAR);
		r_set(&x, r->l + 1, r->t + 1, r->r - 1, r->t + th - 1);
		draw_3dbox(dc, &x, sb->pressed == 1);
		draw_arrow(dc, (x.l + x.r) / 2, (x.t + x.b) / 2, 4, 0, enabled ? 0 : pal_index(sys_color(COLOR_GRAYTEXT)));
		fill(dc, r->l, r->t + th - 1, r->r, r->t + th, COLOR_WINDOWFRAME);
		r_set(&x, r->l + 1, r->b - th + 1, r->r - 1, r->b - 1);
		draw_3dbox(dc, &x, sb->pressed == 5);
		draw_arrow(dc, (x.l + x.r) / 2, (x.t + x.b) / 2, 4, 1, enabled ? 0 : pal_index(sys_color(COLOR_GRAYTEXT)));
		fill(dc, r->l, r->b - th, r->r, r->b - th + 1, COLOR_WINDOWFRAME);
		if (sb->max > sb->min && b - a > th) {
			r_set(&x, r->l, t, r->r, t + th);
			d_frame(dc, &x, black);
			x.l++; x.r--; x.t++; x.b--;
			draw_3dbox(dc, &x, 0);
		}
	} else {
		fill(dc, a, r->t + 1, b, r->b - 1, COLOR_SCROLLBAR);
		r_set(&x, r->l + 1, r->t + 1, r->l + th - 1, r->b - 1);
		draw_3dbox(dc, &x, sb->pressed == 1);
		draw_arrow(dc, (x.l + x.r) / 2, (x.t + x.b) / 2, 4, 2, enabled ? 0 : pal_index(sys_color(COLOR_GRAYTEXT)));
		fill(dc, r->l + th - 1, r->t, r->l + th, r->b, COLOR_WINDOWFRAME);
		r_set(&x, r->r - th + 1, r->t + 1, r->r - 1, r->b - 1);
		draw_3dbox(dc, &x, sb->pressed == 5);
		draw_arrow(dc, (x.l + x.r) / 2, (x.t + x.b) / 2, 4, 3, enabled ? 0 : pal_index(sys_color(COLOR_GRAYTEXT)));
		fill(dc, r->r - th, r->t, r->r - th + 1, r->b, COLOR_WINDOWFRAME);
		if (sb->max > sb->min && b - a > th) {
			r_set(&x, t, r->t, t + th, r->b);
			d_frame(dc, &x, black);
			x.l++; x.r--; x.t++; x.b--;
			draw_3dbox(dc, &x, 0);
		}
	}
}

/* 1 up arrow, 2 page up, 3 thumb, 4 page down, 5 down arrow, 0 none */
int
sb_hit(r, vert, sb, x, y)
	struct rect *r;
	int vert, x, y;
	struct sbinfo *sb;
{
	int th = vert ? r->r - r->l : r->b - r->t, a, b, t, v;

	if (x < r->l || x >= r->r || y < r->t || y >= r->b)
		return 0;
	v = vert ? y : x;
	t = thumbpos(r, vert, sb, &a, &b);
	if (v < a)
		return 1;
	if (v >= b)
		return 5;
	if (sb->max <= sb->min)
		return 0;
	if (v < t)
		return 2;
	if (v < t + th)
		return 3;
	return 4;
}

/* redraw a scroll bar now */
static void
redraw(w, ctl, r, vert, sb)
	struct wnd *w, *ctl;
	struct rect *r;
	int vert;
	struct sbinfo *sb;
{
	struct wnd *t = ctl ? ctl : w;
	u16 hdc = user_dc(t, ctl ? DCK_WINDOW : DCK_WINDOWNC, (struct rgn *)0);
	struct dc *dc = dc_get(hdc);
	struct rect rr = *r;

	if (!ctl)
		rgn_subrect(&dc->vis, &t->cr), dc->effok = 0;
	sb_draw(dc, &rr, vert, sb, !(t->style & WS_DISABLED));
	user_releasedc(hdc);
}

static void
notify(w, ctl, vert, code, pos)
	struct wnd *w, *ctl;
	int vert, code, pos;
{
	u32 msg = vert ? WM_VSCROLL : WM_HSCROLL;

	if (ctl)
		wnd_send(ctl->parent, msg, code, FP(ctl->h, pos));
	else
		wnd_send(w, msg, code, FP(0, pos));
}

void
sb_track(w, ctl, r, vert, sb, x, y)
	struct wnd *w, *ctl;
	struct rect *r;
	struct sbinfo *sb;
	int vert, x, y;
{
	int part = sb_hit(r, vert, sb, x, y), in = 1, a, b, t, th, off, pos = sb->pos, span;
	u32 next = w16_ticks() + 350;
	struct ev e;
	static int codes[6] = { 0, SB_LINEUP, SB_PAGEUP, 0, SB_PAGEDOWN, SB_LINEDOWN };
	u16 h = (ctl ? ctl : w)->h;

	if (!part || sb->disabled)
		return;
	th = vert ? r->r - r->l : r->b - r->t;
	t = thumbpos(r, vert, sb, &a, &b);
	off = (vert ? y : x) - t;
	if (part != 3) {
		sb->pressed = part == 1 || part == 5 ? part : 0;
		redraw(w, ctl, r, vert, sb);
		notify(w, ctl, vert, codes[part], 0);
	}
	for (;;) {
		user_flushpaint();
		scr_flush();
		if (!wnd_get(h))
			return;
		if (user_poll(&e, part == 3 ? -1 : 30) == 1) {
			if (e.type == EV_MOVE || e.type == EV_BTN) {
				scr_mx = e.x;
				scr_my = e.y;
				if (part == 3) {
					span = b - a - th;
					if (span > 0 && sb->max > sb->min) {
						int v = (vert ? e.y : e.x) - off - a;

						if (v < 0) v = 0;
						if (v > span) v = span;
						pos = sb->min + (int)((long long)v * (sb->max - sb->min) + span / 2) / span;
						notify(w, ctl, vert, SB_THUMBTRACK, pos);
					}
				} else
					in = sb_hit(r, vert, sb, e.x, e.y) == part;
				if (e.type == EV_BTN && !e.down)
					break;
			}
			continue;
		}
		if (part != 3 && in && (int)(w16_ticks() - next) >= 0) {
			t = thumbpos(r, vert, sb, &a, &b);
			if ((part == 2 || part == 4) && sb_hit(r, vert, sb, scr_mx, scr_my) != part)
				continue;
			notify(w, ctl, vert, codes[part], 0);
			next = w16_ticks() + 50;
		}
	}
	if (part == 3)
		notify(w, ctl, vert, SB_THUMBPOSITION, pos);
	notify(w, ctl, vert, SB_ENDSCROLL, 0);
	if (wnd_get(h)) {
		sb->pressed = 0;
		redraw(w, ctl, r, vert, sb);
	}
}

/* ---- the SCROLLBAR control ---- */

#define	SBS_VERT	1

u32
scroll_proc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	struct rect r;
	u16 hdc;
	struct dc *dc;
	int vert;

	if (!w)
		return 0;
	vert = w->style & SBS_VERT;
	r = w->cr;
	switch (a[1]) {
	case WM_CREATE:
		w->sb[0].min = 0;
		w->sb[0].max = 100;
		return 0;
	case WM_PAINT:
		{
			extern u16 user_beginpaint();
			extern void user_endpaint();

			hdc = user_beginpaint(w, 0);
			dc = dc_get(hdc);
			sb_draw(dc, &r, vert, &w->sb[0], !(w->style & WS_DISABLED));
			user_endpaint(w, 0);
			user_releasedc(hdc);
		}
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		if (w->style & WS_TABSTOP)
			wnd_setfocus(w);
		sb_track(w->parent, w, &r, vert, &w->sb[0], (short)LO16(a[3]) + w->cr.l, (short)HI16(a[3]) + w->cr.t);
		return 0;
	case WM_KEYDOWN:
		switch (a[2]) {
		case VK_UP: case VK_LEFT: notify(0, w, vert, SB_LINEUP, 0); break;
		case VK_DOWN: case VK_RIGHT: notify(0, w, vert, SB_LINEDOWN, 0); break;
		case VK_PRIOR: notify(0, w, vert, SB_PAGEUP, 0); break;
		case VK_NEXT: notify(0, w, vert, SB_PAGEDOWN, 0); break;
		case VK_HOME: notify(0, w, vert, 6, 0); break;
		case VK_END: notify(0, w, vert, 7, 0); break;
		}
		return 0;
	case WM_GETDLGCODE:
		return 1;	/* DLGC_WANTARROWS */
	case WM_ENABLE:
		wnd_invalidate(w, (struct rect *)0, 0);
		return 0;
	}
	return user_defproc(w, a[1], a[2], a[3]);
}

/* ---- the calls ---- */

static struct sbinfo *
info(w, bar, rp, vertp)
	struct wnd *w;
	int bar, *vertp;
	struct rect *rp;
{
	int sb = sys_metric(SM_CXVSCROLL);

	if (bar == 2) {
		*rp = w->cr;
		*vertp = w->style & SBS_VERT;
		return &w->sb[0];
	}
	*vertp = bar == 1;
	if (bar == 1)
		r_set(rp, w->cr.r - 1, w->cr.t - 1, w->cr.r + sb - 1, w->cr.b + 1);
	else
		r_set(rp, w->cr.l - 1, w->cr.b - 1, w->cr.r + 1, w->cr.b + sb - 1);
	return &w->sb[bar & 1];
}

static void
sbredraw(w, bar)
	struct wnd *w;
	int bar;
{
	struct rect r;
	int vert;
	struct sbinfo *sb = info(w, bar, &r, &vert);

	if (!wnd_visible(w))
		return;
	if (bar == 2) {
		wnd_invalidate(w, (struct rect *)0, 0);
		return;
	}
	if (!(w->style & (bar == 1 ? WS_VSCROLL : WS_HSCROLL)))
		return;
	redraw(w, (struct wnd *)0, &r, vert, sb);
}

static void
showbar(w, bar, show)
	struct wnd *w;
	int bar, show;
{
	u32 st = w->style, f = bar == 1 ? WS_VSCROLL : bar == 0 ? WS_HSCROLL : WS_VSCROLL | WS_HSCROLL;

	if (show)
		w->style |= f;
	else
		w->style &= ~f;
	if (w->style != st)
		wnd_setpos(w, (struct wnd *)0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
		    SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static u32
c_SetScrollPos(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	struct rect r;
	int vert, old, pos = (short)a[2];
	struct sbinfo *sb;

	if (!w)
		return 0;
	sb = info(w, a[1], &r, &vert);
	old = sb->pos;
	if (pos < sb->min) pos = sb->min;
	if (pos > sb->max) pos = sb->max;
	sb->pos = pos;
	if (a[3] && old != pos)
		sbredraw(w, a[1]);
	return old & 0xffff;
}

static u32
c_GetScrollPos(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	struct rect r;
	int vert;

	if (!w)
		return 0;
	return info(w, a[1], &r, &vert)->pos & 0xffff;
}

static u32
c_SetScrollRange(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	struct rect r;
	int vert;
	struct sbinfo *sb;

	if (!w)
		return 0;
	sb = info(w, a[1], &r, &vert);
	sb->min = (short)a[2];
	sb->max = (short)a[3];
	if (sb->pos < sb->min) sb->pos = sb->min;
	if (sb->pos > sb->max) sb->pos = sb->max;
	if (a[1] != 2)
		showbar(w, a[1], sb->min != sb->max);
	if (a[4])
		sbredraw(w, a[1]);
	return 1;
}

static u32
c_GetScrollRange(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	struct rect r;
	int vert;
	struct sbinfo *sb;
	u32 p = lin(FPSEL(a[2]), FPOFF(a[2])), q = lin(FPSEL(a[3]), FPOFF(a[3]));

	if (!w)
		return 0;
	sb = info(w, a[1], &r, &vert);
	if (p) PW(p, sb->min);
	if (q) PW(q, sb->max);
	return 1;
}

static u32
c_ShowScrollBar(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);

	if (!w)
		return 0;
	if (a[1] == 2) {
		wnd_show(w, a[2] ? SW_SHOW : SW_HIDE);
		return 0;
	}
	showbar(w, a[1] == 3 ? 3 : a[1], a[2]);
	return 0;
}

static u32
c_EnableScrollBar(a)
	u32 *a;
{
	return 1;
}

struct impl sb_impl[] = {
	{ "USER", "SetScrollPos", c_SetScrollPos },
	{ "USER", "GetScrollPos", c_GetScrollPos },
	{ "USER", "SetScrollRange", c_SetScrollRange },
	{ "USER", "GetScrollRange", c_GetScrollRange },
	{ "USER", "ShowScrollBar", c_ShowScrollBar },
	{ "USER", "EnableScrollBar", c_EnableScrollBar },
	{ 0 }
};
