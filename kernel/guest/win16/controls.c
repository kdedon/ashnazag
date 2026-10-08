/*
 * controls.c -- the standard controls but the edit control (edit.c):
 * BUTTON (push buttons, check boxes, radio buttons, group boxes),
 * STATIC, LISTBOX and COMBOBOX, drawn in the Windows 3.1 manner, with
 * their messages and notifications.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "win.h"
#include "scr.h"

extern int user_poll();

extern u32 ualloc(), ulin(), ustr(), thunk_internal();
extern void ufree(), wnd_setfocus(), user_flushpaint(), user_endpaint();
extern u16 user_beginpaint();
extern int IsChildOf();
extern u8 keystate[];
extern u32 edit_proc();

#define	STR(p)		(gptr(p) ? gptr(p) : "")

/* ---- shared ---- */

/* the parent's colours for a control: WM_CTLCOLOR, a brush back */
static u16
ctlcolor(w, hdc, type)
	struct wnd *w;
	u32 hdc;
	int type;
{
	u16 br = 0;

	if (w->parent && w->parent != desktop)
		br = wnd_send(w->parent, WM_CTLCOLOR, hdc, FP(type, w->h));
	if (!br) {
		struct dc *dc = dc_get(hdc);

		if (dc) {
			dc->st.text = sys_color(COLOR_WINDOWTEXT);
			dc->st.bk = sys_color(COLOR_WINDOW);
		}
		br = sys_brush(COLOR_WINDOW);
	}
	return br;
}

static void
setfont(dc, w)
	struct dc *dc;
	struct wnd *w;
{
	if (w->dlgfont && gobj(w->dlgfont, OBJ_FONT))
		dc->st.font = w->dlgfont;
	else
		dc->st.font = stockobj[SYSTEM_FONT];
}

static void
notify(w, code)
	struct wnd *w;
	int code;
{
	if (w->parent && w->parent != desktop)
		wnd_send(w->parent, WM_COMMAND, w->id, FP(code, w->h));
}

static void
focusrect(dc, r)
	struct dc *dc;
	struct rect *r;
{
	struct rgn *g = dc_clip(dc);
	int x, y;
	u8 *p;

	for (x = r->l; x < r->r; x += 2) {
		if (rgn_ptin(g, x, r->t)) { p = dc->s->pix + r->t * dc->s->rowb + x; *p = ~*p; }
		if (rgn_ptin(g, x, r->b - 1)) { p = dc->s->pix + (r->b - 1) * dc->s->rowb + x; *p = ~*p; }
	}
	for (y = r->t + 2; y < r->b - 1; y += 2) {
		if (rgn_ptin(g, r->l, y)) { p = dc->s->pix + y * dc->s->rowb + r->l; *p = ~*p; }
		if (rgn_ptin(g, r->r - 1, y)) { p = dc->s->pix + y * dc->s->rowb + r->r - 1; *p = ~*p; }
	}
	scr_dirty(r);
}

/* paint through a DC of the client area, outside WM_PAINT too */
static u16
cdc(w)
	struct wnd *w;
{
	return user_dc(w, DCK_WINDOW, (struct rgn *)0);
}

/* ---- buttons ---- */

#define	BS_PUSHBUTTON	0
#define	BS_DEFPUSHBUTTON 1
#define	BS_CHECKBOX	2
#define	BS_AUTOCHECKBOX	3
#define	BS_RADIOBUTTON	4
#define	BS_3STATE	5
#define	BS_AUTO3STATE	6
#define	BS_GROUPBOX	7
#define	BS_USERBUTTON	8
#define	BS_AUTORADIOBUTTON 9
#define	BS_OWNERDRAW	11
#define	BS_LEFTTEXT	0x20

#define	BST_CHECK	3
#define	BST_PUSHED	4
#define	BST_FOCUS	8

