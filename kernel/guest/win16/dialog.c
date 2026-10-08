/*
 * dialog.c -- dialog boxes: Win16 dialog templates, the dialog class
 * and its default procedure, the keyboard interface (IsDialogMessage:
 * tab, arrow keys, Return, Escape, mnemonics), modal dialogs, the
 * dialog-item calls, and MessageBox built from scratch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "scr.h"

extern u32 ualloc(), ulin(), ustr(), thunk_internal();
extern void ufree(), wnd_setfocus(), wnd_activate(), user_flushpaint();
extern int user_modal(), user_translate(), IsChildOf();
extern u8 keystate[];

#define	STR(p)		(gptr(p) ? gptr(p) : "")
#define	ISINT(p)	(FPSEL(p) == 0)

#define	DS_SETFONT	0x40
#define	DS_MODALFRAME	0x80
#define	DS_SYSMODAL	0x02
#define	DS_ABSALIGN	0x01
#define	DM_GETDEFID	(WM_USER + 0)
#define	DM_SETDEFID	(WM_USER + 1)
#define	DLGC_WANTARROWS	0x0001
#define	DLGC_WANTTAB	0x0002
#define	DLGC_WANTALLKEYS 0x0004
#define	DLGC_HASSETSEL	0x0008
#define	DLGC_DEFPUSHBUTTON 0x0010
#define	DLGC_UNDEFPUSHBUTTON 0x0020
#define	DLGC_RADIOBUTTON 0x0040
#define	DLGC_WANTCHARS	0x0080
#define	DLGC_STATIC	0x0100
#define	DLGC_BUTTON	0x2000
#define	BM_SETCHECK	(WM_USER + 1)
#define	BM_GETCHECK	(WM_USER + 0)
#define	BM_SETSTYLE	(WM_USER + 4)
#define	BS_PUSHBUTTON	0
#define	BS_DEFPUSHBUTTON 1
#define	EM_SETSEL	(WM_USER + 1)

#define	DWL_MSGRESULT	0
#define	DWL_DLGPROC	4
#define	DWL_USER	8
#define	DLGEXTRA	30

static u32 dlgclassproc;

/* dialog units: from the dialog's font */
static int
basex(w)
	struct wnd *w;
{
	static char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	struct gobj *o = gobj(w && w->dlgfont ? w->dlgfont : stockobj[SYSTEM_FONT], OBJ_FONT);
	int ext;

	if (!o)
		return 8;
	/* as Windows measures it: the 52 letters, (extent / 26 + 1) / 2 */
	ext = text_width(o->u.font.bf, abc, 52) + o->u.font.bold;
	return (ext / 26 + 1) / 2;
}

static int
basey(w)
	struct wnd *w;
{
	struct gobj *o = gobj(w && w->dlgfont ? w->dlgfont : stockobj[SYSTEM_FONT], OBJ_FONT);

	return o ? o->u.font.bf->f_height : 16;
}

u32
dlg_baseunits()
{
	return FP(basey((struct wnd *)0), basex((struct wnd *)0));
}

/* ---- the dialog procedure ---- */

static u32
callproc(w, msg, wp, lp)
	struct wnd *w;
	u32 msg, wp, lp;
{
	u32 p = RD32(w->extra, DWL_DLGPROC);

	if (!p)
		return 0;
	return wnd_call(p, w->h, msg, wp, lp) & 0xffff;
}

static struct wnd *
deftab(w)
	struct wnd *w;
{
	struct wnd *c;

	for (c = w->child; c; c = c->next)
		if ((c->style & WS_TABSTOP) && (c->style & WS_VISIBLE) && !(c->style & WS_DISABLED))
			return c;
	return w->child;
}

