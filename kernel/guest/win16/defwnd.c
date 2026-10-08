/*
 * defwnd.c -- DefWindowProc and the Windows 3.1 look: frames, captions
 * with the system-menu box and the minimize and maximize buttons, menu
 * bars and scroll bars around the client area; hit testing, moving and
 * sizing (an outline dragged on the screen, as 3.1 did), and DrawText.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "scr.h"

extern u32 ualloc(), ulin(), ustr();
extern void ufree(), user_ncpaint(), expose(), wnd_setfocus(), wnd_activate();
extern u16 user_beginpaint();
extern void user_endpaint(), user_postquit();
extern u8 keystate[];
extern int IsChildOf();
extern void sb_draw();
extern int sb_hit();
extern void sb_track();
extern void icon_draw();
extern u16 cur_load();

#define	STR(p)		(gptr(p) ? gptr(p) : "")

/* ---- frame geometry ---- */

int
frame_width(w)
	struct wnd *w;
{
	if (w->style & WS_MINIMIZE)
		return 0;
	if (w->style & WS_THICKFRAME)
		return sys_metric(SM_CXFRAME);
	if ((w->style & WS_CAPTION) == WS_DLGFRAME || (w->exstyle & WS_EX_DLGMODALFRAME))
		return sys_metric(SM_CXDLGFRAME);
	if (w->style & WS_BORDER)
		return 1;
	return 0;
}

static int
hascaption(w)
	struct wnd *w;
{
	return (w->style & WS_CAPTION) == WS_CAPTION && !(w->style & WS_MINIMIZE);
}

/* the caption bar, screen coordinates */
static void
captionrect(w, r)
	struct wnd *w;
	struct rect *r;
{
	int b = frame_width(w);

	if ((w->style & WS_CAPTION) == WS_CAPTION && !(w->style & WS_THICKFRAME) &&
	    !(w->exstyle & WS_EX_DLGMODALFRAME))
		b = 1;
	r_set(r, w->wr.l + b, w->wr.t + b, w->wr.r - b, w->wr.t + b + sys_metric(SM_CYCAPTION) - 2);
}

#define	BTN	(sys_metric(SM_CXSIZE) + 1)

/* ---- drawing helpers ---- */

static void
fillc(dc, l, t, r, b, color)
	struct dc *dc;
	int l, t, r, b, color;
{
	struct rect x;

	r_set(&x, l, t, r, b);
	d_fillcolor(dc, &x, pal_index(sys_color(color)));
}

static void
filli(dc, l, t, r, b, idx)
	struct dc *dc;
	int l, t, r, b, idx;
{
	struct rect x;

	r_set(&x, l, t, r, b);
	d_fillcolor(dc, &x, idx);
}

/* a raised (or pressed) 3.1 button face over r, no outer border */
void
draw_3dbox(dc, r, pressed)
	struct dc *dc;
	struct rect *r;
	int pressed;
{
	int face = pal_index(sys_color(COLOR_BTNFACE)), hi = pal_index(sys_color(COLOR_BTNHIGHLIGHT));
	int sh = pal_index(sys_color(COLOR_BTNSHADOW));

	filli(dc, r->l, r->t, r->r, r->b, face);
	if (pressed) {
		filli(dc, r->l, r->t, r->r, r->t + 1, sh);
		filli(dc, r->l, r->t, r->l + 1, r->b, sh);
		return;
	}
	filli(dc, r->l, r->t, r->r - 1, r->t + 1, hi);
	filli(dc, r->l, r->t, r->l + 1, r->b - 1, hi);
	filli(dc, r->l, r->b - 1, r->r, r->b, sh);
	filli(dc, r->r - 1, r->t, r->r, r->b, sh);
	if (r->b - r->t > 12 && r->r - r->l > 12) {
		filli(dc, r->l + 1, r->b - 2, r->r - 1, r->b - 1, sh);
		filli(dc, r->r - 2, r->t + 1, r->r - 1, r->b - 1, sh);
	}
}

/* a small triangle: dir 0 up, 1 down, 2 left, 3 right */
void
draw_arrow(dc, cx, cy, size, dir, idx)
	struct dc *dc;
	int cx, cy, size, dir, idx;
{
	int i;

	for (i = 0; i < size; i++)
		switch (dir) {
		case 0: filli(dc, cx - i, cy - size / 2 + i, cx + i + 1, cy - size / 2 + i + 1, idx); break;
		case 1: filli(dc, cx - i, cy + size / 2 - i, cx + i + 1, cy + size / 2 - i + 1, idx); break;
		case 2: filli(dc, cx - size / 2 + i, cy - i, cx - size / 2 + i + 1, cy + i + 1, idx); break;
		case 3: filli(dc, cx + size / 2 - i, cy - i, cx + size / 2 - i + 1, cy + i + 1, idx); break;
		}
}