static void
btn_paint(w, hdc)
	struct wnd *w;
	u16 hdc;
{
	struct dc *dc = dc_get(hdc);
	int type = w->style & 0x0f, st = w->user, gray = (w->style & WS_DISABLED) != 0;
	struct rect r = w->cr, b, t;
	u16 br;
	int box = 13, black = 0;

	if (!dc)
		return;
	setfont(dc, w);
	dc->st.bkmode = TRANSPARENT;
	switch (type) {
	case BS_OWNERDRAW:
		{
			u32 p = ualloc(26), l = ulin(p);

			PW(l, 4);		/* ODT_BUTTON */
			PW(l + 2, w->id);
			PW(l + 4, 0);
			PW(l + 6, 1);		/* ODA_DRAWENTIRE */
			PW(l + 8, ((st & BST_PUSHED) ? 1 : 0) | ((st & BST_FOCUS) ? 0x10 : 0) | (gray ? 4 : 0));
			PW(l + 10, w->h);
			PW(l + 12, hdc);
			r_set(&t, 0, 0, w->cr.r - w->cr.l, w->cr.b - w->cr.t);
			r_put(l + 14, &t);
			PL(l + 22, 0);
			wnd_send(w->parent, WM_DRAWITEM, w->id, p);
			ufree(p);
		}
		return;
	case BS_PUSHBUTTON:
	case BS_DEFPUSHBUTTON:
	case BS_USERBUTTON:
		{
			int k = type == BS_DEFPUSHBUTTON ? 2 : 1;

			/* the parent's colour in the rounded corners */
			br = ctlcolor(w, hdc, 3);
			d_fill(dc, &r, br, 0xf0);
			r_set(&b, r.l + 1, r.t, r.r - 1, r.t + k);
			d_fillcolor(dc, &b, black);
			r_set(&b, r.l + 1, r.b - k, r.r - 1, r.b);
			d_fillcolor(dc, &b, black);
			r_set(&b, r.l, r.t + 1, r.l + k, r.b - 1);
			d_fillcolor(dc, &b, black);
			r_set(&b, r.r - k, r.t + 1, r.r, r.b - 1);
			d_fillcolor(dc, &b, black);
			r_set(&b, r.l + k, r.t + k, r.r - k, r.b - k);
			draw_3dbox(dc, &b, st & BST_PUSHED);
			dc->st.text = sys_color(gray ? COLOR_GRAYTEXT : COLOR_BTNTEXT);
			t = b;
			if (st & BST_PUSHED) {
				t.l += 2;
				t.t += 2;
			}
			draw_text(dc, w->text, strlen(w->text), &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			if (st & BST_FOCUS) {
				int tw = draw_textw(font_of(dc), w->text, strlen(w->text)), th = font_of(dc)->f_height;

				r_set(&t, (b.l + b.r - tw) / 2 - 2, (b.t + b.b - th) / 2, (b.l + b.r + tw) / 2 + 2,
				    (b.t + b.b + th) / 2);
				if (st & BST_PUSHED) {
					t.l += 2; t.r += 2; t.t += 2; t.b += 2;
				}
				r_and(&t, &t, &b);
				focusrect(dc, &t);
			}
		}
		return;
	case BS_GROUPBOX:
		{
			struct bfont *f;
			int tw;

			br = ctlcolor(w, hdc, 3);
			f = font_of(dc);
			tw = draw_textw(f, w->text, strlen(w->text));
			r_set(&b, r.l, r.t + f->f_height / 2, r.r, r.b);
			d_frame(dc, &b, pal_index(dc->st.text));
			if (*w->text) {
				r_set(&t, r.l + 6, r.t, r.l + 10 + tw, r.t + f->f_height);
				d_fill(dc, &t, br, 0xf0);
				t.l += 2;
				if (gray)
					dc->st.text = sys_color(COLOR_GRAYTEXT);
				draw_text(dc, w->text, strlen(w->text), &t, DT_SINGLELINE);
			}
		}
		return;
	}
	/* check boxes and radio buttons */
	br = ctlcolor(w, hdc, 3);
	d_fill(dc, &r, br, 0xf0);
	if (w->style & BS_LEFTTEXT)
		r_set(&b, r.r - box, (r.t + r.b - box) / 2, r.r, (r.t + r.b - box) / 2 + box);
	else
		r_set(&b, r.l, (r.t + r.b - box) / 2, r.l + box, (r.t + r.b - box) / 2 + box);
	if (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON) {
		struct rect e = b;
		u16 keepb = dc->st.brush, keepp = dc->st.pen;

		dc->st.brush = (st & BST_PUSHED) ? sys_brush(COLOR_BTNSHADOW) : stockobj[WHITE_BRUSH];
		dc->st.pen = stockobj[BLACK_PEN];
		d_ellipse(dc, &e, 1, 1);
		if (st & 3) {
			r_set(&e, b.l + 3, b.t + 3, b.r - 3, b.b - 3);
			dc->st.brush = stockobj[BLACK_BRUSH];
			d_ellipse(dc, &e, 1, 1);
		}
		dc->st.brush = keepb;
		dc->st.pen = keepp;
	} else {
		d_fillcolor(dc, &b, (st & BST_PUSHED) ? pal_index(sys_color(COLOR_BTNSHADOW)) : 255);
		d_frame(dc, &b, black);
		if ((st & 3) == 1 || ((st & 3) == 2)) {
			int i, c = (st & 3) == 2 ? pal_index(sys_color(COLOR_GRAYTEXT)) : black;

			/* an X, as 3.1 drew it */
			for (i = 2; i < box - 2; i++) {
				r_set(&t, b.l + i, b.t + i, b.l + i + 1, b.t + i + 1);
				d_fillcolor(dc, &t, c);
				r_set(&t, b.r - 1 - i, b.t + i, b.r - i, b.t + i + 1);
				d_fillcolor(dc, &t, c);
			}
		}
	}
	t = r;
	if (w->style & BS_LEFTTEXT)
		t.r = b.l - 4;
	else
		t.l = b.r + 4;
	if (gray)
		dc->st.text = sys_color(COLOR_GRAYTEXT);
	draw_text(dc, w->text, strlen(w->text), &t, DT_SINGLELINE | DT_VCENTER);
	if (st & BST_FOCUS) {
		int tw = draw_textw(font_of(dc), w->text, strlen(w->text)), th = font_of(dc)->f_height;

		r_set(&b, t.l - 1, (t.t + t.b - th) / 2 - 1, t.l + tw + 1, (t.t + t.b + th) / 2 + 1);
		r_and(&b, &b, &r);
		focusrect(dc, &b);
	}
}

static void
btn_redraw(w)
	struct wnd *w;
{
	u16 h;

	if (!wnd_visible(w))
		return;
	h = cdc(w);
	btn_paint(w, h);
	user_releasedc(h);
}

/* a radio button checked: the others of its group cleared */
static void
radiogroup(w)
	struct wnd *w;
{
	struct wnd *c, *first = 0, *p = w->parent;

	if (!p)
		return;
	/* the group: from the WS_GROUP at or before w to the next one */
	for (c = p->child; c; c = c->next) {
		if (c->style & WS_GROUP)
			first = c;
		if (c == w)
			break;
	}
	if (!first)
		first = p->child;
	for (c = first; c; c = c->next) {
		if (c != first && (c->style & WS_GROUP))
			break;
		if (c != w && c->cls == w->cls && ((c->style & 0xf) == BS_AUTORADIOBUTTON) && (c->user & 3)) {
			c->user &= ~3;
			btn_redraw(c);
		}
	}
}

static void
btn_click(w)
	struct wnd *w;
{
	int type = w->style & 0xf;

	switch (type) {
	case BS_AUTOCHECKBOX:
		w->user ^= 1;
		break;
	case BS_AUTO3STATE:
		w->user = (w->user & ~3) | (((w->user & 3) + 1) % 3);
		break;
	case BS_AUTORADIOBUTTON:
		w->user |= 1;
		radiogroup(w);
		break;
	}
	btn_redraw(w);
	notify(w, 0);		/* BN_CLICKED */
}

u32
button_proc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	int type, in;

	if (!w)
		return 0;
	type = w->style & 0xf;
	switch (a[1]) {
	case WM_CREATE:
		w->user = 0;
		return 0;
	case WM_PAINT:
		{
			u16 h = user_beginpaint(w, 0);

			btn_paint(w, h);
			user_releasedc(h);
			user_endpaint(w, 0);
		}
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_NCHITTEST:
		if (type == BS_GROUPBOX)
			return (u32)HTTRANSPARENT & 0xffff;
		break;
	case WM_GETDLGCODE:
		switch (type) {
		case BS_PUSHBUTTON: return 0x2000 | 0x20;
		case BS_DEFPUSHBUTTON: return 0x2000 | 0x10;
		case BS_RADIOBUTTON: case BS_AUTORADIOBUTTON: return 0x2000 | 0x40;
		case BS_GROUPBOX: return 0x100;
		}
		return 0x2000;
	case WM_SETTEXT:
		user_defproc(w, a[1], a[2], a[3]);
		btn_redraw(w);
		return 1;
	case WM_SETFONT:
		w->dlgfont = a[2];
		if (LO16(a[3]))
			btn_redraw(w);
		return 0;
	case WM_GETFONT:
		return w->dlgfont;
	case WM_ENABLE:
		btn_redraw(w);
		return 0;
	case WM_SETFOCUS:
		if (type != BS_GROUPBOX) {
			w->user |= BST_FOCUS;
			btn_redraw(w);
		}
		return 0;
	case WM_KILLFOCUS:
		w->user &= ~BST_FOCUS;
		if (w->user & BST_PUSHED) {
			w->user &= ~BST_PUSHED;
			if (wnd_capture == w)
				wnd_capture = 0;
		}
		btn_redraw(w);
		return 0;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		if (type == BS_GROUPBOX)
			return 0;
		wnd_setfocus(w);
		wnd_capture = w;
		w->user |= BST_PUSHED;
		btn_redraw(w);
		return 0;
	case WM_MOUSEMOVE:
		if (wnd_capture != w)
			return 0;
		in = (short)LO16(a[3]) >= 0 && (short)LO16(a[3]) < w->cr.r - w->cr.l &&
		    (short)HI16(a[3]) >= 0 && (short)HI16(a[3]) < w->cr.b - w->cr.t;
		if (in != ((w->user & BST_PUSHED) != 0)) {
			w->user ^= BST_PUSHED;
			btn_redraw(w);
		}
		return 0;
	case WM_LBUTTONUP:
		if (wnd_capture != w)
			return 0;
		wnd_capture = 0;
		in = (w->user & BST_PUSHED) != 0;
		w->user &= ~BST_PUSHED;
		btn_redraw(w);
		if (in && wnd_get(a[0]))
			btn_click(w);
		return 0;
	case WM_KEYDOWN:
		if (a[2] == VK_SPACE && !(w->user & BST_PUSHED)) {
			w->user |= BST_PUSHED;
			btn_redraw(w);
		}
		return 0;
	case WM_KEYUP:
		if (a[2] == VK_SPACE && (w->user & BST_PUSHED)) {
			w->user &= ~BST_PUSHED;
			btn_redraw(w);
			btn_click(w);
		}
		return 0;
	case WM_CHAR:
		if ((type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON) && (a[2] == '+' || a[2] == '='))
			btn_click(w);
		return 0;
	case WM_USER + 0:		/* BM_GETCHECK */
		return w->user & 3;
	case WM_USER + 1:		/* BM_SETCHECK */
		if (type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON && type != BS_GROUPBOX) {
			w->user = (w->user & ~3) | (a[2] & 3);
			btn_redraw(w);
		}
		return 0;
	case WM_USER + 2:		/* BM_GETSTATE */
		return w->user & 0x0f;
	case WM_USER + 3:		/* BM_SETSTATE */
		if (a[2])
			w->user |= BST_PUSHED;
		else
			w->user &= ~BST_PUSHED;
		btn_redraw(w);
		return 0;
	case WM_USER + 4:		/* BM_SETSTYLE */
		w->style = (w->style & ~0xff) | (a[2] & 0xff);
		if (LO16(a[3]))
			btn_redraw(w);
		return 0;
	}
	return user_defproc(w, a[1], a[2], a[3]);
}

/* ---- statics ---- */

#define	SS_ICON		3
#define	SS_NOPREFIX	0x80