static u32
defdlg(w, msg, wp, lp)
	struct wnd *w;
	u32 msg, wp, lp;
{
	struct wnd *c;
	u16 br;

	switch (msg) {
	case WM_ERASEBKGND:
		{
			struct dc *dc = dc_get(wp);
			struct rect r;

			br = wnd_send(w, WM_CTLCOLOR, wp, FP(4, w->h));	/* CTLCOLOR_DLG */
			if (!br)
				br = sys_brush(COLOR_WINDOW);
			if (dc) {
				r = w->cr;
				d_fill(dc, &r, br, 0xf0);
			}
			return 1;
		}
	case WM_CLOSE:
		c = 0;
		{
			struct wnd *k;

			for (k = w->child; k; k = k->next)
				if (k->id == IDCANCEL)
					c = k;
		}
		if (!c || !(c->style & WS_DISABLED))
			wnd_post(w->h, WM_COMMAND, IDCANCEL, FP(0, c ? c->h : 0));
		return 0;
	case WM_SETFOCUS:
		c = wnd_get(w->lastfocus);
		if (!c || !IsChildOf(w, c))
			c = deftab(w);
		if (c)
			wnd_setfocus(c);
		return 0;
	case WM_ACTIVATE:
		if (LO16(wp) == WA_INACTIVE) {
			if (wnd_focus && IsChildOf(w, wnd_focus))
				w->lastfocus = wnd_focus->h;
			return 0;
		}
		c = wnd_get(w->lastfocus);
		if (!c || !IsChildOf(w, c))
			c = deftab(w);
		if (c)
			wnd_setfocus(c);
		else
			wnd_setfocus(w);
		return 0;
	case WM_NEXTDLGCTL:
		{
			extern struct wnd *dlg_nexttab();

			c = LO16(lp) ? wnd_get(wp) : dlg_nexttab(w, wnd_focus, wp != 0);
			if (c) {
				wnd_setfocus(c);
				if (wnd_send(c, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL)
					wnd_send(c, EM_SETSEL, 0, FP(0x7fff, 0));
			}
		}
		return 0;
	case DM_GETDEFID:
		for (c = w->child; c; c = c->next)
			if (wnd_send(c, WM_GETDLGCODE, 0, 0) & DLGC_DEFPUSHBUTTON)
				return FP(0x534b, c->id);	/* DC_HASDEFID */
		return 0;
	case DM_SETDEFID:
		for (c = w->child; c; c = c->next) {
			int code = wnd_send(c, WM_GETDLGCODE, 0, 0);

			if ((code & DLGC_DEFPUSHBUTTON) && c->id != LO16(wp))
				wnd_send(c, BM_SETSTYLE, BS_PUSHBUTTON, 1);
			else if ((code & DLGC_UNDEFPUSHBUTTON) && c->id == LO16(wp))
				wnd_send(c, BM_SETSTYLE, BS_DEFPUSHBUTTON, 1);
		}
		return 1;
	case WM_GETFONT:
		return w->dlgfont;
	case WM_CTLCOLOR:
		return user_defproc(w, msg, wp, lp);
	}
	return user_defproc(w, msg, wp, lp);
}

/* DefDlgProc: the program's procedure first */
u32
dlg_defproc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	u32 r;

	if (!w)
		return 0;
	r = callproc(w, a[1], a[2], a[3]);
	if (!wnd_get(a[0]))
		return r;
	switch (a[1]) {
	case WM_CTLCOLOR:
	case WM_COMPAREITEM:
	case WM_VKEYTOITEM:
	case WM_CHARTOITEM:
	case WM_QUERYDRAGICON:
	case WM_INITDIALOG:
		if (r)
			return r;
		break;
	default:
		if (r)
			return RD32(w->extra, DWL_MSGRESULT);
	}
	return defdlg(w, a[1], a[2], a[3]);
}

/* the DefDlgProc export: the default part only, as a program's dialog class calls it */
static u32
d_DefDlgProc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);

	return w ? defdlg(w, a[1], a[2], a[3]) : 0;
}

/* ---- creating from a template ---- */

static char *
tstr(pp)
	u32 *pp;
{
	char *s = (char *)M + *pp;

	*pp += strlen(s) + 1;
	return s;
}

static u32
tname(pp)
	u32 *pp;
{
	u32 p = *pp;

	if (M[p] == 0xff) {
		*pp += 3;
		return FP(0, GW(p + 1));
	}
	*pp += strlen((char *)M + p) + 1;
	return 0xffffffff;	/* a string at p: the caller has it */
}

static char *ctlclass[] = { "Button", "Edit", "Static", "ListBox", "ScrollBar", "ComboBox" };