/* ---- DrawText ---- */

int
draw_textw(f, s, n)
	struct bfont *f;
	char *s;
	int n;
{
	return text_width(f, s, n);
}

/* one line's width, & prefixes and tabs as drawn */
static int
linew(f, s, n, flags, tabw)
	struct bfont *f;
	char *s;
	int n, flags, tabw;
{
	int w = 0, i;

	for (i = 0; i < n; i++) {
		if (s[i] == '&' && !(flags & DT_NOPREFIX)) {
			if (i + 1 < n && s[i + 1] == '&')
				i++;
			else
				continue;
		}
		if (s[i] == '\t' && (flags & DT_EXPANDTABS)) {
			w = (w / tabw + 1) * tabw;
			continue;
		}
		w += text_width(f, s + i, 1);
	}
	return w;
}

static void
drawline(dc, f, s, n, x, y, flags, tabw)
	struct dc *dc;
	struct bfont *f;
	char *s;
	int n, x, y, flags, tabw;
{
	int i, x0 = x, ul = -1, ulw = 0, cw;
	struct rect r;

	for (i = 0; i < n; i++) {
		if (s[i] == '&' && !(flags & DT_NOPREFIX)) {
			if (i + 1 < n && s[i + 1] == '&')
				i++;
			else {
				ul = x;
				ulw = i + 1 < n ? text_width(f, s + i + 1, 1) : 0;
				continue;
			}
		}
		if (s[i] == '\t' && (flags & DT_EXPANDTABS)) {
			x = x0 + ((x - x0) / tabw + 1) * tabw;
			continue;
		}
		cw = text_width(f, s + i, 1);
		text_draw(dc, x, y, s + i, 1, (struct rect *)0, (struct rect *)0, (int *)0);
		x += cw;
	}
	if (ul >= 0) {
		r_set(&r, ul, y + f->f_ascent + 1, ul + ulw, y + f->f_ascent + 2);
		d_fillcolor(dc, &r, dc->s->mono ? pal_mono(dc->st.text) : pal_index(dc->st.text));
	}
}

/*
 * DrawText in surface coordinates (r): lines broken at newlines and, with
 * DT_WORDBREAK, at spaces; the height back (and r widened with DT_CALCRECT).
 */
int
draw_text(dc, s, n, r, flags)
	struct dc *dc;
	char *s;
	int n, flags;
	struct rect *r;
{
	struct bfont *f = font_of(dc);
	int y = r->t, lh = f->f_height, tabw = 8 * text_width(f, " ", 1), maxw = 0;
	int start, end, brk, w, x, total, i, keepmode = dc->st.bkmode;
	char *p;
	struct rect clip;

	if (n < 0)
		n = strlen(s);
	if (tabw <= 0)
		tabw = 32;
	if (flags & DT_SINGLELINE) {
		w = linew(f, s, n, flags, tabw);
		if (flags & DT_CALCRECT) {
			r->r = r->l + w;
			r->b = r->t + lh;
			return lh;
		}
		x = flags & DT_CENTER ? (r->l + r->r - w) / 2 : flags & DT_RIGHT ? r->r - w : r->l;
		y = flags & DT_VCENTER ? (r->t + r->b - lh) / 2 : flags & DT_BOTTOM ? r->b - lh : r->t;
		if (!(flags & DT_NOCLIP)) {
			clip = *r;
			if (!R_EMPTY(&clip)) {
				struct rgn save;

				rgn_init(&save);
				rgn_copy(&save, dc_clip(dc));
				rgn_andrect(&dc->eff, &clip);
				drawline(dc, f, s, n, x, y, flags, tabw);
				rgn_copy(&dc->eff, &save);
				rgn_free(&save);
			}
		} else
			drawline(dc, f, s, n, x, y, flags, tabw);
		return lh;
	}
	total = 0;
	for (start = 0; start < n; ) {
		/* one line */
		for (end = start; end < n && s[end] != '\n' && s[end] != '\r'; end++)
			;
		if (flags & DT_WORDBREAK) {
			brk = -1;
			for (i = start; i < end; i++) {
				if (s[i] == ' ' || s[i] == '\t')
					brk = i;
				if (linew(f, s + start, i - start + 1, flags, tabw) > r->r - r->l && i > start) {
					end = brk > start ? brk : i;
					break;
				}
			}
		}
		w = linew(f, s + start, end - start, flags, tabw);
		if (w > maxw)
			maxw = w;
		if (!(flags & DT_CALCRECT)) {
			x = flags & DT_CENTER ? (r->l + r->r - w) / 2 : flags & DT_RIGHT ? r->r - w : r->l;
			if (!(flags & DT_NOCLIP)) {
				struct rgn save;

				clip = *r;
				rgn_init(&save);
				rgn_copy(&save, dc_clip(dc));
				rgn_andrect(&dc->eff, &clip);
				drawline(dc, f, s + start, end - start, x, y, flags, tabw);
				rgn_copy(&dc->eff, &save);
				rgn_free(&save);
			} else
				drawline(dc, f, s + start, end - start, x, y, flags, tabw);
		}
		y += lh;
		total += lh;
		start = end;
		if (start < n && s[start] == '\r')
			start++;
		if (start < n && s[start] == '\n')
			start++;
		else if ((flags & DT_WORDBREAK) && start < n && (s[start] == ' ' || s[start] == '\t'))
			start++;
	}
	if (flags & DT_CALCRECT) {
		r->r = r->l + maxw;
		r->b = r->t + total;
	}
	(void)p;
	(void)keepmode;
	return total;
}