static void
st_paint(w, hdc)
	struct wnd *w;
	u16 hdc;
{
	struct dc *dc = dc_get(hdc);
	int type = w->style & 0x0f, fl;
	struct rect r = w->cr;
	u16 br;

	if (!dc)
		return;
	setfont(dc, w);
	switch (type) {
	case 4: case 5: case 6:		/* black, grey, white rectangles */
		d_fillcolor(dc, &r, type == 4 ? 0 : type == 5 ? pal_index(sys_color(COLOR_BTNSHADOW)) : 255);
		return;
	case 7: case 8: case 9:		/* frames */
		br = ctlcolor(w, hdc, 6);
		d_frame(dc, &r, type == 7 ? 0 : type == 8 ? pal_index(sys_color(COLOR_BTNSHADOW)) : 255);
		return;
	case SS_ICON:
		br = ctlcolor(w, hdc, 6);
		d_fill(dc, &r, br, 0xf0);
		if (w->hicon)
			icon_draw(dc, w->hicon, r.l, r.t);
		return;
	}
	br = ctlcolor(w, hdc, 6);
	d_fill(dc, &r, br, 0xf0);
	dc->st.bkmode = TRANSPARENT;
	if (w->style & WS_DISABLED)
		dc->st.text = sys_color(COLOR_GRAYTEXT);
	fl = (w->style & SS_NOPREFIX) ? DT_NOPREFIX : 0;
	switch (type) {
	case 1: fl |= DT_CENTER | DT_WORDBREAK; break;
	case 2: fl |= DT_RIGHT | DT_WORDBREAK; break;
	case 0xb: fl |= DT_SINGLELINE; break;
	case 0xc: fl |= DT_EXPANDTABS; break;
	default: fl |= DT_WORDBREAK | DT_EXPANDTABS; break;
	}
	draw_text(dc, w->text, strlen(w->text), &r, fl);
}

u32
static_proc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	u16 h;

	if (!w)
		return 0;
	switch (a[1]) {
	case WM_CREATE:
		if ((w->style & 0xf) == SS_ICON) {
			u32 cs = lin(FPSEL(a[3]), FPOFF(a[3])), name;

			name = cs ? GL(cs + 22) : 0;
			if (w->text[0]) {
				u32 s = ustr(w->text);

				w->hicon = icon_load(w->hinst, s);
				ufree(s);
			} else if (name && FPSEL(name) == 0)
				w->hicon = icon_load(w->hinst, name);
			if (w->hicon)
				wnd_setpos(w, (struct wnd *)0, 0, 0, 32, 32, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
		return 0;
	case WM_PAINT:
		h = user_beginpaint(w, 0);
		st_paint(w, h);
		user_releasedc(h);
		user_endpaint(w, 0);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_NCHITTEST:
		return (u32)HTTRANSPARENT & 0xffff;
	case WM_GETDLGCODE:
		return 0x100;		/* DLGC_STATIC */
	case WM_SETTEXT:
		user_defproc(w, a[1], a[2], a[3]);
		wnd_invalidate(w, (struct rect *)0, 1);
		return 1;
	case WM_SETFONT:
		w->dlgfont = a[2];
		if (LO16(a[3]))
			wnd_invalidate(w, (struct rect *)0, 1);
		return 0;
	case WM_GETFONT:
		return w->dlgfont;
	case WM_ENABLE:
		wnd_invalidate(w, (struct rect *)0, 1);
		return 0;
	case WM_USER + 0:		/* STM_SETICON */
		h = w->hicon;
		w->hicon = a[2];
		wnd_invalidate(w, (struct rect *)0, 1);
		return h;
	case WM_USER + 1:		/* STM_GETICON */
		return w->hicon;
	}
	return user_defproc(w, a[1], a[2], a[3]);
}

/* ---- list boxes ---- */

#define	LBS_NOTIFY	0x0001
#define	LBS_SORT	0x0002
#define	LBS_NOREDRAW	0x0004
#define	LBS_MULTIPLESEL	0x0008
#define	LBS_OWNERDRAWFIXED 0x0010
#define	LBS_OWNERDRAWVARIABLE 0x0020
#define	LBS_HASSTRINGS	0x0040
#define	LBS_USETABSTOPS	0x0080
#define	LBS_NOINTEGRALHEIGHT 0x0100
#define	LBS_MULTICOLUMN	0x0200
#define	LBS_WANTKEYBOARDINPUT 0x0400
#define	LBS_EXTENDEDSEL	0x0800

#define	LB_ERR		0xffff

struct lbitem {
	char	*s;
	u32	data;
	int	sel;
};

struct lb {
	int	n, max;
	struct lbitem *it;
	int	top, cur, anchor, ih;
	int	combo;		/* the list of a combo box: notifies it */
	struct wnd *owner;	/* the combo box */
};

static struct lb *
lbof(w)
	struct wnd *w;
{
	if (!w->priv) {
		struct lb *l = (struct lb *)calloc(1, sizeof(struct lb));

		l->cur = -1;
		l->anchor = -1;
		w->priv = l;
	}
	return (struct lb *)w->priv;
}

static int
hasstrings(w)
	struct wnd *w;
{
	return !(w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) || (w->style & LBS_HASSTRINGS);
}

static int
itemh(w)
	struct wnd *w;
{
	struct lb *l = lbof(w);
	struct gobj *o;

	if (l->ih)
		return l->ih;
	o = gobj(w->dlgfont ? w->dlgfont : stockobj[SYSTEM_FONT], OBJ_FONT);
	l->ih = o ? o->u.font.bf->f_height : 16;
	if (w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) {
		/* WM_MEASUREITEM: CtlType, CtlID, itemID, itemWidth, itemHeight, itemData */
		u32 p = ualloc(16), q = ulin(p);

		PW(q, 2);		/* ODT_LISTBOX */
		PW(q + 2, w->id);
		PW(q + 4, 0);
		PW(q + 6, w->cr.r - w->cr.l);
		PW(q + 8, l->ih);
		PL(q + 10, 0);
		wnd_send(l->combo ? l->owner->parent : w->parent, WM_MEASUREITEM, w->id, p);
		if (GW(q + 8))
			l->ih = GW(q + 8);
		ufree(p);
	}
	return l->ih;
}

static int
visible_rows(w)
	struct wnd *w;
{
	int h = itemh(w);

	return h ? (w->cr.b - w->cr.t) / h : 1;
}

static void
lb_notify(w, code)
	struct wnd *w;
	int code;
{
	struct lb *l = lbof(w);

	if (l->combo) {
		wnd_send(l->owner, WM_USER + 0x7f00, code, 0);
		return;
	}
	if (w->style & LBS_NOTIFY)
		notify(w, code);
}

static void
lb_scrollbar(w)
	struct wnd *w;
{
	struct lb *l = lbof(w);
	int rows = visible_rows(w), max = l->n - rows;

	if (!(w->style & WS_VSCROLL))
		return;
	w->sb[1].min = 0;
	w->sb[1].max = max > 0 ? max : 0;
	w->sb[1].pos = l->top;
	wnd_redrawframe(w);
}

static void
lb_drawitem(w, dc, i, r)
	struct wnd *w;
	struct dc *dc;
	int i;
	struct rect *r;
{
	struct lb *l = lbof(w);
	int sel = i >= 0 && i < l->n && (l->it[i].sel || (!(w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) && i == l->cur));
	struct rect t;

	if (w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) {
		u32 p, q;
		u16 br;

		/* the item erased first, in the parent's list box brush, as Windows does */
		br = ctlcolor(l->combo ? l->owner : w, dc->h, 2);
		d_fill(dc, r, br ? br : stockobj[WHITE_BRUSH], 0xf0);
		p = ualloc(26);
		q = ulin(p);
		PW(q, 2);
		PW(q + 2, w->id);
		PW(q + 4, i);
		PW(q + 6, 1);
		PW(q + 8, (sel ? 1 : 0) | (i == l->cur && wnd_focus == w ? 0x10 : 0));
		PW(q + 10, l->combo ? l->owner->h : w->h);
		PW(q + 12, dc->h);
		t = *r;
		t.l -= dc->ox; t.r -= dc->ox; t.t -= dc->oy; t.b -= dc->oy;
		r_put(q + 14, &t);
		PL(q + 22, i >= 0 && i < l->n ? l->it[i].data : 0);
		wnd_send(l->combo ? l->owner->parent : w->parent, WM_DRAWITEM, w->id, p);
		ufree(p);
		return;
	}
	d_fillcolor(dc, r, pal_index(sys_color(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW)));
	if (i < 0 || i >= l->n)
		return;
	dc->st.text = sys_color(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
	dc->st.bkmode = TRANSPARENT;
	t = *r;
	t.l += 2;
	if (l->it[i].s)
		draw_text(dc, l->it[i].s, strlen(l->it[i].s), &t, DT_SINGLELINE | DT_NOPREFIX |
		    ((w->style & LBS_USETABSTOPS) ? DT_EXPANDTABS : 0));
	if (i == l->cur && wnd_focus == w && (w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)))
		focusrect(dc, r);
}

static void
lb_paint(w, hdc)
	struct wnd *w;
	u16 hdc;
{
	struct dc *dc = dc_get(hdc);
	struct lb *l = lbof(w);
	int i, h = itemh(w), y;
	struct rect r;
	u16 br;

	if (!dc)
		return;
	setfont(dc, w);
	br = l->combo ? sys_brush(COLOR_WINDOW) : ctlcolor(w, hdc, 2);
	for (i = l->top, y = w->cr.t; y < w->cr.b; i++, y += h) {
		r_set(&r, w->cr.l, y, w->cr.r, y + h);
		if (i < l->n)
			lb_drawitem(w, dc, i, &r);
		else
			d_fill(dc, &r, br, 0xf0);
	}
}

static void
lb_redraw(w)
	struct wnd *w;
{
	u16 h;

	if (!wnd_visible(w) || (w->style & LBS_NOREDRAW))
		return;
	h = cdc(w);
	lb_paint(w, h);
	user_releasedc(h);
}

static int
lb_insert(w, pos, p)
	struct wnd *w;
	int pos;
	u32 p;
{
	struct lb *l = lbof(w);
	struct lbitem *it;
	char *s = hasstrings(w) ? STR(p) : 0;
	int i;

	if (l->n == l->max) {
		l->max = l->max ? l->max * 2 : 16;
		l->it = (struct lbitem *)realloc(l->it, l->max * sizeof *l->it);
	}
	if (pos < 0 || pos > l->n) {
		pos = l->n;
		if ((w->style & LBS_SORT) && s)
			for (pos = 0; pos < l->n && l->it[pos].s && w16_stricmp(l->it[pos].s, s) <= 0; pos++)
				;
		else if ((w->style & LBS_SORT) && (w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE))) {
			/* the owner's order (WM_COMPAREITEM), by halves */
			int lo = 0, hi = l->n, mid;
			u32 cp = ualloc(18), q = ulin(cp);

			while (lo < hi) {
				mid = (lo + hi) / 2;
				PW(q, 2);		/* ODT_LISTBOX */
				PW(q + 2, w->id);
				PW(q + 4, w->h);
				PW(q + 6, mid);
				PL(q + 8, l->it[mid].data);
				PW(q + 12, (u16)-1);
				PL(q + 14, p);
				if ((short)wnd_send(l->combo ? l->owner->parent : w->parent, WM_COMPAREITEM, w->id, cp) <= 0)
					lo = mid + 1;
				else
					hi = mid;
				q = ulin(cp);
			}
			ufree(cp);
			pos = lo;
		}
	}
	memmove(&l->it[pos + 1], &l->it[pos], (l->n - pos) * sizeof *l->it);
	l->n++;
	it = &l->it[pos];
	it->s = s ? strdup(s) : 0;
	it->data = s ? 0 : p;
	it->sel = 0;
	if (l->cur >= pos)
		l->cur++;
	(void)i;
	lb_scrollbar(w);
	lb_redraw(w);
	return pos;
}