u16
dlg_create(hinst, t, ownerh, proc, param, modal)
	u32 hinst, t, ownerh, proc, param;
	int modal;
{
	u32 p = t, style, menu = 0, cls = 0, title, cn, tx, f, lpcls, lptitle;
	int n, x, y, cx, cy, i, pt = 0, bx, by;
	char face[32];
	struct wnd *w, *c, *owner = wnd_get(ownerh);
	u16 font = 0;
	u32 menuspec, clsspec;

	style = GL(p);
	n = M[p + 4];
	x = (short)GW(p + 5);
	y = (short)GW(p + 7);
	cx = (short)GW(p + 9);
	cy = (short)GW(p + 11);
	p += 13;
	menuspec = p;
	menu = tname(&p);
	clsspec = p;
	cls = tname(&p);
	title = p;
	tstr(&p);
	face[0] = 0;
	if (style & DS_SETFONT) {
		pt = GW(p);
		p += 2;
		strncpy(face, tstr(&p), 31);
		face[31] = 0;
	}
	if (pt) {
		struct impl *im;
		u32 a[14];

		memset(a, 0, sizeof a);
		a[0] = (u32)(-(pt * 96 / 72)) & 0xffff;
		a[0] = (u32)(s32)(short)a[0];
		a[4] = 700;	/* 3.1 dialogs are bold */
		a[13] = ustr(face);
		for (im = g_impl; im->im_mod; im++)
			if (strcmp(im->im_name, "CreateFont") == 0)
				font = (*im->im_fn)(a);
		ufree(a[13]);
	}
	/* the dialog window: units to pixels by its font */
	{
		struct wnd tmp;

		memset(&tmp, 0, sizeof tmp);
		tmp.dlgfont = font;
		bx = basex(&tmp);
		by = basey(&tmp);
	}
	x = x * bx / 4;
	y = y * by / 8;
	cx = cx * bx / 4;
	cy = cy * by / 8;
	/* the size given is the client area's */
	{
		struct wnd tw;
		struct rect r, cr;

		memset(&tw, 0, sizeof tw);
		tw.style = style;
		tw.exstyle = (style & DS_MODALFRAME) ? WS_EX_DLGMODALFRAME : 0;
		r_set(&r, 0, 0, 1000, 1000);
		wnd_calcclient(&tw, &r, &cr);
		cx += 1000 - (cr.r - cr.l);
		cy += 1000 - (cr.b - cr.t);
		if (!(style & WS_CHILD) && !(style & DS_ABSALIGN) && owner) {
			x += owner->cr.l;
			y += owner->cr.t;
		}
		if (!(style & WS_CHILD)) {
			x += cr.l - 0 - 0;
			x -= cr.l;
			/* keep it on the screen */
			if (x + cx > screen.w) x = screen.w - cx;
			if (y + cy > screen.h) y = screen.h - cy;
			if (x < 0) x = 0;
			if (y < 0) y = 0;
		}
	}
	lptitle = ualloc(strlen((char *)M + title) + 1);
	strcpy((char *)M + ulin(lptitle), (char *)M + title);
	if (cls == 0xffffffff && M[clsspec]) {
		lpcls = ualloc(strlen((char *)M + clsspec) + 1);
		strcpy((char *)M + ulin(lpcls), (char *)M + clsspec);
	} else
		lpcls = ustr("#32770");
	w = wnd_create((style & DS_MODALFRAME) ? WS_EX_DLGMODALFRAME : 0, lpcls, lptitle,
	    style & ~WS_VISIBLE, x, y, cx, cy, ownerh, 0, hinst, 0);
	ufree(lpcls);
	ufree(lptitle);
	if (!w)
		return 0;
	if (menu != 0xffffffff || M[menuspec])
		w->id = menu_load(hinst, menu != 0xffffffff ? menu : FP(0, 0));
	w->flags |= WF_DIALOG;
	w->dlgfont = font;
	if (w->cls->wndextra >= 8)
		WR32(w->extra, DWL_DLGPROC, proc);
	w->dlgproc = proc;
	/* the controls */
	for (i = 0; i < n; i++) {
		int ix, iy, icx, icy, id;
		u32 ist, ctlname, txt, extra = 0;
		char *cname;
		struct wnd *k;

		ix = (short)GW(p) * bx / 4;
		iy = (short)GW(p + 2) * by / 8;
		icx = (short)GW(p + 4) * bx / 4;
		icy = (short)GW(p + 6) * by / 8;
		id = GW(p + 8);
		ist = GL(p + 10);
		p += 14;
		if (M[p] & 0x80) {
			cname = (M[p] - 0x80) < 6 ? ctlclass[M[p] - 0x80] : "Static";
			p++;
		} else
			cname = tstr(&p);
		cn = ustr(cname);
		if (M[p] == 0xff) {
			txt = FP(0, GW(p + 1));
			tx = 0;
			p += 3;
		} else {
			tx = ualloc(strlen((char *)M + p) + 1);
			strcpy((char *)M + ulin(tx), (char *)M + p);
			txt = tx;
			tstr(&p);
		}
		f = M[p++];
		if (f) {
			extra = ualloc(f);
			memcpy(M + ulin(extra), M + p, f);
			p += f;
		}
		k = wnd_create(WS_EX_NOPARENTNOTIFY, cn, txt, ist | WS_CHILD, ix, iy, icx, icy, w->h, id, hinst,
		    extra);
		if (extra)
			ufree(extra);
		if (tx)
			ufree(tx);
		ufree(cn);
		if (!wnd_get(w->h))
			return 0;
		if (k && font)
			wnd_send(k, WM_SETFONT, font, 0);
		(void)ctlname;
	}
	/* WM_INITDIALOG: focus the first tab stop unless the procedure did */
	c = deftab(w);
	if (wnd_send(w, WM_INITDIALOG, c ? c->h : 0, param) && wnd_get(w->h)) {
		c = deftab(w);
		if (c && (wnd_send(c, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL))
			wnd_send(c, EM_SETSEL, 0, FP(0x7fff, 0));
		w->lastfocus = c ? c->h : 0;
	}
	if (!wnd_get(w->h))
		return 0;
	if (style & WS_VISIBLE || modal) {
		wnd_show(w, SW_SHOW);
		if (wnd_get(w->lastfocus))
			wnd_setfocus(wnd_get(w->lastfocus));
	}
	return w->h;
}

/* the modal loop of a dialog: its result */
int
dlg_run(h)
	u32 h;
{
	struct wnd *w = wnd_get(h), *owner;
	int done = 0, r, en;

	if (!w)
		return -1;
	owner = w->owner;
	en = owner && !(owner->style & WS_DISABLED);
	if (en) {
		owner->style |= WS_DISABLED;
	}
	w->priv = (void *)&done;
	user_modal(w, &w->flags, WF_DLGEND);
	r = wnd_get(h) ? w->dlgresult : -1;
	if (en && wnd_get(owner->h))
		owner->style &= ~WS_DISABLED;
	if (wnd_get(h))
		wnd_destroy(w);
	if (owner && wnd_get(owner->h))
		wnd_activate(owner, WA_ACTIVE);
	return r;
}

/* ---- the keyboard ---- */

struct wnd *
dlg_nexttab(dlg, from, prev)
	struct wnd *dlg, *from;
	int prev;
{
	struct wnd *list[256], *c;
	int n = 0, i, k;

	for (c = dlg->child; c && n < 256; c = c->next)
		list[n++] = c;
	if (!n)
		return 0;
	for (k = 0; k < n && list[k] != from; k++)
		;
	if (k == n)
		k = prev ? 0 : n - 1;
	for (i = 1; i <= n; i++) {
		c = list[(k + (prev ? n - i : i)) % n];
		if ((c->style & WS_TABSTOP) && (c->style & WS_VISIBLE) && !(c->style & WS_DISABLED))
			return c;
	}
	return from;
}

static struct wnd *
nextgroup(dlg, from, prev)
	struct wnd *dlg, *from;
	int prev;
{
	struct wnd *list[256], *c;
	int n = 0, k, start, end, i;

	for (c = dlg->child; c && n < 256; c = c->next)
		list[n++] = c;
	for (k = 0; k < n && list[k] != from; k++)
		;
	if (k == n)
		return from;
	for (start = k; start > 0 && !(list[start]->style & WS_GROUP); start--)
		;
	for (end = k + 1; end < n && !(list[end]->style & WS_GROUP); end++)
		;
	for (i = 1; i < end - start; i++) {
		c = list[start + (k - start + (prev ? end - start - i : i)) % (end - start)];
		if ((c->style & WS_VISIBLE) && !(c->style & WS_DISABLED))
			return c;
	}
	return from;
}

static struct wnd *
bymnemonic(dlg, ch)
	struct wnd *dlg;
	int ch;
{
	struct wnd *c, *n;
	char *p;
	int u = ch >= 'a' && ch <= 'z' ? ch - 32 : ch;

	for (c = dlg->child; c; c = c->next) {
		int code = wnd_send(c, WM_GETDLGCODE, 0, 0);

		if (!(code & (DLGC_BUTTON | DLGC_STATIC | DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON |
		    DLGC_RADIOBUTTON)) && !(code & DLGC_STATIC))
			continue;
		if (!c->text || (p = strchr(c->text, '&')) == 0)
			continue;
		if ((p[1] >= 'a' && p[1] <= 'z' ? p[1] - 32 : p[1]) != u)
			continue;
		if (!(c->style & WS_VISIBLE) || (c->style & WS_DISABLED))
			continue;
		if (code & DLGC_STATIC) {
			/* a label: the control after it */
			for (n = c->next; n && (!(n->style & WS_VISIBLE) || (n->style & WS_DISABLED)); n = n->next)
				;
			return n;
		}
		return c;
	}
	return 0;
}

static void
clickbutton(c)
	struct wnd *c;
{
	wnd_setfocus(c);
	wnd_send(c, WM_LBUTTONDOWN, 0, 0);
	wnd_send(c, WM_LBUTTONUP, 0, 0);
}

int
dlg_ismsg(dlg, a)
	struct wnd *dlg;
	u32 a;
{
	struct wnd *to = wnd_get(GW(a + MSG_HWND)), *c;
	u32 msg = GW(a + MSG_MESSAGE), wp = GW(a + MSG_WPARAM);
	int code;

	if (!dlg || !to || (to != dlg && !IsChildOf(dlg, to)))
		return 0;
	code = wnd_send(to, WM_GETDLGCODE, 0, 0);
	if (msg == WM_KEYDOWN) {
		switch (wp) {
		case VK_TAB:
			if (code & (DLGC_WANTTAB | DLGC_WANTALLKEYS))
				break;
			c = dlg_nexttab(dlg, wnd_focus, (keystate[VK_SHIFT] & 0x80) != 0);
			if (c) {
				wnd_setfocus(c);
				if (wnd_send(c, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL)
					wnd_send(c, EM_SETSEL, 0, FP(0x7fff, 0));
			}
			return 1;
		case VK_LEFT:
		case VK_UP:
		case VK_RIGHT:
		case VK_DOWN:
			if (code & (DLGC_WANTARROWS | DLGC_WANTALLKEYS))
				break;
			c = nextgroup(dlg, wnd_focus, wp == VK_LEFT || wp == VK_UP);
			if (c && c != wnd_focus) {
				wnd_setfocus(c);
				if (wnd_send(c, WM_GETDLGCODE, 0, 0) & DLGC_RADIOBUTTON)
					clickbutton(c);
			}
			return 1;
		case VK_RETURN:
			if (code & DLGC_WANTALLKEYS)
				break;
			{
				u32 def = wnd_send(dlg, DM_GETDEFID, 0, 0);
				int id = HI16(def) == 0x534b ? LO16(def) : IDOK;

				if ((code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) && wnd_focus)
					id = wnd_focus->id;
				for (c = dlg->child; c; c = c->next)
					if (c->id == id)
						break;
				if (c && (c->style & WS_DISABLED)) {
					scr_beep();
					return 1;
				}
				wnd_send(dlg, WM_COMMAND, id, FP(0, c ? c->h : 0));
			}
			return 1;
		case VK_ESCAPE:
			if (code & DLGC_WANTALLKEYS)
				break;
			for (c = dlg->child; c; c = c->next)
				if (c->id == IDCANCEL)
					break;
			wnd_send(dlg, WM_COMMAND, IDCANCEL, FP(0, c ? c->h : 0));
			return 1;
		}
	}
	if (msg == WM_CHAR && !(code & (DLGC_WANTCHARS | DLGC_WANTALLKEYS)) && wp > ' ') {
		if ((c = bymnemonic(dlg, wp)) != 0) {
			int cc = wnd_send(c, WM_GETDLGCODE, 0, 0);

			if (cc & (DLGC_BUTTON | DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON | DLGC_RADIOBUTTON))
				clickbutton(c);
			else
				wnd_setfocus(c);
			return 1;
		}
	}
	if (msg == WM_SYSCHAR) {
		if ((c = bymnemonic(dlg, wp)) != 0) {
			int cc = wnd_send(c, WM_GETDLGCODE, 0, 0);

			if (cc & (DLGC_BUTTON | DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON | DLGC_RADIOBUTTON))
				clickbutton(c);
			else
				wnd_setfocus(c);
			return 1;
		}
	}
	user_translate(a);
	user_dispatch(a);
	return 1;
}

/* ---- the calls ---- */

static u32
createparam(a, modal, indirect)
	u32 *a;
	int modal, indirect;
{
	u32 t;
	struct module *m = mod_byhandle(a[0]);

	if (indirect)
		t = lin(a[1], 0) ? lin(a[1], 0) : lin(FPSEL(a[1]), FPOFF(a[1]));
	else
		t = res_data(m, FP(0, RT_DIALOG), a[1], (u32 *)0);
	if (!t)
		return 0;
	return dlg_create(a[0], t, a[2], a[3], a[4], modal);
}

static u32 d_CreateDialog(a) u32 *a; { u32 b[5]; memcpy(b, a, 16); b[4] = 0; return createparam(b, 0, 0); }
static u32 d_CreateDialogParam(a) u32 *a; { return createparam(a, 0, 0); }

static u32
d_CreateDialogIndirect(a)
	u32 *a;
{
	u32 b[5];

	memcpy(b, a, 16);
	b[4] = 0;
	b[1] = lin(FPSEL(a[1]), FPOFF(a[1]));
	if (!b[1])
		return 0;
	return dlg_create(b[0], b[1], b[2], b[3], 0, 0);
}

static u32
d_CreateDialogIndirectParam(a)
	u32 *a;
{
	u32 t = lin(FPSEL(a[1]), FPOFF(a[1]));

	return t ? dlg_create(a[0], t, a[2], a[3], a[4], 0) : 0;
}

static u32
modal(h)
	u32 h;
{
	if (!h)
		return 0xffff;
	return dlg_run(h) & 0xffff;
}

static u32 d_DialogBox(a) u32 *a; { u32 b[5]; memcpy(b, a, 16); b[4] = 0; return modal(createparam(b, 1, 0)); }
static u32 d_DialogBoxParam(a) u32 *a; { return modal(createparam(a, 1, 0)); }

static u32
d_DialogBoxIndirect(a)
	u32 *a;
{
	u32 t = lin(a[1], 0);

	return t ? modal(dlg_create(a[0], t, a[2], a[3], 0, 1)) : 0xffff;
}

static u32
d_DialogBoxIndirectParam(a)
	u32 *a;
{
	u32 t = lin(a[1], 0);

	return t ? modal(dlg_create(a[0], t, a[2], a[3], a[4], 1)) : 0xffff;
}

static u32
d_EndDialog(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);

	if (!w)
		return 0;
	w->dlgresult = (short)a[1];
	w->flags |= WF_DLGEND;
	if (!(w->flags & WF_DIALOG) || !w->priv)
		wnd_show(w, SW_HIDE);
	return 1;
}

static u32
d_IsDialogMessage(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));

	return p ? dlg_ismsg(wnd_get(a[0]), p) : 0;
}