/* ---- non-client painting ---- */

/* the system menu box: a grey square with the short white bar */
static void
draw_sysbox(dc, x, y, s)
	struct dc *dc;
	int x, y, s;
{
	struct rect r;
	int mid = y + s / 2;

	r_set(&r, x, y, x + s, y + s);
	d_fillcolor(dc, &r, pal_index(sys_color(COLOR_BTNFACE)));
	fillc(dc, x + 3, mid - 1, x + s - 3, mid + 2, COLOR_BTNHIGHLIGHT);
	fillc(dc, x + 3, mid + 2, x + s - 2, mid + 3, COLOR_BTNSHADOW);
	fillc(dc, x + s - 3, mid - 1, x + s - 2, mid + 3, COLOR_BTNSHADOW);
	filli(dc, x + 2, mid - 2, x + s - 3, mid - 1, 0);
	filli(dc, x + 2, mid - 2, x + 3, mid + 2, 0);
}

static void
draw_capbutton(dc, x, y, s, kind, pressed)
	struct dc *dc;
	int x, y, s, kind, pressed;		/* 0 minimize, 1 maximize, 2 restore */
{
	struct rect r;
	int cx = x + s / 2 + (pressed ? 1 : 0), cy = y + s / 2 + (pressed ? 1 : 0);

	r_set(&r, x, y, x + s, y + s);
	draw_3dbox(dc, &r, pressed);
	switch (kind) {
	case 0: draw_arrow(dc, cx, cy, 4, 1, 0); break;
	case 1: draw_arrow(dc, cx, cy, 4, 0, 0); break;
	case 2: draw_arrow(dc, cx, cy - 3, 3, 0, 0); draw_arrow(dc, cx, cy + 3, 3, 1, 0); break;
	}
}

static int capbtnpressed;	/* while tracking: 1 min, 2 max */

void
user_drawnc(w)
	struct wnd *w;
{
	u16 hdc;
	struct dc *dc;
	struct rect r, c;
	int b, active = w == wnd_active || (w->flags & WF_ACTIVE), black = 0, x, bw, s, fr;
	struct bfont *f;