static void
lb_delete(w, i)
	struct wnd *w;
	int i;
{
	struct lb *l = lbof(w);

	if (w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) {
		u32 p = ualloc(12), q = ulin(p);

		PW(q, 2);
		PW(q + 2, w->id);
		PW(q + 4, i);
		PW(q + 6, w->h);
		PL(q + 8, l->it[i].data);
		wnd_send(w->parent, WM_DELETEITEM, w->id, p);
		ufree(p);
	}
	if (l->it[i].s)
		free(l->it[i].s);
	memmove(&l->it[i], &l->it[i + 1], (l->n - i - 1) * sizeof *l->it);
	l->n--;
	if (l->cur == i)
		l->cur = -1;
	else if (l->cur > i)
		l->cur--;
	if (l->top >= l->n && l->top > 0)
		l->top = l->n - 1;
}

static void
lb_show(w, i)
	struct wnd *w;
	int i;
{
	struct lb *l = lbof(w);
	int rows = visible_rows(w);

	if (i < 0)
		return;
	if (i < l->top)
		l->top = i;
	else if (i >= l->top + rows)
		l->top = i - rows + 1;
	if (l->top < 0)
		l->top = 0;
	lb_scrollbar(w);
}

/* owner-drawn without strings: the item whose data is this, after start */
static int
lb_finddata(w, start, data)
	struct wnd *w;
	int start;
	u32 data;
{
	struct lb *l = lbof(w);
	int i, k;

	for (k = 0; k < l->n; k++) {
		i = (start + 1 + k) % l->n;
		if (i >= 0 && l->it[i].data == data)
			return i;
	}
	return -1;
}

static int
lb_find(w, start, s, exact)
	struct wnd *w;
	int start, exact;
	char *s;
{
	struct lb *l = lbof(w);
	int i, k, n = strlen(s);

	for (k = 0; k < l->n; k++) {
		i = (start + 1 + k) % l->n;
		if (!l->it[i].s)
			continue;
		if (exact ? w16_stricmp(l->it[i].s, s) == 0 : w16_strnicmp(l->it[i].s, s, n) == 0)
			return i;
	}
	return -1;
}

/* the item under client y */
static int
lb_at(w, y)
	struct wnd *w;
	int y;
{
	struct lb *l = lbof(w);
	int i = l->top + y / itemh(w);

	return y < 0 ? l->top - 1 : i;
}

static void
lb_select(w, i, notifyit)
	struct wnd *w;
	int i, notifyit;
{
	struct lb *l = lbof(w);

	if (i < 0 || i >= l->n)
		return;
	if (w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) {
		l->cur = i;
	} else {
		if (l->cur == i)
			return;
		l->cur = i;
	}
	lb_show(w, i);
	lb_redraw(w);
	if (notifyit)
		lb_notify(w, 1);	/* LBN_SELCHANGE */
}

/* LB_DIR: files matching a DOS pattern, and directories and drives as asked */
static int
lb_dir(w, attr, pat)
	struct wnd *w;
	int attr;
	char *pat;
{
	char full[300], host[1024], *slash, dn[16], buf[32];
	DIR *dp;
	struct dirent *de;
	int n = 0, d;
	extern int dos_isdir();

	if (dos_fullpath(pat, full) != 0)
		return -1;
	slash = strrchr(full, '\\');
	*slash = 0;
	if (dos_hostpath(full[2] ? full : "C:\\", host, sizeof host, 0) != 0 && full[2])
		return -1;
	if (!full[2])
		strcpy(full + 2, "\\");
	if ((dp = opendir(host)) == 0)
		return -1;
	while ((de = readdir(dp)) != 0) {
		char path[1300];
		int isdir;
		extern int dos_83();

		if (!dos_83(de->d_name, dn))
			continue;
		sprintf(path, "%s/%s", host, de->d_name);
		isdir = dos_isdir(path);
		if (isdir) {
			if (!(attr & 0x10))
				continue;
			sprintf(buf, "[%s]", dn);
			w16_upper(buf);
		} else {
			extern int dos_wild();

			if ((attr & 0x8000) || !dos_wild(slash + 1, dn))
				continue;
			strcpy(buf, dn);
			if (w->style & 0)
				;
		}
		{
			u32 s = ustr(buf);
			int old = w->style;

			w->style |= LBS_SORT;
			lb_insert(w, -1, s);
			w->style = old;
			ufree(s);
			n++;
		}
	}
	closedir(dp);
	if ((attr & 0x10) && strlen(full) > 3) {
		u32 s = ustr("[..]");

		lb_insert(w, 0, s);
		ufree(s);
	}
	if (attr & 0x4000)
		for (d = 0; d < 26; d++)
			if (drive_root[d]) {
				u32 s;

				sprintf(buf, "[-%c-]", 'a' + d);
				s = ustr(buf);
				lb_insert(w, -1, s);
				ufree(s);
			}
	return n;
}