static struct wnd *
item(h, id)
	u32 h, id;
{
	struct wnd *w = wnd_get(h), *c;

	if (!w)
		return 0;
	for (c = w->child; c; c = c->next)
		if (c->id == (id & 0xffff))
			return c;
	return 0;
}

static u32 d_GetDlgItem(a) u32 *a; { struct wnd *c = item(a[0], a[1]); return c ? c->h : 0; }
static u32 d_GetDlgCtrlID(a) u32 *a; { struct wnd *w = wnd_get(a[0]); return w ? w->id : 0; }

static u32
d_SetDlgItemText(a)
	u32 *a;
{
	struct wnd *c = item(a[0], a[1]);

	if (c)
		wnd_send(c, WM_SETTEXT, 0, a[2]);
	return 0;
}

static u32
d_GetDlgItemText(a)
	u32 *a;
{
	struct wnd *c = item(a[0], a[1]);

	return c ? wnd_send(c, WM_GETTEXT, a[3], a[2]) : 0;
}

static u32
d_SetDlgItemInt(a)
	u32 *a;
{
	struct wnd *c = item(a[0], a[1]);
	u32 s;
	char buf[16];

	if (!c)
		return 0;
	if (a[3])
		sprintf(buf, "%d", (short)a[2]);
	else
		sprintf(buf, "%u", a[2] & 0xffff);
	s = ustr(buf);
	wnd_send(c, WM_SETTEXT, 0, s);
	ufree(s);
	return 0;
}