	if (!wnd_visible(w))
		return;
	hdc = user_dc(w, DCK_WINDOWNC, (struct rgn *)0);
	dc = dc_get(hdc);
	/* the frame only: the client area is the window's */
	rgn_subrect(&dc->vis, &w->cr);
	dc->effok = 0;
	r = w->wr;
	if (w->style & WS_MINIMIZE) {
		fillc(dc, r.l, r.t, r.r, r.b, COLOR_BACKGROUND);
		icon_draw(dc, w->hicon ? w->hicon : w->cls->icon, r.l + 2, r.t + 2);
		user_releasedc(hdc);
		return;
	}
	b = frame_width(w);
	fr = b;
	if (hascaption(w) && !(w->style & WS_THICKFRAME) && !(w->exstyle & WS_EX_DLGMODALFRAME))
		fr = 1;
	if (fr) {
		d_frame(dc, &r, black);
		if (fr > 1) {
			struct rect in;
			int band;

			band = (w->style & WS_THICKFRAME) ? (active ? COLOR_ACTIVEBORDER : COLOR_INACTIVEBORDER) :
			    (active ? COLOR_ACTIVECAPTION : COLOR_INACTIVECAPTION);
			fillc(dc, r.l + 1, r.t + 1, r.r - 1, r.t + fr - 1, band);
			fillc(dc, r.l + 1, r.b - fr + 1, r.r - 1, r.b - 1, band);
			fillc(dc, r.l + 1, r.t + 1, r.l + fr - 1, r.b - 1, band);
			fillc(dc, r.r - fr + 1, r.t + 1, r.r - 1, r.b - 1, band);
			r_set(&in, r.l + fr - 1, r.t + fr - 1, r.r - fr + 1, r.b - fr + 1);
			d_frame(dc, &in, black);
			if (w->style & WS_THICKFRAME) {
				/* the corner marks */
				int k = sys_metric(SM_CYCAPTION) + fr - 1;

				filli(dc, r.l, r.t + k, r.l + fr, r.t + k + 1, black);
				filli(dc, r.r - fr, r.t + k, r.r, r.t + k + 1, black);
				filli(dc, r.l, r.b - k - 1, r.l + fr, r.b - k, black);
				filli(dc, r.r - fr, r.b - k - 1, r.r, r.b - k, black);
				filli(dc, r.l + k, r.t, r.l + k + 1, r.t + fr, black);
				filli(dc, r.l + k, r.b - fr, r.l + k + 1, r.b, black);
				filli(dc, r.r - k - 1, r.t, r.r - k, r.t + fr, black);
				filli(dc, r.r - k - 1, r.b - fr, r.r - k, r.b, black);
			}
		}
	}
	if (hascaption(w)) {
		captionrect(w, &c);
		s = c.b - c.t;
		fillc(dc, c.l, c.t, c.r, c.b, active ? COLOR_ACTIVECAPTION : COLOR_INACTIVECAPTION);
		filli(dc, c.l, c.b, c.r, c.b + 1, black);
		x = c.l;
		if (w->style & WS_SYSMENU) {
			draw_sysbox(dc, c.l, c.t, s);
			filli(dc, c.l + s, c.t, c.l + s + 1, c.b, black);
			x = c.l + s + 1;
		}
		bw = c.r;
		if (w->style & WS_MAXIMIZEBOX) {
			bw -= s + 1;
			filli(dc, bw, c.t, bw + 1, c.b, black);
			draw_capbutton(dc, bw + 1, c.t, s, (w->style & WS_MAXIMIZE) ? 2 : 1, capbtnpressed == 2);
		}
		if (w->style & WS_MINIMIZEBOX) {
			bw -= s + 1;
			filli(dc, bw, c.t, bw + 1, c.b, black);
			draw_capbutton(dc, bw + 1, c.t, s, 0, capbtnpressed == 1);
		}
		/* the title, centred in what is left */
		f = gobj(stockobj[SYSTEM_FONT], OBJ_FONT)->u.font.bf;
		{
			struct rect t;
			int keepf = dc->st.font;
			COLORREF kt = dc->st.text;
			int km = dc->st.bkmode;

			r_set(&t, x + 2, c.t, bw - 2, c.b);
			dc->st.font = stockobj[SYSTEM_FONT];
			dc->st.text = sys_color(active ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT);
			dc->st.bkmode = TRANSPARENT;
			draw_text(dc, w->text, strlen(w->text), &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE |
			    DT_NOPREFIX);
			dc->st.font = keepf;
			dc->st.text = kt;
			dc->st.bkmode = km;
		}
		(void)f;
	}
	if (!(w->style & WS_CHILD) && w->id && menu_barheight(w))
		menu_drawbar(w, dc);
	/* scroll bars */
	if (w->style & (WS_VSCROLL | WS_HSCROLL)) {
		struct rect sr;
		int sb = sys_metric(SM_CXVSCROLL);

		if (w->style & WS_VSCROLL) {
			r_set(&sr, w->cr.r - 1, w->cr.t - 1, w->cr.r + sb - 1, w->cr.b + 1);
			if (sr.r > w->wr.r - b)
				sr.r = w->wr.r - b;
			sb_draw(dc, &sr, 1, &w->sb[1], w->style & WS_DISABLED ? 0 : 1);
		}
		if (w->style & WS_HSCROLL) {
			r_set(&sr, w->cr.l - 1, w->cr.b - 1, w->cr.r + 1, w->cr.b + sb - 1);
			sb_draw(dc, &sr, 0, &w->sb[0], 1);
		}
		if ((w->style & WS_VSCROLL) && (w->style & WS_HSCROLL))
			fillc(dc, w->cr.r, w->cr.b, w->cr.r + sb - 2, w->cr.b + sb - 2, COLOR_BTNFACE);
	}
	user_releasedc(hdc);
}

/* ---- hit testing ---- */