u32
listbox_proc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	struct lb *l;
	int i, n, rows, y;
	u32 p;

	if (!w)
		return 0;
	l = lbof(w);
	switch (a[1]) {
	case WM_CREATE:
		{
			u32 cs = lin(FPSEL(a[3]), FPOFF(a[3]));

			/* a combo box's own list says so in its create data */
			if (cs && GL(cs) == 0x434f4d42) {
				l->combo = 1;
				l->owner = wnd_get(GW(cs + 8));
				if (!l->owner && w->owner)
					l->owner = w->owner;
			}
		}
		return 0;
	case WM_NCDESTROY:
		for (i = 0; i < l->n; i++)
			if (l->it[i].s)
				free(l->it[i].s);
		free(l->it);
		free(l);
		w->priv = 0;
		return 0;
	case WM_PAINT:
		{
			u16 h = user_beginpaint(w, 0);

			lb_paint(w, h);
			user_releasedc(h);
			user_endpaint(w, 0);
		}
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_SIZE:
		lb_scrollbar(w);
		return 0;
	case WM_SETFONT:
		w->dlgfont = a[2];
		l->ih = 0;
		if (LO16(a[3]))
			lb_redraw(w);
		return 0;
	case WM_GETFONT:
		return w->dlgfont;
	case WM_GETDLGCODE:
		return 0x0001 | 0x0080;	/* arrows, chars */
	case WM_SETFOCUS:
		lb_redraw(w);
		lb_notify(w, 4);
		return 0;
	case WM_KILLFOCUS:
		lb_redraw(w);
		lb_notify(w, 5);
		return 0;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		if (!l->combo)
			wnd_setfocus(w);
		i = lb_at(w, (short)HI16(a[3]));
		if (i < 0 || i >= l->n)
			return 0;
		if (w->style & LBS_MULTIPLESEL)
			l->it[i].sel = !l->it[i].sel;
		else if (w->style & LBS_EXTENDEDSEL) {
			if (!(a[2] & 8)) {
				for (n = 0; n < l->n; n++)
					l->it[n].sel = 0;
			}
			if ((a[2] & 4) && l->anchor >= 0) {
				for (n = l->anchor < i ? l->anchor : i; n <= (l->anchor < i ? i : l->anchor); n++)
					l->it[n].sel = 1;
			} else {
				l->it[i].sel = !(a[2] & 8) || !l->it[i].sel;
				l->anchor = i;
			}
		}
		l->cur = -2;
		lb_select(w, i, 1);
		if (a[1] == WM_LBUTTONDBLCLK)
			lb_notify(w, 2);	/* LBN_DBLCLK */
		else
			wnd_capture = w;
		return 0;
	case WM_MOUSEMOVE:
		if (wnd_capture != w || (w->style & LBS_MULTIPLESEL))
			return 0;
		y = (short)HI16(a[3]);
		i = lb_at(w, y);
		if (i >= l->n)
			i = l->n - 1;
		if (i < 0)
			i = 0;
		if (y < 0 && l->top > 0)
			i = l->top - 1;
		if (i != l->cur && i >= 0) {
			if (w->style & LBS_EXTENDEDSEL) {
				for (n = 0; n < l->n; n++)
					l->it[n].sel = l->anchor >= 0 && n >= (l->anchor < i ? l->anchor : i) &&
					    n <= (l->anchor < i ? i : l->anchor);
			}
			lb_select(w, i, 1);
		}
		return 0;
	case WM_LBUTTONUP:
		if (wnd_capture == w)
			wnd_capture = 0;
		return 0;
	case WM_KEYDOWN:
		if ((w->style & LBS_WANTKEYBOARDINPUT) && !l->combo) {
			i = (short)wnd_send(w->parent, WM_VKEYTOITEM, a[2], FP(w->h, l->cur));
			if (i == -2)
				return 0;
			if (i >= 0) {
				lb_select(w, i, 1);
				return 0;
			}
		}
		rows = visible_rows(w);
		i = l->cur;
		switch (a[2]) {
		case VK_UP: i--; break;
		case VK_DOWN: i++; break;
		case VK_PRIOR: i -= rows - 1; break;
		case VK_NEXT: i += rows - 1; break;
		case VK_HOME: i = 0; break;
		case VK_END: i = l->n - 1; break;
		case VK_SPACE:
			if ((w->style & LBS_MULTIPLESEL) && l->cur >= 0) {
				l->it[l->cur].sel = !l->it[l->cur].sel;
				lb_redraw(w);
				lb_notify(w, 1);
			}
			return 0;
		case VK_RETURN:
			if (l->combo)
				lb_notify(w, 2);
			return 0;
		default:
			return 0;
		}
		if (i < 0) i = 0;
		if (i >= l->n) i = l->n - 1;
		if (i >= 0) {
			if (w->style & LBS_EXTENDEDSEL) {
				for (n = 0; n < l->n; n++)
					l->it[n].sel = n == i;
				l->anchor = i;
			}
			lb_select(w, i, 1);
		}
		return 0;
	case WM_CHAR:
		if (a[2] > ' ' && hasstrings(w)) {
			char s[2];

			s[0] = a[2];
			s[1] = 0;
			i = lb_find(w, l->cur, s, 0);
			if (i >= 0)
				lb_select(w, i, 1);
		}
		return 0;
	case WM_VSCROLL:
		rows = visible_rows(w);
		switch (LO16(a[2])) {
		case 0: l->top--; break;
		case 1: l->top++; break;
		case 2: l->top -= rows; break;
		case 3: l->top += rows; break;
		case 4: case 5: l->top = LO16(a[3]); break;
		default: return 0;
		}
		if (l->top > l->n - rows) l->top = l->n - rows;
		if (l->top < 0) l->top = 0;
		lb_scrollbar(w);
		lb_redraw(w);
		return 0;
	case WM_ENABLE:
		lb_redraw(w);
		return 0;
	/* LB_* */
	case WM_USER + 1:	/* LB_ADDSTRING */
		return lb_insert(w, -1, a[3]);
	case WM_USER + 2:	/* LB_INSERTSTRING */
		return lb_insert(w, (short)a[2] < 0 ? l->n : (short)a[2], a[3]);
	case WM_USER + 3:	/* LB_DELETESTRING */
		if ((short)a[2] < 0 || (short)a[2] >= l->n)
			return LB_ERR;
		lb_delete(w, (short)a[2]);
		lb_scrollbar(w);
		lb_redraw(w);
		return l->n;
	case WM_USER + 5:	/* LB_RESETCONTENT */
		while (l->n)
			lb_delete(w, l->n - 1);
		l->top = 0;
		l->cur = -1;
		lb_scrollbar(w);
		lb_redraw(w);
		return 0;
	case WM_USER + 6:	/* LB_SETSEL */
		if (!(w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)))
			return LB_ERR;
		i = (short)LO16(a[3]);
		for (n = 0; n < l->n; n++)
			if (i == -1 || n == i)
				l->it[n].sel = a[2] != 0;
		lb_redraw(w);
		return 0;
	case WM_USER + 7:	/* LB_SETCURSEL */
		i = (short)a[2];
		if (w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL))
			return LB_ERR;
		l->cur = i < l->n ? i : -1;
		if (l->cur >= 0)
			lb_show(w, l->cur);
		lb_redraw(w);
		return i < 0 || i >= l->n ? LB_ERR : i;
	case WM_USER + 8:	/* LB_GETSEL */
		i = (short)a[2];
		if (i < 0 || i >= l->n)
			return LB_ERR;
		return (w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) ? l->it[i].sel : i == l->cur;
	case WM_USER + 9:	/* LB_GETCURSEL */
		return l->cur >= 0 ? l->cur : LB_ERR;
	case WM_USER + 10:	/* LB_GETTEXT */
		i = (short)a[2];
		if (i < 0 || i >= l->n || !(p = lin(FPSEL(a[3]), FPOFF(a[3]))))
			return LB_ERR;
		if (!hasstrings(w)) {
			PL(p, l->it[i].data);
			return 4;
		}
		strcpy((char *)M + p, l->it[i].s);
		return strlen(l->it[i].s);
	case WM_USER + 11:	/* LB_GETTEXTLEN */
		i = (short)a[2];
		if (i < 0 || i >= l->n)
			return LB_ERR;
		return l->it[i].s ? strlen(l->it[i].s) : 4;
	case WM_USER + 12:	/* LB_GETCOUNT */
		return l->n;
	case WM_USER + 13:	/* LB_SELECTSTRING */
		i = hasstrings(w) ? lb_find(w, (short)a[2], STR(a[3]), 0) : lb_finddata(w, (short)a[2], a[3]);
		if (i < 0)
			return LB_ERR;
		l->cur = i;
		lb_show(w, i);
		lb_redraw(w);
		return i;
	case WM_USER + 14:	/* LB_DIR */
		i = lb_dir(w, a[2], STR(a[3]));
		return i < 0 ? LB_ERR : i;
	case WM_USER + 15:	/* LB_GETTOPINDEX */
		return l->top;
	case WM_USER + 16:	/* LB_FINDSTRING */
	case WM_USER + 35:	/* LB_FINDSTRINGEXACT */
		if (!hasstrings(w))
			return (i = lb_finddata(w, (short)a[2], a[3])) < 0 ? LB_ERR : i;
		i = lb_find(w, (short)a[2], STR(a[3]), a[1] == WM_USER + 35);
		return i < 0 ? LB_ERR : i;
	case WM_USER + 17:	/* LB_GETSELCOUNT */
		for (n = 0, i = 0; i < l->n; i++)
			n += l->it[i].sel;
		return n;
	case WM_USER + 18:	/* LB_GETSELITEMS */
		p = lin(FPSEL(a[3]), FPOFF(a[3]));
		for (n = 0, i = 0; p && i < l->n && n < (int)a[2]; i++)
			if (l->it[i].sel)
				PW(p + 2 * n++, i);
		return n;
	case WM_USER + 19:	/* LB_SETTABSTOPS */
		return 1;
	case WM_USER + 20:	/* LB_GETHORIZONTALEXTENT */
	case WM_USER + 21:
	case WM_USER + 22:
		return 0;
	case WM_USER + 24:	/* LB_SETTOPINDEX */
		l->top = (short)a[2];
		if (l->top < 0) l->top = 0;
		lb_scrollbar(w);
		lb_redraw(w);
		return 0;
	case WM_USER + 25:	/* LB_GETITEMRECT */
		p = lin(FPSEL(a[3]), FPOFF(a[3]));
		if (p) {
			struct rect r;

			r_set(&r, 0, ((short)a[2] - l->top) * itemh(w), w->cr.r - w->cr.l,
			    ((short)a[2] - l->top + 1) * itemh(w));
			r_put(p, &r);
		}
		return 1;
	case WM_USER + 26:	/* LB_GETITEMDATA */
		i = (short)a[2];
		return i >= 0 && i < l->n ? l->it[i].data : LB_ERR;
	case WM_USER + 27:	/* LB_SETITEMDATA */
		i = (short)a[2];
		if (i < 0 || i >= l->n)
			return LB_ERR;
		l->it[i].data = a[3];
		return 0;
	case WM_USER + 28:	/* LB_SELITEMRANGE */
		for (i = LO16(a[3]); i <= (int)HI16(a[3]) && i < l->n; i++)
			l->it[i].sel = a[2] != 0;
		lb_redraw(w);
		return 0;
	case WM_USER + 31:	/* LB_SETCARETINDEX */
		l->cur = (short)a[2];
		lb_show(w, l->cur);
		lb_redraw(w);
		return 0;
	case WM_USER + 32:	/* LB_GETCARETINDEX */
		return l->cur;
	case WM_USER + 33:	/* LB_SETITEMHEIGHT */
		l->ih = LO16(a[3]);
		lb_redraw(w);
		return 0;
	case WM_USER + 34:	/* LB_GETITEMHEIGHT */
		return itemh(w);
	}
	return user_defproc(w, a[1], a[2], a[3]);
}