static u32
d_GetDlgItemInt(a)
	u32 *a;
{
	struct wnd *c = item(a[0], a[1]);
	u32 ok = lin(FPSEL(a[2]), FPOFF(a[2])), s;
	char *t, *e;
	long v;

	if (ok)
		PW(ok, 0);
	if (!c)
		return 0;
	s = ualloc(64);
	wnd_send(c, WM_GETTEXT, 64, s);
	t = (char *)M + ulin(s);
	while (*t == ' ')
		t++;
	v = strtol(t, &e, 10);
	ufree(s);
	if (e == t || (!a[3] && v < 0) || v > (a[3] ? 32767 : 65535) || v < (a[3] ? -32768 : 0))
		return 0;
	if (ok)
		PW(ok, 1);
	return v & 0xffff;
}

static u32
d_CheckDlgButton(a)
	u32 *a;
{
	struct wnd *c = item(a[0], a[1]);

	if (c)
		wnd_send(c, BM_SETCHECK, a[2], 0);
	return 0;
}

static u32
d_IsDlgButtonChecked(a)
	u32 *a;
{
	struct wnd *c = item(a[0], a[1]);

	return c ? wnd_send(c, BM_GETCHECK, 0, 0) : 0;
}

static u32
d_CheckRadioButton(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]), *c;

	if (!w)
		return 0;
	for (c = w->child; c; c = c->next)
		if (c->id >= LO16(a[1]) && c->id <= LO16(a[2]))
			wnd_send(c, BM_SETCHECK, c->id == LO16(a[3]), 0);
	return 0;
}