int
user_nchittest(w, x, y)
	struct wnd *w;
	int x, y;
{
	struct rect r = w->wr, c;
	int b = frame_width(w), s, corner;

	if (x < r.l || x >= r.r || y < r.t || y >= r.b)
		return HTNOWHERE;
	if (w->style & WS_MINIMIZE)
		return HTCAPTION;
	if (x >= w->cr.l && x < w->cr.r && y >= w->cr.t && y < w->cr.b)
		return HTCLIENT;
	if ((w->style & WS_THICKFRAME) && (x < r.l + b || x >= r.r - b || y < r.t + b || y >= r.b - b)) {
		corner = sys_metric(SM_CYCAPTION) + b;
		if (y < r.t + corner) {
			if (x < r.l + corner) return HTTOPLEFT;
			if (x >= r.r - corner) return HTTOPRIGHT;
		}
		if (y >= r.b - corner) {
			if (x < r.l + corner) return HTBOTTOMLEFT;
			if (x >= r.r - corner) return HTBOTTOMRIGHT;
		}
		if (y < r.t + b) return HTTOP;
		if (y >= r.b - b) return HTBOTTOM;
		if (x < r.l + b) return HTLEFT;
		return HTRIGHT;
	}
	if (hascaption(w)) {
		captionrect(w, &c);
		s = c.b - c.t;
		if (y >= c.t && y <= c.b) {
			if ((w->style & WS_SYSMENU) && x < c.l + s + 1)
				return HTSYSMENU;
			if (w->style & WS_MAXIMIZEBOX) {
				if (x >= c.r - s - 1)
					return HTMAXBUTTON;
				c.r -= s + 1;
			}
			if ((w->style & WS_MINIMIZEBOX) && x >= c.r - s - 1)
				return HTMINBUTTON;
			return HTCAPTION;
		}
	}
	if (!(w->style & WS_CHILD) && w->id && menu_barheight(w) && y < w->cr.t &&
	    y >= w->cr.t - menu_barheight(w))
		return HTMENU;
	if ((w->style & WS_VSCROLL) && x >= w->cr.r && y < w->cr.b)
		return HTVSCROLL;
	if ((w->style & WS_HSCROLL) && y >= w->cr.b && x < w->cr.r)
		return HTHSCROLL;
	if ((w->style & WS_VSCROLL) && (w->style & WS_HSCROLL) && x >= w->cr.r && y >= w->cr.b)
		return (w->style & WS_THICKFRAME) ? HTBOTTOMRIGHT : HTSIZE;
	return HTBORDER;
}

/* ---- moving and sizing: an outline on the screen ---- */

static void
xorframe(r)
	struct rect *r;
{
	static u16 sdc;
	struct dc *dc;
	struct rect e;
	int k = 4;

	if (!sdc) {
		u32 a[1];
		struct impl *im;

		a[0] = 0;
		for (im = g_impl; im->im_mod; im++)
			if (strcmp(im->im_name, "CreateDC") == 0) {
				sdc = (*im->im_fn)(a);
				break;
			}
	}
	dc = dc_get(sdc);
	r_set(&e, r->l, r->t, r->r, r->t + k); d_invert(dc, &e);
	r_set(&e, r->l, r->b - k, r->r, r->b); d_invert(dc, &e);
	r_set(&e, r->l, r->t + k, r->l + k, r->b - k); d_invert(dc, &e);
	r_set(&e, r->r - k, r->t + k, r->r, r->b - k); d_invert(dc, &e);
}