/* ---- combo boxes ---- */

#define	CBS_SIMPLE	1
#define	CBS_DROPDOWN	2
#define	CBS_DROPDOWNLIST 3

struct cb {
	struct wnd *edit, *list;
	int dropped;
	int listh;
};

static struct cb *
cbof(w)
	struct wnd *w;
{
	if (!w->priv)
		w->priv = calloc(1, sizeof(struct cb));
	return (struct cb *)w->priv;
}

static int
cbtype(w)
	struct wnd *w;
{
	return w->style & 3;
}

static int
cbh(w)
	struct wnd *w;
{
	struct gobj *o = gobj(w->dlgfont ? w->dlgfont : stockobj[SYSTEM_FONT], OBJ_FONT);

	return (o ? o->u.font.bf->f_height : 16) + 6;
}

static void
cb_paint(w, hdc)
	struct wnd *w;
	u16 hdc;
{
	struct dc *dc = dc_get(hdc);
	struct cb *c = cbof(w);
	struct rect r, b;
	int h = cbh(w), bw = sys_metric(SM_CXVSCROLL);
	u16 br;
	struct lb *l;

	if (!dc || cbtype(w) == CBS_SIMPLE)
		return;
	setfont(dc, w);
	br = ctlcolor(w, hdc, 1);
	r_set(&r, w->cr.l, w->cr.t, w->cr.r, w->cr.t + h);
	r_set(&b, r.r - bw, r.t, r.r, r.b);
	d_frame(dc, &b, 0);
	b.l++; b.t++; b.r--; b.b--;
	draw_3dbox(dc, &b, c->dropped);
	draw_arrow(dc, (b.l + b.r) / 2 + c->dropped, (b.t + b.b) / 2 + c->dropped, 4, 1, 0);
	if (cbtype(w) == CBS_DROPDOWNLIST) {
		struct rect t;

		r_set(&t, r.l, r.t, r.r - bw + 1, r.b);
		d_frame(dc, &t, 0);
		t.l++; t.t++; t.r--; t.b--;
		l = lbof(c->list);
		if (wnd_focus == w) {
			d_fillcolor(dc, &t, pal_index(sys_color(COLOR_HIGHLIGHT)));
			dc->st.text = sys_color(COLOR_HIGHLIGHTTEXT);
		} else
			d_fill(dc, &t, br, 0xf0);
		dc->st.bkmode = TRANSPARENT;
		if (l->cur >= 0 && l->cur < l->n) {
			struct rect tt = t;

			tt.l += 2;
			if (c->list->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE))
				lb_drawitem(c->list, dc, l->cur, &t);
			else if (l->it[l->cur].s)
				draw_text(dc, l->it[l->cur].s, strlen(l->it[l->cur].s), &tt, DT_SINGLELINE | DT_VCENTER |
				    DT_NOPREFIX);
		}
		if (wnd_focus == w) {
			t.l++; t.t++; t.r--; t.b--;
			focusrect(dc, &t);
		}
	}
}

static void
cb_redraw(w)
	struct wnd *w;
{
	u16 h;

	if (!wnd_visible(w))
		return;
	h = cdc(w);
	cb_paint(w, h);
	user_releasedc(h);
}

/* the edit or display shows the list's selection */
static void
cb_update(w)
	struct wnd *w;
{
	struct cb *c = cbof(w);
	struct lb *l = lbof(c->list);

	if (c->edit && l->cur >= 0 && l->cur < l->n && l->it[l->cur].s) {
		u32 s = ustr(l->it[l->cur].s);

		wnd_send(c->edit, WM_SETTEXT, 0, s);
		wnd_send(c->edit, WM_USER + 1, 0, FP(0x7fff, 0));	/* select all */
		ufree(s);
	}
	cb_redraw(w);
}

static void
cb_dropdown(w)
	struct wnd *w;
{
	struct cb *c = cbof(w);
	struct lb *l = lbof(c->list);
	int h = cbh(w), lh, n;
	struct ev e;
	int done = 0, chose = 0, k;