static u32
d_SendDlgItemMessage(a)
	u32 *a;
{
	struct wnd *c = item(a[0], a[1]);

	return c ? wnd_send(c, a[2], a[3], a[4]) : 0;
}

static u32
d_MapDialogRect(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	int bx = basex(w), by = basey(w);

	if (!p)
		return 0;
	PW(p, (short)GW(p) * bx / 4);
	PW(p + 2, (short)GW(p + 2) * by / 8);
	PW(p + 4, (short)GW(p + 4) * bx / 4);
	PW(p + 6, (short)GW(p + 6) * by / 8);
	return 0;
}

static u32
d_GetNextDlgTabItem(a)
	u32 *a;
{
	struct wnd *c = dlg_nexttab(wnd_get(a[0]), wnd_get(a[1]), a[2]);

	return c ? c->h : 0;
}

static u32
d_GetNextDlgGroupItem(a)
	u32 *a;
{
	struct wnd *d = wnd_get(a[0]), *c;

	if (!d)
		return 0;
	c = nextgroup(d, wnd_get(a[1]), a[2]);
	return c ? c->h : 0;
}

/* ---- MessageBox ---- */

static int mbresult;

static u32
mb_proc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);

	if (!w)
		return 0;
	if (a[1] == WM_COMMAND && LO16(a[2]) >= IDOK && LO16(a[2]) <= IDNO) {
		w->dlgresult = LO16(a[2]);
		w->flags |= WF_DLGEND;
		return 1;
	}
	return 0;
}