static void
movesize(w, ht, x0, y0)
	struct wnd *w;
	int ht, x0, y0;
{
	struct rect r = w->wr, n;
	struct ev e;
	int done = 0, dx, dy, minw = sys_metric(SM_CXMINTRACK), minh = sys_metric(SM_CYMINTRACK);

	if (w->style & WS_MAXIMIZE)
		return;
	xorframe(&r);
	scr_flush();
	n = r;
	while (!done) {
		if (scr_poll(&e, -1) != 1)
			continue;
		if (e.type == EV_MOVE || e.type == EV_BTN) {
			dx = e.x - x0;
			dy = e.y - y0;
			n = r;
			switch (ht) {
			case HTCAPTION:
				n.l += dx; n.r += dx; n.t += dy; n.b += dy;
				break;
			case HTLEFT: n.l += dx; break;
			case HTRIGHT: n.r += dx; break;
			case HTTOP: n.t += dy; break;
			case HTBOTTOM: n.b += dy; break;
			case HTTOPLEFT: n.l += dx; n.t += dy; break;
			case HTTOPRIGHT: n.r += dx; n.t += dy; break;
			case HTBOTTOMLEFT: n.l += dx; n.b += dy; break;
			case HTBOTTOMRIGHT: case HTSIZE: n.r += dx; n.b += dy; break;
			}
			if (n.r - n.l < minw) {
				if (ht == HTLEFT || ht == HTTOPLEFT || ht == HTBOTTOMLEFT)
					n.l = n.r - minw;
				else
					n.r = n.l + minw;
			}
			if (n.b - n.t < minh) {
				if (ht == HTTOP || ht == HTTOPLEFT || ht == HTTOPRIGHT)
					n.t = n.b - minh;
				else
					n.b = n.t + minh;
			}
			xorframe(&w->priv_track);
			w->priv_track = n;
			xorframe(&n);
			scr_mx = e.x;
			scr_my = e.y;
			scr_flush();
			if (e.type == EV_BTN && !e.down)
				done = 1;
		} else if (e.type == EV_KEY && e.down && e.vk == VK_ESCAPE) {
			n = r;
			done = 1;
		}
		if (!done && w->priv_track.r == 0)
			w->priv_track = n;
	}
	xorframe(&w->priv_track);
	memset(&w->priv_track, 0, sizeof w->priv_track);
	if (n.l != r.l || n.t != r.t || n.r != r.r || n.b != r.b) {
		if (w->style & WS_CHILD) {
			n.l -= w->parent->cr.l; n.r -= w->parent->cr.l;
			n.t -= w->parent->cr.t; n.b -= w->parent->cr.t;
		}
		wnd_setpos(w, (struct wnd *)0, n.l, n.t, n.r - n.l, n.b - n.t, SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

/* a caption button held down: 1 if released over it */
static int
trackbutton(w, which)
	struct wnd *w;
	int which;
{
	struct ev e;
	int in = 1, ht;

	capbtnpressed = which;
	user_drawnc(w);
	scr_flush();
	for (;;) {
		if (scr_poll(&e, -1) != 1)
			continue;
		if (e.type != EV_MOVE && e.type != EV_BTN)
			continue;
		scr_mx = e.x;
		scr_my = e.y;
		ht = user_nchittest(w, e.x, e.y);
		in = ht == (which == 1 ? HTMINBUTTON : HTMAXBUTTON);
		if ((capbtnpressed != 0) != in) {
			capbtnpressed = in ? which : 0;
			user_drawnc(w);
		}
		scr_flush();
		if (e.type == EV_BTN && !e.down)
			break;
	}
	capbtnpressed = 0;
	user_drawnc(w);
	return in;
}

/* ---- DefWindowProc ---- */

static u32
syscommand(w, cmd, lp)
	struct wnd *w;
	u32 cmd, lp;
{
	switch (cmd & 0xfff0) {
	case SC_CLOSE:
		if (w->cls && (w->cls->style & CS_NOCLOSE))
			return 0;
		wnd_send(w, WM_CLOSE, 0, 0);
		return 0;
	case SC_MINIMIZE:
		wnd_show(w, SW_MINIMIZE);
		return 0;
	case SC_MAXIMIZE:
		wnd_show(w, SW_SHOWMAXIMIZED);
		return 0;
	case SC_RESTORE:
		wnd_show(w, SW_RESTORE);
		return 0;
	case SC_MOVE:
		movesize(w, HTCAPTION, (short)LO16(lp), (short)HI16(lp));
		return 0;
	case SC_SIZE:
		return 0;
	case SC_KEYMENU:
	case SC_MOUSEMENU:
		if (!(w->style & WS_CHILD) && w->id)
			menu_trackbar(w, -1, LO16(lp));
		else if (w->style & WS_SYSMENU)
			menu_track(w->sysmenu ? w->sysmenu : sysmenu_of(w), 0x0100,
			    w->wr.l + frame_width(w), w->wr.t + frame_width(w) + sys_metric(SM_CYCAPTION) - 1, w, -1);
		return 0;
	}
	return 0;
}

u32
user_defproc(w, msg, wp, lp)
	struct wnd *w;
	u32 msg, wp, lp;
{
	u32 a, r;
	int n, ht;

	if (!w)
		return 0;
	switch (msg) {
	case WM_NCCREATE:
		if (w->style & (WS_VSCROLL | WS_HSCROLL)) {
			w->sb[0].max = w->sb[1].max = 100;
		}
		return 1;
	case WM_NCCALCSIZE:
		if ((a = lin(FPSEL(lp), FPOFF(lp))) != 0) {
			struct rect wr, cr;
			extern void wnd_calcclient();

			r_get(&wr, a);
			wnd_calcclient(w, &wr, &cr);
			r_put(a, &cr);
		}
		return 0;
	case WM_NCHITTEST:
		return user_nchittest(w, (short)LO16(lp), (short)HI16(lp)) & 0xffff;
	case WM_NCPAINT:
		user_drawnc(w);
		return 0;
	case WM_NCACTIVATE:
		if (wp)
			w->flags |= WF_ACTIVE;
		else
			w->flags &= ~WF_ACTIVE;
		if (wnd_visible(w))
			user_drawnc(w);
		return 1;
	case WM_ACTIVATE:
		if (LO16(wp) != WA_INACTIVE && !(w->style & WS_MINIMIZE))
			wnd_setfocus(w);
		return 0;
	case WM_SETTEXT:
		free(w->text);
		w->text = strdup(lp ? STR(lp) : "");
		if (hascaption(w))
			wnd_redrawframe(w);
		return 1;
	case WM_GETTEXT:
		if ((a = lin(FPSEL(lp), FPOFF(lp))) == 0 || (short)wp <= 0)
			return 0;
		n = strlen(w->text);
		if (n > (short)wp - 1)
			n = (short)wp - 1;
		memcpy(M + a, w->text, n);
		M[a + n] = 0;
		return n;
	case WM_GETTEXTLENGTH:
		return strlen(w->text);
	case WM_PAINT:
		{
			u32 ps = ualloc(32);
			u16 hdc = user_beginpaint(w, ulin(ps));

			if (w->style & WS_MINIMIZE)
				icon_draw(dc_get(hdc), w->hicon ? w->hicon : w->cls->icon, 0, 0);
			user_endpaint(w, ulin(ps));
			ufree(ps);
		}
		return 0;
	case WM_ERASEBKGND:
	case WM_ICONERASEBKGND:
		{
			struct dc *dc = dc_get(wp);
			u16 br = w->cls ? w->cls->bg : 0;
			struct rect rr;

			if (!br || !dc)
				return 0;
			if (br <= NSYSCOLOR + 1 && br >= 1)
				br = sys_brush(br - 1);
			rr = w->cr;
			d_fill(dc, &rr, br, 0xf0);
			return 1;
		}
	case WM_SETCURSOR:
		ht = (short)LO16(lp);
		if ((w->style & WS_CHILD) && w->parent && w->parent != desktop &&
		    wnd_send(w->parent, WM_SETCURSOR, wp, lp))
			return 1;
		if (ht == HTCLIENT) {
			if (w->cls && w->cls->cursor)
				cur_set(w->cls->cursor);
			return 0;
		}
		switch (ht) {
		case HTLEFT: case HTRIGHT: cur_set(cur_load(0, 32644)); break;
		case HTTOP: case HTBOTTOM: cur_set(cur_load(0, 32645)); break;
		case HTTOPLEFT: case HTBOTTOMRIGHT: cur_set(cur_load(0, 32642)); break;
		case HTTOPRIGHT: case HTBOTTOMLEFT: cur_set(cur_load(0, 32643)); break;
		default: cur_set(cur_arrow);
		}
		return 0;
	case WM_MOUSEACTIVATE:
		if ((w->style & WS_CHILD) && w->parent && w->parent != desktop) {
			r = wnd_send(w->parent, WM_MOUSEACTIVATE, wp, lp);
			if (r)
				return r;
		}
		return 1;
	case WM_NCLBUTTONDOWN:
		ht = (short)wp;
		switch (ht) {
		case HTCAPTION:
			if (wnd_toplevel(w) != wnd_active)
				wnd_activate(w, WA_CLICKACTIVE);
			if (w->style & WS_MINIMIZE)
				return 0;
			movesize(w, HTCAPTION, (short)LO16(lp), (short)HI16(lp));
			return 0;
		case HTSYSMENU:
			syscommand(w, SC_MOUSEMENU + HTSYSMENU, lp);
			return 0;
		case HTMINBUTTON:
			if (trackbutton(w, 1))
				wnd_send(w, WM_SYSCOMMAND, SC_MINIMIZE, lp);
			return 0;
		case HTMAXBUTTON:
			if (trackbutton(w, 2))
				wnd_send(w, WM_SYSCOMMAND, (w->style & WS_MAXIMIZE) ? SC_RESTORE : SC_MAXIMIZE, lp);
			return 0;
		case HTMENU:
			menu_trackbar(w, menu_barhit(w, (short)LO16(lp), (short)HI16(lp)), 0);
			return 0;
		case HTVSCROLL:
		case HTHSCROLL:
			{
				struct rect sr;
				int sb = sys_metric(SM_CXVSCROLL), v = ht == HTVSCROLL;

				if (v)
					r_set(&sr, w->cr.r - 1, w->cr.t - 1, w->cr.r + sb - 1, w->cr.b + 1);
				else
					r_set(&sr, w->cr.l - 1, w->cr.b - 1, w->cr.r + 1, w->cr.b + sb - 1);
				sb_track(w, 0, &sr, v, &w->sb[v], (short)LO16(lp), (short)HI16(lp));
			}
			return 0;
		case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
		case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT: case HTBOTTOMRIGHT: case HTSIZE:
			movesize(w, ht, (short)LO16(lp), (short)HI16(lp));
			return 0;
		}
		return 0;
	case WM_NCLBUTTONDBLCLK:
		if (wp == HTCAPTION && (w->style & WS_MAXIMIZEBOX))
			wnd_send(w, WM_SYSCOMMAND, (w->style & (WS_MAXIMIZE | WS_MINIMIZE)) ? SC_RESTORE : SC_MAXIMIZE, lp);
		else if (wp == HTCAPTION && (w->style & WS_MINIMIZE))
			wnd_send(w, WM_SYSCOMMAND, SC_RESTORE, lp);
		else if (wp == HTSYSMENU)
			wnd_send(w, WM_SYSCOMMAND, SC_CLOSE, lp);
		return 0;
	case WM_SYSCOMMAND:
		return syscommand(w, wp, lp);
	case WM_CLOSE:
		wnd_destroy(w);
		return 0;
	case WM_SYSKEYDOWN:
		if (wp == VK_F1 + 3 && (lp & 0x20000000)) {
			wnd_send(wnd_toplevel(w), WM_SYSCOMMAND, SC_CLOSE, 0);
			return 0;
		}
		return 0;
	case WM_SYSKEYUP:
		if ((wp == VK_MENU || wp == VK_F1 + 9) && !(w->style & WS_CHILD))
			wnd_send(w, WM_SYSCOMMAND, SC_KEYMENU, 0);
		return 0;
	case WM_SYSCHAR:
		if (wp == ' ' && (wnd_toplevel(w)->style & WS_SYSMENU)) {
			struct wnd *t = wnd_toplevel(w);

			menu_track(t->sysmenu ? t->sysmenu : sysmenu_of(t), 0x0100, t->wr.l + frame_width(t),
			    t->wr.t + frame_width(t) + sys_metric(SM_CYCAPTION) - 1, t, -1);
			return 0;
		}
		if (wnd_toplevel(w)->id && !(wnd_toplevel(w)->style & WS_CHILD))
			wnd_send(wnd_toplevel(w), WM_SYSCOMMAND, SC_KEYMENU, wp);
		else
			scr_beep();
		return 0;
	case WM_KEYDOWN:
		if (wp == VK_F1 + 9) {
			wnd_send(wnd_toplevel(w), WM_SYSCOMMAND, SC_KEYMENU, 0);
			return 0;
		}
		return 0;
	case WM_CTLCOLOR:
		{
			struct dc *dc = dc_get(wp);
			int type = HI16(lp);

			if (type == 3) {			/* CTLCOLOR_SCROLLBAR */
				if (dc) {
					dc->st.bk = RGB(255, 255, 255);
					dc->st.text = 0;
				}
				return sys_brush(COLOR_SCROLLBAR);
			}
			if (dc) {
				dc->st.text = sys_color(COLOR_WINDOWTEXT);
				dc->st.bk = sys_color(COLOR_WINDOW);
			}
			return sys_brush(COLOR_WINDOW);
		}
	case WM_WINDOWPOSCHANGED:
		{
			u32 l = lin(FPSEL(lp), FPOFF(lp));
			int fl = l ? GW(l + 12) : 0;

			if (!(fl & SWP_NOMOVE))
				wnd_send(w, WM_MOVE, 0, FP(w->cr.t - (w->parent == desktop ? 0 : w->parent->cr.t),
				    w->cr.l - (w->parent == desktop ? 0 : w->parent->cr.l)));
			if (!(fl & SWP_NOSIZE))
				wnd_send(w, WM_SIZE, (w->style & WS_MAXIMIZE) ? 2 : (w->style & WS_MINIMIZE) ? 1 : 0,
				    FP(w->cr.b - w->cr.t, w->cr.r - w->cr.l));
		}
		return 0;
	case WM_QUERYOPEN:
	case WM_QUERYENDSESSION:
		return 1;
	case WM_VKEYTOITEM:
	case WM_CHARTOITEM:
		return 0xffff;
	case WM_CANCELMODE:
		if (wnd_capture == w)
			wnd_capture = 0;
		return 0;
	case WM_GETFONT:
		return 0;
	case WM_QUERYDRAGICON:
		return w->cls ? w->cls->icon : 0;
	}
	return 0;
}

/* a native window procedure's WM_PAINT and others: what DefWindowProc gives them */
u32
native_defproc(a)
	u32 *a;
{
	return user_defproc(wnd_get(a[0]), a[1], a[2], a[3]);
}