	if (c->dropped || cbtype(w) == CBS_SIMPLE)
		return;
	notify(w, 7);			/* CBN_DROPDOWN */
	n = l->n < 8 ? (l->n ? l->n : 1) : 8;
	lh = n * itemh(c->list) + 2;
	if (c->listh > 0 && c->listh < lh)
		lh = c->listh;
	wnd_setpos(c->list, (struct wnd *)0, w->wr.l, w->wr.t + h, w->wr.r - w->wr.l, lh, SWP_SHOWWINDOW | SWP_NOACTIVATE);
	lb_show(c->list, l->cur);
	c->dropped = 1;
	cb_redraw(w);
	user_flushpaint();
	k = l->cur;
	/* the list's own loop: track the pointer, pick with a click or Return */
	while (!done) {
		scr_flush();
		if (user_poll(&e, -1) != 1)
			continue;
		if (e.type == EV_MOVE || e.type == EV_BTN) {
			scr_mx = e.x;
			scr_my = e.y;
			if (e.x >= c->list->cr.l && e.x < c->list->cr.r && e.y >= c->list->cr.t && e.y < c->list->cr.b) {
				int i = lb_at(c->list, e.y - c->list->cr.t);

				if (i >= 0 && i < l->n && i != l->cur) {
					l->cur = i;
					lb_redraw(c->list);
				}
				if (e.type == EV_BTN && !e.down && e.btn == 0) {
					chose = 1;
					done = 1;
				}
			} else if (e.type == EV_BTN && e.down) {
				int ht;

				if (e.x >= c->list->wr.r - sys_metric(SM_CXVSCROLL) && e.x < c->list->wr.r &&
				    e.y >= c->list->wr.t && e.y < c->list->wr.b && (c->list->style & WS_VSCROLL)) {
					extern void sb_track();
					struct rect sr;

					r_set(&sr, c->list->cr.r - 1, c->list->cr.t - 1, c->list->cr.r + sys_metric(SM_CXVSCROLL) - 1,
					    c->list->cr.b + 1);
					sb_track(c->list, (struct wnd *)0, &sr, 1, &c->list->sb[1], e.x, e.y);
					continue;
				}
				(void)ht;
				done = 1;
			}
		} else if (e.type == EV_KEY && e.down) {
			switch (e.vk) {
			case VK_ESCAPE: l->cur = k; done = 1; break;
			case VK_RETURN: chose = 1; done = 1; break;
			case VK_UP: if (l->cur > 0) { l->cur--; lb_show(c->list, l->cur); lb_redraw(c->list); } break;
			case VK_DOWN: if (l->cur < l->n - 1) { l->cur++; lb_show(c->list, l->cur); lb_redraw(c->list); } break;
			}
		}
	}
	wnd_show(c->list, SW_HIDE);
	c->dropped = 0;
	user_flushpaint();
	notify(w, 8);			/* CBN_CLOSEUP */
	if (l->cur != k || chose) {
		cb_update(w);
		if (l->cur != k)
			notify(w, 1);	/* CBN_SELCHANGE */
	} else
		cb_redraw(w);
	if (c->edit)
		wnd_setfocus(c->edit);
}

u32
combo_proc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	struct cb *c;
	u32 cls, cs, r;
	int h, i;

	if (!w)
		return 0;
	c = cbof(w);
	switch (a[1]) {
	case WM_NCCREATE:
		/* the scroll bar the style asks for is the list's */
		if (w->style & WS_VSCROLL)
			w->user |= 0x40000;
		w->style &= ~(WS_VSCROLL | WS_HSCROLL);
		return user_defproc(w, a[1], a[2], a[3]);
	case WM_CREATE:
		h = cbh(w);
		c->listh = w->cr.b - w->cr.t - h;
		if (cbtype(w) != CBS_DROPDOWNLIST) {
			cls = ustr("Edit");
			c->edit = wnd_create(WS_EX_NOPARENTNOTIFY, cls, 0, WS_CHILD | WS_VISIBLE | WS_BORDER | 0x80 /* ES_AUTOHSCROLL */,
			    0, 0, (w->cr.r - w->cr.l) - (cbtype(w) == CBS_SIMPLE ? 0 : sys_metric(SM_CXVSCROLL) - 1), h,
			    w->h, 1001, w->hinst, 0);
			ufree(cls);
			if (c->edit)
				c->edit->user |= 0x10000;	/* tell the combo of changes */
		}
		cls = ustr("ListBox");
		cs = ualloc(12);
		PL(ulin(cs), 0x434f4d42);
		PW(ulin(cs) + 8, w->h);
		{
			u32 lbs = WS_BORDER | WS_VSCROLL | (w->style & 0x100 ? LBS_SORT : 0) |
			    (w->style & 0x10 ? LBS_OWNERDRAWFIXED : 0) | (w->style & 0x20 ? LBS_OWNERDRAWVARIABLE : 0) |
			    (w->style & 0x200 ? LBS_HASSTRINGS : 0);

			if (cbtype(w) == CBS_SIMPLE)
				c->list = wnd_create(WS_EX_NOPARENTNOTIFY, cls, 0, WS_CHILD | WS_VISIBLE | lbs, 0, h - 1,
				    w->cr.r - w->cr.l, w->cr.b - w->cr.t - h + 1, w->h, 1000, w->hinst, cs);
			else
				c->list = wnd_create(0, cls, 0, WS_POPUP | lbs, 0, 0, 10, 10, w->h, 1000, w->hinst, cs);
		}
		ufree(cs);
		ufree(cls);
		if (c->list) {
			lbof(c->list)->combo = 1;
			lbof(c->list)->owner = w;
		}
		if (cbtype(w) != CBS_SIMPLE)
			wnd_setpos(w, (struct wnd *)0, 0, 0, w->wr.r - w->wr.l, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	case WM_DESTROY:
		if (c->list && !(c->list->style & WS_CHILD))
			wnd_destroy(c->list);
		return 0;
	case WM_NCDESTROY:
		free(c);
		w->priv = 0;
		return 0;
	case WM_PAINT:
		{
			u16 hd = user_beginpaint(w, 0);

			cb_paint(w, hd);
			user_releasedc(hd);
			user_endpaint(w, 0);
		}
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_SETFONT:
		w->dlgfont = a[2];
		if (c->edit)
			wnd_send(c->edit, WM_SETFONT, a[2], a[3]);
		if (c->list)
			wnd_send(c->list, WM_SETFONT, a[2], a[3]);
		return 0;
	case WM_GETFONT:
		return w->dlgfont;
	case WM_SETFOCUS:
		if (c->edit)
			wnd_setfocus(c->edit);
		else
			cb_redraw(w);
		notify(w, 3);
		return 0;
	case WM_KILLFOCUS:
		cb_redraw(w);
		notify(w, 4);
		return 0;
	case WM_GETDLGCODE:
		return 0x0001 | 0x0080;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		if (!c->edit)
			wnd_setfocus(w);
		if ((short)LO16(a[3]) >= w->cr.r - w->cr.l - sys_metric(SM_CXVSCROLL) || !c->edit)
			cb_dropdown(w);
		return 0;
	case WM_KEYDOWN:
		if (a[2] == VK_F1 + 3 || (a[2] == VK_DOWN && (keystate[VK_MENU] & 0x80))) {
			cb_dropdown(w);
			return 0;
		}
		if (c->list && (a[2] == VK_UP || a[2] == VK_DOWN)) {
			struct lb *l = lbof(c->list);

			i = l->cur + (a[2] == VK_UP ? -1 : 1);
			if (i >= 0 && i < l->n) {
				l->cur = i;
				cb_update(w);
				notify(w, 1);
			}
		}
		return 0;
	case WM_CHAR:
		if (c->list && !c->edit && a[2] > ' ') {
			char s[2];
			struct lb *l = lbof(c->list);

			s[0] = a[2];
			s[1] = 0;
			i = lb_find(c->list, l->cur, s, 0);
			if (i >= 0) {
				l->cur = i;
				cb_update(w);
				notify(w, 1);
			}
		}
		return 0;
	case WM_COMMAND:
		/* from our edit */
		if (c->edit && LO16(a[2]) == 1001) {
			if (HI16(a[3]) == 0x300)
				notify(w, 5);	/* CBN_EDITCHANGE */
			else if (HI16(a[3]) == 0x400)
				notify(w, 6);	/* CBN_EDITUPDATE */
		}
		return 0;
	case WM_USER + 0x7f00:	/* from our list */
		if (a[2] == 1 && cbtype(w) == CBS_SIMPLE) {
			cb_update(w);
			notify(w, 1);
		} else if (a[2] == 2)
			notify(w, 2);
		return 0;
	case WM_SETTEXT:
	case WM_GETTEXT:
	case WM_GETTEXTLENGTH:
		if (c->edit)
			return wnd_send(c->edit, a[1], a[2], a[3]);
		if (a[1] == WM_SETTEXT)
			return 1;
		{
			struct lb *l = lbof(c->list);
			char *s = l->cur >= 0 && l->cur < l->n && l->it[l->cur].s ? l->it[l->cur].s : "";
			u32 p = lin(FPSEL(a[3]), FPOFF(a[3]));

			if (a[1] == WM_GETTEXTLENGTH)
				return strlen(s);
			if (!p || (short)a[2] <= 0)
				return 0;
			strncpy((char *)M + p, s, (short)a[2] - 1);
			M[p + (short)a[2] - 1] = 0;
			return strlen((char *)M + p);
		}
	/* CB_* */
	case WM_USER + 0:	/* CB_GETEDITSEL */
		return c->edit ? wnd_send(c->edit, WM_USER + 0, a[2], a[3]) : 0xffffffff;
	case WM_USER + 1:	/* CB_LIMITTEXT */
		return c->edit ? wnd_send(c->edit, WM_USER + 21, a[2], 0), 1 : 0xffffffff;
	case WM_USER + 2:	/* CB_SETEDITSEL */
		return c->edit ? wnd_send(c->edit, WM_USER + 1, 0, a[3]), 1 : 0xffffffff;
	case WM_USER + 3: return wnd_send(c->list, WM_USER + 1, a[2], a[3]);		/* ADDSTRING */
	case WM_USER + 4: return wnd_send(c->list, WM_USER + 3, a[2], a[3]);		/* DELETESTRING */
	case WM_USER + 5: return wnd_send(c->list, WM_USER + 14, a[2], a[3]);		/* DIR */
	case WM_USER + 6: return wnd_send(c->list, WM_USER + 12, a[2], a[3]);		/* GETCOUNT */
	case WM_USER + 7: return wnd_send(c->list, WM_USER + 9, a[2], a[3]);		/* GETCURSEL */
	case WM_USER + 8: return wnd_send(c->list, WM_USER + 10, a[2], a[3]);		/* GETLBTEXT */
	case WM_USER + 9: return wnd_send(c->list, WM_USER + 11, a[2], a[3]);		/* GETLBTEXTLEN */
	case WM_USER + 10: return wnd_send(c->list, WM_USER + 2, a[2], a[3]);		/* INSERTSTRING */
	case WM_USER + 11:	/* RESETCONTENT */
		wnd_send(c->list, WM_USER + 5, 0, 0);
		if (c->edit) {
			u32 s = ustr("");

			wnd_send(c->edit, WM_SETTEXT, 0, s);
			ufree(s);
		}
		cb_redraw(w);
		return 0;
	case WM_USER + 12: return wnd_send(c->list, WM_USER + 16, a[2], a[3]);	/* FINDSTRING */
	case WM_USER + 24: return wnd_send(c->list, WM_USER + 35, a[2], a[3]);	/* FINDSTRINGEXACT */
	case WM_USER + 13:	/* SELECTSTRING */
		r = wnd_send(c->list, WM_USER + 13, a[2], a[3]);
		cb_update(w);
		return r;
	case WM_USER + 14:	/* SETCURSEL */
		r = wnd_send(c->list, WM_USER + 7, a[2], a[3]);
		cb_update(w);
		if ((short)a[2] < 0 && c->edit) {
			u32 s = ustr("");

			wnd_send(c->edit, WM_SETTEXT, 0, s);
			ufree(s);
		}
		return r;
	case WM_USER + 15:	/* SHOWDROPDOWN */
		if (a[2])
			cb_dropdown(w);
		return 1;
	case WM_USER + 16: return wnd_send(c->list, WM_USER + 26, a[2], a[3]);	/* GETITEMDATA */
	case WM_USER + 17: return wnd_send(c->list, WM_USER + 27, a[2], a[3]);	/* SETITEMDATA */
	case WM_USER + 18:	/* GETDROPPEDCONTROLRECT */
		{
			u32 p = lin(FPSEL(a[3]), FPOFF(a[3]));
			struct rect rr;

			r_set(&rr, w->wr.l, w->wr.t, w->wr.r, w->wr.t + cbh(w) + c->listh);
			if (p)
				r_put(p, &rr);
		}
		return 1;
	case WM_USER + 19: return wnd_send(c->list, WM_USER + 33, a[2], a[3]);	/* SETITEMHEIGHT */
	case WM_USER + 20: return wnd_send(c->list, WM_USER + 34, a[2], a[3]);	/* GETITEMHEIGHT */
	case WM_USER + 21:
	case WM_USER + 22:
		return 0;
	case WM_USER + 23:	/* GETDROPPEDSTATE */
		return c->dropped;
	}
	return user_defproc(w, a[1], a[2], a[3]);
}