int
user_messagebox(hwnd, text, caption, style)
	u32 hwnd, style;
	char *text, *caption;
{
	static char *labels[] = { "", "OK", "Cancel", "&Abort", "&Retry", "&Ignore", "&Yes", "&No" };
	static int sets[6][4] = {
		{ IDOK, 0 }, { IDOK, IDCANCEL, 0 }, { IDABORT, IDRETRY, IDIGNORE, 0 },
		{ IDYES, IDNO, IDCANCEL, 0 }, { IDYES, IDNO, 0 }, { IDRETRY, IDCANCEL, 0 }
	};
	struct bfont *f = gobj(stockobj[SYSTEM_FONT], OBJ_FONT)->u.font.bf;
	struct wnd *w, *owner = wnd_get(hwnd);
	struct rect r;
	struct dc *dc;
	int *set = sets[(style & 7) < 6 ? style & 7 : 0], nb, i, tw, th, bw = 74, bh = 26, w0, h0;
	int icon = (style >> 4) & 7, ix, def = (style >> 8) & 3, res;
	u32 cls, t, p;
	u16 hdc, hicon = 0;
	extern u16 user_dc();

	if (!desktop)
		return IDOK;
	for (nb = 0; set[nb]; nb++)
		;
	/* the text's size, wrapped to at most two thirds of the screen */
	{
		u16 h = dc_new(DCK_MEMORY);
		struct impl *im;
		u32 b[1];

		(void)im;
		(void)b;
		dc = dc_get(h);
		dc->st.font = stockobj[SYSTEM_FONT];
		r_set(&r, 0, 0, screen.w * 2 / 3, 0);
		draw_text(dc, text, strlen(text), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
		dc_free(h);
	}
	tw = r.r - r.l;
	th = r.b - r.t;
	ix = icon ? 48 : 16;
	w0 = ix + tw + 24;
	if (w0 < nb * (bw + 12) + 24)
		w0 = nb * (bw + 12) + 24;
	h0 = (th > 32 ? th : 32) + bh + 48;
	cls = ustr("#32770");
	t = ustr(caption);
	w = wnd_create(WS_EX_DLGMODALFRAME, cls, t, WS_POPUP | WS_CAPTION | WS_SYSMENU,
	    (screen.w - w0) / 2, (screen.h - h0 - 20) / 2, w0 + 8, h0 + 28, hwnd, 0, 0, 0);
	ufree(t);
	ufree(cls);
	if (!w)
		return IDOK;
	w->flags |= WF_DIALOG;
	p = thunk_internal(mb_proc, "wwwl", 'w', "MessageBoxProc");
	WR32(w->extra, DWL_DLGPROC, p);
	if (icon >= 1 && icon <= 4)
		hicon = icon_load(0, FP(0, 32512 + icon));
	/* the text, as a static control */
	cls = ustr("Static");
	t = ustr(text);
	wnd_create(0, cls, t, WS_CHILD | WS_VISIBLE | 0x80 /* SS_NOPREFIX */, ix, 16, tw + 8, th, w->h, 0xffff, 0, 0);
	ufree(t);
	ufree(cls);
	/* the buttons, centred */
	cls = ustr("Button");
	for (i = 0; i < nb; i++) {
		struct wnd *b;
		int x = (w->cr.r - w->cr.l - nb * (bw + 12) + 12) / 2 + i * (bw + 12);

		t = ustr(labels[set[i]]);
		b = wnd_create(0, cls, t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == def ? BS_DEFPUSHBUTTON : 0),
		    x, h0 - bh - 14, bw, bh, w->h, set[i], 0, 0);
		ufree(t);
		if (i == def && b)
			w->lastfocus = b->h;
	}
	ufree(cls);
	w->user = hicon;
	{
		int en = owner && !(owner->style & WS_DISABLED);

		if (en)
			owner->style |= WS_DISABLED;
		wnd_show(w, SW_SHOW);
		if (wnd_get(w->lastfocus))
			wnd_setfocus(wnd_get(w->lastfocus));
		/* the icon, painted with the dialog */
		user_flushpaint();
		if (hicon) {
			hdc = user_dc(w, DCK_WINDOW, (struct rgn *)0);
			icon_draw(dc_get(hdc), hicon, w->cr.l + 12, w->cr.t + 14);
			user_releasedc(hdc);
		}
		res = dlg_run(w->h);
		if (en && wnd_get(owner->h))
			owner->style &= ~WS_DISABLED;
	}
	(void)mbresult;
	if (res <= 0)
		res = set[nb - 1] == IDCANCEL ? IDCANCEL : set[0];
	return res;
}