/* ---- the classes ---- */

void
controls_init()
{
	extern void menu_init(), dialog_init(), mdi_init();
	u32 p;

	p = thunk_internal(button_proc, "wwwl", 'l', "ButtonWndProc");
	cls_register("Button", CS_GLOBALCLASS | CS_DBLCLKS | CS_PARENTDC, p, 0, 0, 0, 0, cur_arrow, 0, (u32)0, 1);
	p = thunk_internal(static_proc, "wwwl", 'l', "StaticWndProc");
	cls_register("Static", CS_GLOBALCLASS | CS_PARENTDC, p, 0, 0, 0, 0, cur_arrow, 0, (u32)0, 1);
	p = thunk_internal(listbox_proc, "wwwl", 'l', "ListBoxWndProc");
	cls_register("ListBox", CS_GLOBALCLASS | CS_DBLCLKS, p, 0, 0, 0, 0, cur_arrow, 0, (u32)0, 1);
	p = thunk_internal(combo_proc, "wwwl", 'l', "ComboBoxWndProc");
	cls_register("ComboBox", CS_GLOBALCLASS | CS_DBLCLKS | CS_PARENTDC, p, 0, 0, 0, 0, cur_arrow, 0, (u32)0, 1);
	p = thunk_internal(edit_proc, "wwwl", 'l', "EditWndProc");
	cls_register("Edit", CS_GLOBALCLASS | CS_DBLCLKS, p, 0, 6, 0, 0, cur_load(0, FP(0, 32513)), 0, (u32)0, 1);
	p = thunk_internal(scroll_proc, "wwwl", 'l', "ScrollBarWndProc");
	cls_register("ScrollBar", CS_GLOBALCLASS | CS_DBLCLKS | CS_PARENTDC, p, 0, 0, 0, 0, cur_arrow, 0, (u32)0, 1);
	menu_init();
	dialog_init();
	mdi_init();
}

/* ---- the dialog-directory calls ---- */

static u32
c_DlgDirList(a)
	u32 *a;
{
	struct wnd *d = wnd_get(a[0]), *c, *st = 0;
	char *spec = gptr(a[1]), full[300];
	u32 s;

	if (!d)
		return 0;
	for (c = d->child; c; c = c->next) {
		if (c->id == LO16(a[2]))
			break;
	}
	for (st = d->child; st; st = st->next)
		if (st->id == LO16(a[3]))
			break;
	if (spec && dos_fullpath(spec, full) == 0) {
		char *sl = strrchr(full, '\\');

		/* the directory becomes current, as DlgDirList does */
		*sl = 0;
		if (full[2]) {
			extern int dos_chdir();

			dos_chdir(full);
		}
		*sl = '\\';
		if (st) {
			*sl = 0;
			s = ustr(full[2] ? full : "C:\\");
			wnd_send(st, WM_SETTEXT, 0, s);
			ufree(s);
			*sl = '\\';
		}
		if (c) {
			wnd_send(c, WM_USER + 5, 0, 0);
			s = ustr(sl + 1);
			wnd_send(c, WM_USER + 14, a[4] & ~0x8000, s);
			ufree(s);
		}
	}
	return 1;
}

static u32
c_DlgDirSelect(a)
	u32 *a;
{
	struct wnd *d = wnd_get(a[0]), *c;
	char *out = gptr(a[1]);
	struct lb *l;
	char *s;

	if (!d || !out)
		return 0;
	for (c = d->child; c; c = c->next)
		if (c->id == LO16(a[2]))
			break;
	if (!c)
		return 0;
	l = lbof(c);
	if (l->cur < 0 || l->cur >= l->n || !(s = l->it[l->cur].s))
		return 0;
	if (s[0] == '[' && s[1] == '-') {
		sprintf(out, "%c:", s[2] & 0x5f);
		return 1;
	}
	if (s[0] == '[') {
		strcpy(out, s + 1);
		out[strlen(out) - 1] = '\\';
		return 1;
	}
	strcpy(out, s);
	return 0;
}

struct impl ct_impl[] = {
	{ "USER", "DlgDirList", c_DlgDirList },
	{ "USER", "DlgDirSelect", c_DlgDirSelect },
	{ "USER", "DlgDirSelectEx", c_DlgDirSelect },
	{ 0 }
};