void
dialog_init()
{
	dlgclassproc = thunk_internal(dlg_defproc, "wwwl", 'l', "DefDlgProc");
	cls_register("#32770", CS_GLOBALCLASS | CS_SAVEBITS, dlgclassproc, 0, DLGEXTRA, 0, 0, cur_arrow, 0,
	    (u32)0, 1);
}

struct impl dl_impl[] = {
	{ "USER", "CreateDialog", d_CreateDialog },
	{ "USER", "CreateDialogParam", d_CreateDialogParam },
	{ "USER", "CreateDialogIndirect", d_CreateDialogIndirect },
	{ "USER", "CreateDialogIndirectParam", d_CreateDialogIndirectParam },
	{ "USER", "DialogBox", d_DialogBox },
	{ "USER", "DialogBoxParam", d_DialogBoxParam },
	{ "USER", "DialogBoxIndirect", d_DialogBoxIndirect },
	{ "USER", "DialogBoxIndirectParam", d_DialogBoxIndirectParam },
	{ "USER", "EndDialog", d_EndDialog },
	{ "USER", "IsDialogMessage", d_IsDialogMessage },
	{ "USER", "DefDlgProc", d_DefDlgProc },
	{ "USER", "GetDlgItem", d_GetDlgItem },
	{ "USER", "GetDlgCtrlID", d_GetDlgCtrlID },
	{ "USER", "SetDlgItemText", d_SetDlgItemText },
	{ "USER", "GetDlgItemText", d_GetDlgItemText },
	{ "USER", "SetDlgItemInt", d_SetDlgItemInt },
	{ "USER", "GetDlgItemInt", d_GetDlgItemInt },
	{ "USER", "CheckDlgButton", d_CheckDlgButton },
	{ "USER", "IsDlgButtonChecked", d_IsDlgButtonChecked },
	{ "USER", "CheckRadioButton", d_CheckRadioButton },
	{ "USER", "SendDlgItemMessage", d_SendDlgItemMessage },
	{ "USER", "MapDialogRect", d_MapDialogRect },
	{ "USER", "GetNextDlgTabItem", d_GetNextDlgTabItem },
	{ "USER", "GetNextDlgGroupItem", d_GetNextDlgGroupItem },
	{ 0 }
};
