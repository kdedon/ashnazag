/*
 * uapi.c -- the USER exports: thin handlers over user.c, defwnd.c,
 * menu.c, dialog.c and controls.c, plus rectangles, strings
 * (wsprintf, Ansi*), resources (strings, bitmaps, accelerators), the
 * clipboard and the keyboard.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "scr.h"

extern u32 ualloc(), ulin(), ustr(), atom_add();
extern char *atom_name();
extern void ufree(), wnd_setfocus(), wnd_activate(), user_postquit(), user_endpaint();
extern void user_destroycaret(), user_setcaretpos(), user_showcaret(), expose(), user_ncpaint();
extern int user_settimer(), user_killtimer(), user_setsystimer(), user_createcaret(), user_translate();
extern int user_caretwnd(), IsChildOf(), user_modal(), user_keystate(), ico_valid();
extern u32 user_getcaretpos();
extern u16 user_beginpaint(), bitmap_handle(), cur_get(), ico_create();
extern void ico_destroy();
extern u8 keystate[];
extern struct bitmap *dib_bitmap();

#define	STR(p)		(gptr(p) ? gptr(p) : "")
#define	ISINT(p)	(FPSEL(p) == 0)
#define	W(h)		struct wnd *w = wnd_get(h); if (!w) return 0
#define	LIN(p)		lin(FPSEL(p), FPOFF(p))

/* ---- windows ---- */

static u32
u_RegisterClass(a)
	u32 *a;
{
	u32 p = LIN(a[0]);
	struct cls *c;
	u32 name, menu;
	int global;

	if (!p)
		return 0;
	name = GL(p + 22);
	menu = GL(p + 18);
	global = (GW(p) & CS_GLOBALCLASS) != 0;
	if ((c = cls_find(name, GW(p + 10))) != 0 && c->hinst == GW(p + 10))
		return 0;
	c = cls_register(ISINT(name) ? atom_name(FPOFF(name)) ? atom_name(FPOFF(name)) : "" : STR(name),
	    GW(p), GL(p + 2), (short)GW(p + 6), (short)GW(p + 8), GW(p + 10), GW(p + 12), GW(p + 14),
	    GW(p + 16), menu, global);
	return c->atom;
}

static u32
u_UnregisterClass(a)
	u32 *a;
{
	return 1;
}

static u32
u_GetClassInfo(a)
	u32 *a;
{
	struct cls *c = cls_find(a[1], a[0]);
	u32 p = LIN(a[2]);

	if (!c || !p)
		return 0;
	PW(p, c->style);
	PL(p + 2, c->proc);
	PW(p + 6, c->clsextra);
	PW(p + 8, c->wndextra);
	PW(p + 10, c->hinst);
	PW(p + 12, c->icon);
	PW(p + 14, c->cursor);
	PW(p + 16, c->bg);
	PL(p + 18, c->menuname);
	PL(p + 22, a[1]);
	return 1;
}

static u32
u_CreateWindowEx(a)
	u32 *a;
{
	struct wnd *w = wnd_create(a[0], a[1], a[2], a[3], (short)a[4], (short)a[5], (short)a[6], (short)a[7],
	    a[8], a[9], a[10], a[11]);

	return w ? w->h : 0;
}

static u32
u_CreateWindow(a)
	u32 *a;
{
	u32 b[12];

	b[0] = 0;
	memcpy(b + 1, a, 11 * sizeof *a);
	return u_CreateWindowEx(b);
}

static u32 u_DestroyWindow(a) u32 *a; { W(a[0]); wnd_destroy(w); return 1; }

static u32
u_ShowWindow(a)
	u32 *a;
{
	int was;
	W(a[0]);

	was = (w->style & WS_VISIBLE) != 0;
	wnd_show(w, a[1]);
	return was;
}

static u32 u_UpdateWindow(a) u32 *a; { W(a[0]); wnd_update(w); return 0; }
static u32 u_IsWindow(a) u32 *a; { return wnd_get(a[0]) != 0; }
static u32 u_IsWindowVisible(a) u32 *a; { W(a[0]); return wnd_visible(w); }
static u32 u_IsWindowEnabled(a) u32 *a; { W(a[0]); return !(w->style & WS_DISABLED); }
static u32 u_IsIconic(a) u32 *a; { W(a[0]); return (w->style & WS_MINIMIZE) != 0; }
static u32 u_IsZoomed(a) u32 *a; { W(a[0]); return (w->style & WS_MAXIMIZE) != 0; }

static u32
u_IsChild(a)
	u32 *a;
{
	struct wnd *p = wnd_get(a[0]), *c = wnd_get(a[1]);

	return p && c && IsChildOf(p, c);
}

static u32
u_EnableWindow(a)
	u32 *a;
{
	int was;
	W(a[0]);

	was = (w->style & WS_DISABLED) != 0;
	if (a[1] && was) {
		w->style &= ~WS_DISABLED;
		wnd_send(w, WM_ENABLE, 1, 0);
	} else if (!a[1] && !was) {
		w->style |= WS_DISABLED;
		if (wnd_focus == w || IsChildOf(w, wnd_focus))
			wnd_setfocus((struct wnd *)0);
		if (wnd_capture == w)
			wnd_capture = 0;
		wnd_send(w, WM_ENABLE, 0, 0);
	}
	return was;
}

static u32
u_GetWindowRect(a)
	u32 *a;
{
	u32 p = LIN(a[1]);
	W(a[0]);

	if (p)
		r_put(p, &w->wr);
	return 0;
}

static u32
u_GetClientRect(a)
	u32 *a;
{
	u32 p = LIN(a[1]);
	struct rect r;
	W(a[0]);

	r_set(&r, 0, 0, w->cr.r - w->cr.l, w->cr.b - w->cr.t);
	if (p)
		r_put(p, &r);
	return 0;
}

static u32
u_ClientToScreen(a)
	u32 *a;
{
	u32 p = LIN(a[1]);
	W(a[0]);

	if (p) {
		PW(p, (short)GW(p) + w->cr.l);
		PW(p + 2, (short)GW(p + 2) + w->cr.t);
	}
	return 0;
}

static u32
u_ScreenToClient(a)
	u32 *a;
{
	u32 p = LIN(a[1]);
	W(a[0]);

	if (p) {
		PW(p, (short)GW(p) - w->cr.l);
		PW(p + 2, (short)GW(p + 2) - w->cr.t);
	}
	return 0;
}

static u32
u_MapWindowPoints(a)
	u32 *a;
{
	struct wnd *f = wnd_get(a[0]), *t = wnd_get(a[1]);
	u32 p = LIN(a[2]);
	int dx, dy, i;

	dx = (f ? f->cr.l : 0) - (t ? t->cr.l : 0);
	dy = (f ? f->cr.t : 0) - (t ? t->cr.t : 0);
	for (i = 0; p && i < (int)a[3]; i++, p += 4) {
		PW(p, (short)GW(p) + dx);
		PW(p + 2, (short)GW(p + 2) + dy);
	}
	return FP(dy, dx);
}

static u32
u_WindowFromPoint(a)
	u32 *a;
{
	int ht;
	struct wnd *w = wnd_frompoint((short)LO16(a[0]), (short)HI16(a[0]), &ht);

	return w == desktop ? desktop->h : w->h;
}

static u32
u_ChildWindowFromPoint(a)
	u32 *a;
{
	struct wnd *c;
	int x, y;
	W(a[0]);

	x = (short)LO16(a[1]) + w->cr.l;
	y = (short)HI16(a[1]) + w->cr.t;
	if (x < w->cr.l || x >= w->cr.r || y < w->cr.t || y >= w->cr.b)
		return 0;
	for (c = w->child; c; c = c->next)
		if (x >= c->wr.l && x < c->wr.r && y >= c->wr.t && y < c->wr.b)
			return c->h;
	return w->h;
}

static u32
u_MoveWindow(a)
	u32 *a;
{
	W(a[0]);
	wnd_setpos(w, (struct wnd *)0, (short)a[1], (short)a[2], (short)a[3], (short)a[4],
	    SWP_NOZORDER | SWP_NOACTIVATE | (a[5] ? 0 : SWP_NOREDRAW));
	return 1;
}

static u32
u_SetWindowPos(a)
	u32 *a;
{
	struct wnd *after = 0;
	W(a[0]);

	if (LO16(a[1]) == 1)
		after = (struct wnd *)1;
	else if (LO16(a[1]) > 1)
		after = wnd_get(a[1]);
	wnd_setpos(w, after, (short)a[2], (short)a[3], (short)a[4], (short)a[5], a[6]);
	if ((a[6] & SWP_SHOWWINDOW) && !(w->style & WS_CHILD) && !(a[6] & SWP_NOACTIVATE))
		wnd_activate(w, WA_ACTIVE);
	return 1;
}

static u32
u_BringWindowToTop(a)
	u32 *a;
{
	W(a[0]);
	if (!(w->style & WS_CHILD))
		wnd_activate(w, WA_ACTIVE);
	else
		wnd_setpos(w, (struct wnd *)0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	return 1;
}

static u32 u_CloseWindow(a) u32 *a; { W(a[0]); wnd_show(w, SW_MINIMIZE); return 0; }
static u32 u_OpenIcon(a) u32 *a; { W(a[0]); wnd_show(w, SW_RESTORE); return 1; }

static u32
u_GetParent(a)
	u32 *a;
{
	W(a[0]);
	if (w->style & WS_CHILD)
		return w->parent && w->parent != desktop ? w->parent->h : 0;
	return w->owner ? w->owner->h : 0;
}

static u32
u_SetParent(a)
	u32 *a;
{
	struct wnd *p = wnd_get(a[1]), *old;
	extern void wnd_reparent();
	W(a[0]);

	old = w->parent;
	wnd_reparent(w, p ? p : desktop);
	return old && old != desktop ? old->h : 0;
}

static u32
u_GetWindow(a)
	u32 *a;
{
	struct wnd *c;
	W(a[0]);

	switch (a[1]) {
	case 0: return w->parent ? w->parent->child ? w->parent->child->h : 0 : 0;
	case 1:
		if (!w->parent)
			return 0;
		for (c = w->parent->child; c && c->next; c = c->next)
			;
		return c ? c->h : 0;
	case 2: return w->next ? w->next->h : 0;
	case 3:
		if (!w->parent)
			return 0;
		for (c = w->parent->child; c && c->next != w; c = c->next)
			;
		return c ? c->h : 0;
	case 4: return w->owner ? w->owner->h : 0;
	case 5: return w->child ? w->child->h : 0;
	}
	return 0;
}

static u32
u_GetTopWindow(a)
	u32 *a;
{
	struct wnd *w = LO16(a[0]) ? wnd_get(a[0]) : desktop;

	return w && w->child ? w->child->h : 0;
}

static u32
u_GetNextWindow(a)
	u32 *a;
{
	u32 b[2];

	b[0] = a[0];
	b[1] = a[1] == 2 ? 2 : 3;
	return u_GetWindow(b);
}

static u32 u_GetDesktopWindow(a) u32 *a; { return desktop->h; }

static u32
u_GetLastActivePopup(a)
	u32 *a;
{
	return a[0];
}

static u32
u_FindWindow(a)
	u32 *a;
{
	struct wnd *w;
	char *t = gptr(a[1]);

	for (w = desktop->child; w; w = w->next) {
		if (a[0] && w->cls != cls_find(a[0], 0))
			continue;
		if (t && strcmp(w->text, t) != 0)
			continue;
		return w->h;
	}
	return 0;
}

static u32
u_EnumWindows(a)
	u32 *a;
{
	struct wnd *w, *n;
	u16 hs[512];
	int k = 0, i;

	for (w = desktop->child; w && k < 512; w = n) {
		n = w->next;
		hs[k++] = w->h;
	}
	for (i = 0; i < k; i++)
		if (wnd_get(hs[i])) {
			cb_begin();
			cb_push16(hs[i]);
			cb_push32(a[1]);
			if (!(cb_call(a[0], 0) & 0xffff))
				break;
		}
	return 1;
}

static int
enumkids(w, proc, lp, hs, k)
	struct wnd *w;
	u32 proc, lp;
	u16 *hs;
	int k;
{
	struct wnd *c;

	for (c = w->child; c && k < 1024; c = c->next) {
		hs[k++] = c->h;
		k = enumkids(c, proc, lp, hs, k);
	}
	return k;
}

static u32
u_EnumChildWindows(a)
	u32 *a;
{
	u16 hs[1024];
	int k, i;
	W(a[0]);

	k = enumkids(w, a[1], a[2], hs, 0);
	for (i = 0; i < k; i++)
		if (wnd_get(hs[i])) {
			cb_begin();
			cb_push16(hs[i]);
			cb_push32(a[2]);
			if (!(cb_call(a[1], 0) & 0xffff))
				break;
		}
	return 1;
}

static u32
u_EnumTaskWindows(a)
	u32 *a;
{
	u32 b[2];

	b[0] = a[1];
	b[1] = a[2];
	return u_EnumWindows(b);
}

static u32 u_GetWindowTask(a) u32 *a; { return curtask ? curtask->t_htask : 0; }

static u32
u_GetWindowText(a)
	u32 *a;
{
	return wnd_sendh(a[0], WM_GETTEXT, a[2], a[1]);
}

static u32 u_SetWindowText(a) u32 *a; { wnd_sendh(a[0], WM_SETTEXT, 0, a[1]); return 0; }
static u32 u_GetWindowTextLength(a) u32 *a; { return wnd_sendh(a[0], WM_GETTEXTLENGTH, 0, 0); }

static u32
u_GetClassName(a)
	u32 *a;
{
	char *d = gptr(a[1]);
	int n = a[2];
	W(a[0]);

	if (!d || n <= 0)
		return 0;
	strncpy(d, w->cls->name, n - 1);
	d[n - 1] = 0;
	return strlen(d);
}

/* window words and longs */
static u32
u_GetWindowWord(a)
	u32 *a;
{
	int off = (short)a[1];
	W(a[0]);

	switch (off) {
	case -6: return w->hinst;
	case -8: return (w->style & WS_CHILD) ? (w->parent != desktop ? w->parent->h : 0) : w->owner ? w->owner->h : 0;
	case -12: return w->id;
	}
	if (off < 0 || off + 2 > w->cls->wndextra)
		return 0;
	return w->extra[off] | w->extra[off + 1] << 8;
}

static u32
u_SetWindowWord(a)
	u32 *a;
{
	int off = (short)a[1];
	u32 o;
	W(a[0]);

	switch (off) {
	case -6: o = w->hinst; w->hinst = a[2]; return o;
	case -12: o = w->id; w->id = a[2]; return o;
	case -8: return 0;
	}
	if (off < 0 || off + 2 > w->cls->wndextra)
		return 0;
	o = w->extra[off] | w->extra[off + 1] << 8;
	w->extra[off] = a[2];
	w->extra[off + 1] = a[2] >> 8;
	return o;
}

static u32
u_GetWindowLong(a)
	u32 *a;
{
	int off = (short)a[1];
	W(a[0]);

	switch (off) {
	case -4: return w->proc;
	case -16: return w->style;
	case -20: return w->exstyle;
	}
	if (off < 0 || off + 4 > w->cls->wndextra)
		return 0;
	return RD32(w->extra, off);
}

static u32
u_SetWindowLong(a)
	u32 *a;
{
	int off = (short)a[1];
	u32 o;
	W(a[0]);

	switch (off) {
	case -4: o = w->proc; w->proc = a[2]; return o;
	case -16: o = w->style; w->style = a[2]; vis_epoch++; return o;
	case -20: o = w->exstyle; w->exstyle = a[2]; return o;
	}
	if (off < 0 || off + 4 > w->cls->wndextra)
		return 0;
	o = RD32(w->extra, off);
	WR32(w->extra, off, a[2]);
	return o;
}

static u32
u_GetClassWord(a)
	u32 *a;
{
	int off = (short)a[1];
	struct cls *c;
	W(a[0]);

	c = w->cls;
	switch (off) {
	case -10: return c->bg;
	case -12: return c->cursor;
	case -14: return c->icon;
	case -16: return c->hinst;
	case -18: return c->wndextra;
	case -20: return c->clsextra;
	case -26: return c->style;
	case -32: return c->atom;
	}
	if (off < 0 || off + 2 > c->clsextra)
		return 0;
	return c->extra[off] | c->extra[off + 1] << 8;
}

static u32
u_SetClassWord(a)
	u32 *a;
{
	int off = (short)a[1];
	struct cls *c;
	u32 o;
	W(a[0]);

	c = w->cls;
	switch (off) {
	case -10: o = c->bg; c->bg = a[2]; return o;
	case -12: o = c->cursor; c->cursor = a[2]; return o;
	case -14: o = c->icon; c->icon = a[2]; return o;
	case -26: o = c->style; c->style = a[2]; return o;
	}
	if (off < 0 || off + 2 > c->clsextra)
		return 0;
	o = c->extra[off] | c->extra[off + 1] << 8;
	c->extra[off] = a[2];
	c->extra[off + 1] = a[2] >> 8;
	return o;
}

static u32
u_GetClassLong(a)
	u32 *a;
{
	int off = (short)a[1];
	W(a[0]);

	if (off == -24)
		return w->cls->proc;
	if (off == -8)
		return w->cls->menuname;
	if (off < 0 || off + 4 > w->cls->clsextra)
		return 0;
	return RD32(w->cls->extra, off);
}

static u32
u_SetClassLong(a)
	u32 *a;
{
	int off = (short)a[1];
	u32 o;
	W(a[0]);

	if (off == -24) {
		o = w->cls->proc;
		w->cls->proc = a[2];
		return o;
	}
	if (off < 0 || off + 4 > w->cls->clsextra)
		return 0;
	o = RD32(w->cls->extra, off);
	WR32(w->cls->extra, off, a[2]);
	return o;
}

/* properties */
static struct prop *
findprop(w, p)
	struct wnd *w;
	u32 p;
{
	struct prop *pr;
	u32 at = ISINT(p) ? FPOFF(p) : 0;

	for (pr = w->props; pr; pr = pr->next)
		if (at ? pr->atom == at : (pr->name && w16_stricmp(pr->name, STR(p)) == 0))
			return pr;
	return 0;
}

static u32
u_SetProp(a)
	u32 *a;
{
	struct prop *p;
	W(a[0]);

	if ((p = findprop(w, a[1])) == 0) {
		p = (struct prop *)calloc(1, sizeof *p);
		if (ISINT(a[1]))
			p->atom = FPOFF(a[1]);
		else
			p->name = strdup(STR(a[1]));
		p->next = w->props;
		w->props = p;
	}
	p->val = a[2];
	return 1;
}

static u32
u_GetProp(a)
	u32 *a;
{
	struct prop *p;
	W(a[0]);

	p = findprop(w, a[1]);
	return p ? p->val : 0;
}

static u32
u_RemoveProp(a)
	u32 *a;
{
	struct prop *p, **pp;
	u16 v;
	W(a[0]);

	if ((p = findprop(w, a[1])) == 0)
		return 0;
	for (pp = &w->props; *pp != p; pp = &(*pp)->next)
		;
	*pp = p->next;
	v = p->val;
	if (p->name)
		free(p->name);
	free(p);
	return v;
}

static u32
u_EnumProps(a)
	u32 *a;
{
	struct prop *p;
	u32 r = 1, s;
	W(a[0]);

	for (p = w->props; p && r; p = p->next) {
		s = p->name ? ustr(p->name) : FP(0, p->atom);
		cb_begin();
		cb_push16(w->h);
		cb_push32(s);
		cb_push16(p->val);
		r = cb_call(a[1], 0) & 0xffff;
		if (p->name)
			ufree(s);
	}
	return r;
}

/* ---- focus, activation, capture ---- */

static u32
u_SetFocus(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]), *old = wnd_focus;

	if (w && (w->style & WS_DISABLED))
		return old ? old->h : 0;
	if (w && wnd_toplevel(w) != wnd_active)
		wnd_activate(w, WA_ACTIVE);
	wnd_setfocus(w);
	return old && wnd_get(old->h) ? old->h : 0;
}

static u32 u_GetFocus(a) u32 *a; { return wnd_focus ? wnd_focus->h : 0; }

static u32
u_SetActiveWindow(a)
	u32 *a;
{
	struct wnd *old = wnd_active;
	W(a[0]);

	wnd_activate(w, WA_ACTIVE);
	return old ? old->h : 0;
}

static u32 u_GetActiveWindow(a) u32 *a; { return wnd_active ? wnd_active->h : 0; }

static u32
u_SetCapture(a)
	u32 *a;
{
	struct wnd *old = wnd_capture;

	wnd_capture = wnd_get(a[0]);
	return old ? old->h : 0;
}

static u32 u_ReleaseCapture(a) u32 *a; { wnd_capture = 0; return 0; }
static u32 u_GetCapture(a) u32 *a; { return wnd_capture ? wnd_capture->h : 0; }

static u32
u_SetSysModalWindow(a)
	u32 *a;
{
	return 0;
}

/* ---- painting ---- */

static u32
u_BeginPaint(a)
	u32 *a;
{
	u32 p = LIN(a[1]);
	W(a[0]);

	return user_beginpaint(w, p);
}

static u32
u_EndPaint(a)
	u32 *a;
{
	user_endpaint(wnd_get(a[0]), LIN(a[1]));
	return 0;
}

static u32
u_GetDC(a)
	u32 *a;
{
	struct wnd *w = LO16(a[0]) ? wnd_get(a[0]) : desktop;

	if (!w)
		return 0;
	return user_dc(w, DCK_WINDOW, (struct rgn *)0);
}

static u32
u_GetWindowDC(a)
	u32 *a;
{
	struct wnd *w = LO16(a[0]) ? wnd_get(a[0]) : desktop;

	if (!w)
		return 0;
	return user_dc(w, DCK_WINDOWNC, (struct rgn *)0);
}

static u32
u_GetDCEx(a)
	u32 *a;
{
	return u_GetDC(a);
}

static u32 u_ReleaseDC(a) u32 *a; { user_releasedc(a[1]); return 1; }

static u32
u_WindowFromDC(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);

	return dc ? dc->hwnd : 0;
}

/* a client rectangle (guest RECT or all) to the screen */
static void
toscreen(w, p, r)
	struct wnd *w;
	u32 p;
	struct rect *r;
{
	if (p) {
		r_get(r, p);
		r->l += w->cr.l;
		r->r += w->cr.l;
		r->t += w->cr.t;
		r->b += w->cr.t;
	} else
		*r = w->cr;
}

/*
 * InvalidateRect/InvalidateRgn: the window, and its children there too
 * unless it clips them (WS_CLIPCHILDREN), as Windows does: Control
 * Panel repaints its applet pane by invalidating the frame.
 */
static void
invalidate(w, r, erase)
	struct wnd *w;
	struct rect *r;
	int erase;
{
	struct wnd *c;
	struct rect m;

	wnd_invalidate(w, r, erase);
	if (w->style & WS_CLIPCHILDREN)
		return;
	for (c = w->child; c; c = c->next)
		if ((c->style & WS_VISIBLE) && r_and(&m, r, &c->wr) && r_and(&m, &m, &w->cr))
			invalidate(c, &m, erase);
}

static u32
u_InvalidateRect(a)
	u32 *a;
{
	struct rect r;
	struct wnd *w = LO16(a[0]) ? wnd_get(a[0]) : desktop;

	if (!w)
		return 0;
	if (w == desktop && !a[1]) {
		expose(&desktop->wr);
		return 0;
	}
	toscreen(w, LIN(a[1]), &r);
	invalidate(w, &r, a[2]);
	return 0;
}

static u32
u_InvalidateRgn(a)
	u32 *a;
{
	struct gobj *o = gobj(a[1], OBJ_RGN);
	struct rect r;
	int i;
	W(a[0]);

	if (!o) {
		invalidate(w, &w->cr, a[2]);
		return 0;
	}
	for (i = 0; i < o->u.rgn.n; i++) {
		r = o->u.rgn.r[i];
		r.l += w->cr.l; r.r += w->cr.l; r.t += w->cr.t; r.b += w->cr.t;
		invalidate(w, &r, a[2]);
	}
	return 0;
}

static u32
u_ValidateRect(a)
	u32 *a;
{
	struct rect r;
	W(a[0]);

	toscreen(w, LIN(a[1]), &r);
	rgn_subrect(&w->upd, &r);
	return 0;
}

static u32
u_ValidateRgn(a)
	u32 *a;
{
	struct gobj *o = gobj(a[1], OBJ_RGN);
	struct rgn t;
	W(a[0]);

	if (!o) {
		w->upd.n = 0;
		return 0;
	}
	rgn_init(&t);
	rgn_copy(&t, &o->u.rgn);
	rgn_offset(&t, w->cr.l, w->cr.t);
	rgn_diff(&w->upd, &w->upd, &t);
	rgn_free(&t);
	return 0;
}

static u32
u_GetUpdateRect(a)
	u32 *a;
{
	u32 p = LIN(a[1]);
	struct rect r;
	W(a[0]);

	if (w->upd.n) {
		r = w->upd.box;
		r.l -= w->cr.l; r.r -= w->cr.l; r.t -= w->cr.t; r.b -= w->cr.t;
	} else
		r_set(&r, 0, 0, 0, 0);
	if (p)
		r_put(p, &r);
	if (a[2] && w->upd.n && w->erase) {
		u16 hdc = user_dc(w, DCK_WINDOW, &w->upd);

		w->erase = !wnd_send(w, WM_ERASEBKGND, hdc, 0);
		user_releasedc(hdc);
	}
	return w->upd.n != 0;
}

static u32
u_GetUpdateRgn(a)
	u32 *a;
{
	struct gobj *o = gobj(a[1], OBJ_RGN);
	W(a[0]);

	if (!o)
		return 0;
	rgn_copy(&o->u.rgn, &w->upd);
	rgn_offset(&o->u.rgn, -w->cr.l, -w->cr.t);
	return rgn_kind(&o->u.rgn);
}

static u32
u_RedrawWindow(a)
	u32 *a;
{
	struct rect r;
	u32 fl = a[3];
	W(a[0]);

	if (fl & 0x0001) {		/* RDW_INVALIDATE */
		toscreen(w, LIN(a[1]), &r);
		wnd_invalidate(w, &r, fl & 0x0004);
		if (fl & 0x0400)	/* RDW_FRAME */
			w->ncpaint = 1;
	}
	if (fl & 0x0008)		/* RDW_VALIDATE */
		w->upd.n = 0;
	if (fl & 0x0100)		/* RDW_UPDATENOW */
		wnd_update(w);
	return 1;
}

/* ScrollWindow: the pixels move, what is uncovered is invalidated */
static u32
u_ScrollWindow(a)
	u32 *a;
{
	int dx = (short)a[1], dy = (short)a[2];
	struct rect r, c, src;
	struct rgn vis, inv;
	u16 hdc;
	struct dc *dc;
	struct wnd *ch;
	extern void caret_hide(), caret_show();
	W(a[0]);

	toscreen(w, LIN(a[3]), &r);
	if (a[4]) {
		toscreen(w, LIN(a[4]), &c);
		r_and(&r, &r, &c);
	} else
		r_and(&r, &r, &w->cr);
	caret_hide();
	rgn_init(&vis);
	rgn_init(&inv);
	wnd_visrgn(w, &vis, 1);
	hdc = user_dc(w, DCK_WINDOW, (struct rgn *)0);
	dc = dc_get(hdc);
	/* copy what is visible both before and after */
	src = r;
	{
		struct rgn ok, sh;

		rgn_init(&ok);
		rgn_init(&sh);
		rgn_copy(&ok, &vis);
		rgn_andrect(&ok, &r);
		rgn_copy(&sh, &ok);
		rgn_offset(&sh, dx, dy);
		rgn_and(&sh, &sh, &ok);
		rgn_copy(&dc->eff, &sh);
		dc->effok = 1;
		d_blt(dc, r.l + dx, r.t + dy, r.r - r.l, r.b - r.t, dc, r.l, r.t, SRCCOPY);
		/* everything in r not refreshed by the copy repaints */
		rgn_set(&inv, &r);
		rgn_diff(&inv, &inv, &sh);
		rgn_free(&ok);
		rgn_free(&sh);
	}
	user_releasedc(hdc);
	{
		int i;

		for (i = 0; i < inv.n; i++)
			wnd_invalidate(w, &inv.r[i], 1);
	}
	/* a pending update moves along */
	rgn_offset(&w->upd, dx, dy);
	if (!a[3])
		for (ch = w->child; ch; ch = ch->next)
			wnd_setpos(ch, (struct wnd *)0, ch->wr.l - w->cr.l + dx, ch->wr.t - w->cr.t + dy, 0, 0,
			    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	rgn_free(&vis);
	rgn_free(&inv);
	caret_show();
	(void)src;
	return 0;
}

static u32
u_ScrollDC(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);
	struct rect r;
	u32 p = LIN(a[3]);

	if (!dc || !p)
		return 0;
	r_get(&r, p);
	r.l += dc->ox; r.r += dc->ox; r.t += dc->oy; r.b += dc->oy;
	d_blt(dc, r.l + (short)a[1], r.t + (short)a[2], r.r - r.l, r.b - r.t, dc, r.l, r.t, SRCCOPY);
	if (LIN(a[6])) {
		r.l -= dc->ox; r.r -= dc->ox; r.t -= dc->oy; r.b -= dc->oy;
		r_put(LIN(a[6]), &r);
	}
	return 1;
}

static u32 u_LockWindowUpdate(a) u32 *a; { return 1; }

static u32
u_FillRect(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);
	u32 p = LIN(a[1]);
	struct rect r;
	int l, t, rr, b;
	u16 br = a[2];

	if (!dc || !p)
		return 0;
	r_get(&r, p);
	l = r.l; t = r.t; rr = r.r; b = r.b;
	lp2dp(dc, &l, &t);
	lp2dp(dc, &rr, &b);
	r_set(&r, l + dc->ox, t + dc->oy, rr + dc->ox, b + dc->oy);
	if (br >= 1 && br <= NSYSCOLOR + 1 && !gobj(br, OBJ_BRUSH))
		br = sys_brush(br - 1);
	d_fill(dc, &r, br, 0xf0);
	return 1;
}

static u32
u_FrameRect(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);
	u32 p = LIN(a[1]);
	struct rect r, e;
	int l, t, rr, b;

	if (!dc || !p)
		return 0;
	r_get(&r, p);
	l = r.l; t = r.t; rr = r.r; b = r.b;
	lp2dp(dc, &l, &t);
	lp2dp(dc, &rr, &b);
	r_set(&r, l + dc->ox, t + dc->oy, rr + dc->ox, b + dc->oy);
	r_set(&e, r.l, r.t, r.r, r.t + 1); d_fill(dc, &e, a[2], 0xf0);
	r_set(&e, r.l, r.b - 1, r.r, r.b); d_fill(dc, &e, a[2], 0xf0);
	r_set(&e, r.l, r.t, r.l + 1, r.b); d_fill(dc, &e, a[2], 0xf0);
	r_set(&e, r.r - 1, r.t, r.r, r.b); d_fill(dc, &e, a[2], 0xf0);
	return 1;
}

static u32
u_InvertRect(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);
	u32 p = LIN(a[1]);
	struct rect r;
	int l, t, rr, b;

	if (!dc || !p)
		return 0;
	r_get(&r, p);
	l = r.l; t = r.t; rr = r.r; b = r.b;
	lp2dp(dc, &l, &t);
	lp2dp(dc, &rr, &b);
	r_set(&r, l + dc->ox, t + dc->oy, rr + dc->ox, b + dc->oy);
	d_invert(dc, &r);
	return 1;
}

static u32
u_DrawFocusRect(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);
	u32 p = LIN(a[1]);
	struct rect r;
	int x, y;

	if (!dc || !p)
		return 0;
	r_get(&r, p);
	r.l += dc->ox; r.r += dc->ox; r.t += dc->oy; r.b += dc->oy;
	{
		struct rgn *g = dc_clip(dc);
		u8 *px;

		for (x = r.l; x < r.r; x += 2) {
			if (rgn_ptin(g, x, r.t)) { px = dc->s->pix + r.t * dc->s->rowb + x; *px = ~*px; }
			if (rgn_ptin(g, x, r.b - 1)) { px = dc->s->pix + (r.b - 1) * dc->s->rowb + x; *px = ~*px; }
		}
		for (y = r.t + 2; y < r.b - 1; y += 2) {
			if (rgn_ptin(g, r.l, y)) { px = dc->s->pix + y * dc->s->rowb + r.l; *px = ~*px; }
			if (rgn_ptin(g, r.r - 1, y)) { px = dc->s->pix + y * dc->s->rowb + r.r - 1; *px = ~*px; }
		}
		scr_dirty(&r);
	}
	return 1;
}

static u32
u_DrawText(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);
	char *s = gptr(a[1]);
	u32 p = LIN(a[3]);
	struct rect r;
	int l, t, rr, b, h;

	if (!dc || !s || !p)
		return 0;
	r_get(&r, p);
	l = r.l; t = r.t; rr = r.r; b = r.b;
	lp2dp(dc, &l, &t);
	lp2dp(dc, &rr, &b);
	r_set(&r, l + dc->ox, t + dc->oy, rr + dc->ox, b + dc->oy);
	h = draw_text(dc, s, (short)a[2], &r, a[4]);
	if (a[4] & DT_CALCRECT) {
		l = r.l - dc->ox; t = r.t - dc->oy; rr = r.r - dc->ox; b = r.b - dc->oy;
		dp2lp(dc, &l, &t);
		dp2lp(dc, &rr, &b);
		r_set(&r, l, t, rr, b);
		r_put(p, &r);
	}
	return h;
}

/* TabbedTextOut: tabs to the stops given (or every 8 average characters) */
static u32
tabbed(a, draw)
	u32 *a;
	int draw;
{
	struct dc *dc = dc_get(a[0]);
	char *s = gptr(a[3]);
	int n = (short)a[4], ntabs = (short)a[5], origin = (short)a[7], x = (short)a[1], y = (short)a[2];
	u32 tp = LIN(a[6]);
	struct bfont *f;
	int i, start, w = 0, t, stop;

	if (!dc || !s)
		return 0;
	f = font_of(dc);
	if (!draw) {
		n = (short)a[2];
		s = gptr(a[1]);
		ntabs = (short)a[3];
		tp = LIN(a[4]);
		origin = 0;
		x = 0;
		y = 0;
		if (!s)
			return 0;
	}
	for (start = 0, i = 0; i <= n; i++) {
		if (i < n && s[i] != '\t')
			continue;
		if (draw && i > start) {
			int sx = x + w, sy = y;

			tosurf_pub(dc, &sx, &sy);
			text_draw(dc, sx, sy, s + start, i - start, (struct rect *)0, (struct rect *)0, (int *)0);
		}
		w += text_width(f, s + start, i - start);
		if (i < n) {
			stop = 0;
			for (t = 0; t < (ntabs > 0 ? ntabs : 0); t++) {
				int ts = (short)GW(tp + 2 * t) - origin + (draw ? 0 : 0);

				if (ts > w) {
					stop = ts;
					break;
				}
			}
			if (!stop) {
				int step = ntabs == 1 && tp ? (short)GW(tp) : 8 * f->f_avgw;

				if (step <= 0)
					step = 8 * f->f_avgw;
				stop = (w / step + 1) * step;
			}
			w = stop;
		}
		start = i + 1;
	}
	return FP(f->f_height, w);
}

static u32 u_TabbedTextOut(a) u32 *a; { return tabbed(a, 1); }
static u32 u_GetTabbedTextExtent(a) u32 *a; { return tabbed(a, 0); }

static u32
u_GrayString(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);
	char *s = gptr(a[3]);
	int x = (short)a[5], y = (short)a[6], n = (short)a[4];
	COLORREF keep;

	if (!dc)
		return 0;
	if (a[2] && !s) {
		/* an output procedure draws it */
		cb_begin();
		cb_push16(a[0]);
		cb_push32(a[3]);
		cb_push16(n);
		return cb_call(a[2], 0) & 0xffff;
	}
	if (!s)
		return 0;
	if (n <= 0)
		n = strlen(s);
	keep = dc->st.text;
	dc->st.text = sys_color(COLOR_GRAYTEXT);
	tosurf_pub(dc, &x, &y);
	text_draw(dc, x, y, s, n, (struct rect *)0, (struct rect *)0, (int *)0);
	dc->st.text = keep;
	return 1;
}

static u32
u_DrawIcon(a)
	u32 *a;
{
	struct dc *dc = dc_get(a[0]);
	int x = (short)a[1], y = (short)a[2];

	if (!dc)
		return 0;
	tosurf_pub(dc, &x, &y);
	icon_draw(dc, a[3], x, y);
	return 1;
}

/* ---- messages ---- */

static u32
u_GetMessage(a)
	u32 *a;
{
	u32 p = LIN(a[0]);
	int r;

	if (!p)
		return 0;
	r = user_getmessage(p, a[1], a[2], a[3], 1, 1);
	return r < 0 ? 0 : 1;
}

static u32
u_PeekMessage(a)
	u32 *a;
{
	u32 p = LIN(a[0]);
	int r;

	if (!p)
		return 0;
	r = user_getmessage(p, a[1], a[2], a[3], a[4] & 1, 0);
	return r != 0;
}

static u32
u_WaitMessage(a)
	u32 *a;
{
	u32 p = ualloc(MSG_SIZE);

	user_getmessage(ulin(p), 0, 0, 0, 0, 1);
	ufree(p);
	return 0;
}

static u32 u_TranslateMessage(a) u32 *a; { u32 p = LIN(a[0]); return p ? user_translate(p) : 0; }
static u32 u_DispatchMessage(a) u32 *a; { u32 p = LIN(a[0]); return p ? user_dispatch(p) : 0; }

static u32
u_SendMessage(a)
	u32 *a;
{
	struct wnd *w, *n;

	if (LO16(a[0]) == 0xffff) {
		for (w = desktop->child; w; w = n) {
			n = w->next;
			wnd_send(w, a[1], a[2], a[3]);
		}
		return 0;
	}
	return wnd_sendh(a[0], a[1], a[2], a[3]);
}

static u32
u_PostMessage(a)
	u32 *a;
{
	struct wnd *w;

	if (LO16(a[0]) == 0xffff) {
		for (w = desktop->child; w; w = w->next)
			wnd_post(w->h, a[1], a[2], a[3]);
		return 1;
	}
	if (LO16(a[0]) && !wnd_get(a[0]))
		return 0;
	return wnd_post(a[0], a[1], a[2], a[3]);
}

static u32
u_PostAppMessage(a)
	u32 *a;
{
	extern int user_posttask();
	int i;

	for (i = 0; i < NTASK_MAX; i++)
		if (tasks[i] && tasks[i]->t_htask == (a[0] & 0xffff))
			return user_posttask(tasks[i], a[1], a[2], a[3]);
	return a[0] & 0xffff ? 0 : wnd_post(0, a[1], a[2], a[3]);
}

static u32 u_PostQuitMessage(a) u32 *a; { user_postquit((short)a[0]); return 0; }
static u32 u_ReplyMessage(a) u32 *a; { return 0; }
static u32 u_InSendMessage(a) u32 *a; { return 0; }
static u32 u_GetMessagePos(a) u32 *a; { return FP(scr_my, scr_mx); }
static u32 u_GetMessageTime(a) u32 *a; { return w16_ticks(); }
static u32 u_GetMessageExtraInfo(a) u32 *a; { return 0; }

static u32
u_CallWindowProc(a)
	u32 *a;
{
	return wnd_call(a[0], a[1], a[2], a[3], a[4]);
}

static u32
u_DefWindowProc(a)
	u32 *a;
{
	return user_defproc(wnd_get(a[0]), a[1], a[2], a[3]);
}

static u32
u_RegisterWindowMessage(a)
	u32 *a;
{
	u32 at = atom_add(a[0]);

	return at >= 0xc000 ? at : 0xc000 + at;
}

static u32
u_GetInputState(a)
	u32 *a;
{
	return 0;
}

static u32 u_GetQueueStatus(a) u32 *a; { return 0; }

/* hooks: kept, never called */
static u32 u_SetWindowsHook(a) u32 *a; { return 0; }
static u32 u_SetWindowsHookEx(a) u32 *a; { return FP(0x7fff, ((short)a[0] + 2) & 0xff); }
static u32 u_UnhookWindowsHook(a) u32 *a; { return 1; }
static u32 u_DefHookProc(a) u32 *a; { return 0; }
static u32 u_CallMsgFilter(a) u32 *a; { return 0; }

/* timers */
static u32 u_SetTimer(a) u32 *a; { return user_settimer(a[0], a[1], a[2], a[3]); }
static u32 u_KillTimer(a) u32 *a; { return user_killtimer(a[0], a[1]); }
static u32 u_SetSystemTimer(a) u32 *a; { return user_setsystimer(a[0], a[1], a[2], a[3]); }
static u32 u_GetTickCount(a) u32 *a; { return w16_ticks(); }
static u32 u_GetTimerResolution(a) u32 *a; { return 1000; }

/* ---- the caret ---- */

static u32 u_CreateCaret(a) u32 *a; { return user_createcaret(a[0], a[1], (short)a[2], (short)a[3]); }
static u32 u_DestroyCaret(a) u32 *a; { user_destroycaret(); return 0; }
static u32 u_SetCaretPos(a) u32 *a; { user_setcaretpos((short)a[0], (short)a[1]); return 0; }
static u32 u_HideCaret(a) u32 *a; { user_showcaret(a[0], 0); return 0; }
static u32 u_ShowCaret(a) u32 *a; { user_showcaret(a[0], 1); return 0; }
static u32 u_GetCaretBlinkTime(a) u32 *a; { return 500; }

static u32
u_GetCaretPos(a)
	u32 *a;
{
	u32 p = LIN(a[0]), v = user_getcaretpos();

	if (p) {
		PW(p, v);
		PW(p + 2, v >> 16);
	}
	return 0;
}

/* ---- the cursor and keyboard ---- */

static u32
u_SetCursor(a)
	u32 *a;
{
	u16 o = cur_get();

	cur_set(a[0]);
	return o;
}

static u32 u_GetCursor(a) u32 *a; { return cur_get(); }

static u32
u_ShowCursor(a)
	u32 *a;
{
	static int count;

	count += a[0] ? 1 : -1;
	return count;
}

static u32
u_GetCursorPos(a)
	u32 *a;
{
	u32 p = LIN(a[0]);

	if (p) {
		PW(p, scr_mx);
		PW(p + 2, scr_my);
	}
	return 0;
}

static u32
u_SetCursorPos(a)
	u32 *a;
{
	scr_warp((short)a[0], (short)a[1]);
	return 0;
}

static u32 u_ClipCursor(a) u32 *a; { return 1; }
static u32 u_GetKeyState(a) u32 *a; { int v = user_keystate(a[0]); return (v & 0x80 ? 0xff80 : 0) | (v & 1); }
static u32 u_GetAsyncKeyState(a) u32 *a; { extern int user_asyncstate(); return user_asyncstate(a[0]) & 0x80 ? 0x8000 : 0; }

static u32
u_GetKeyboardState(a)
	u32 *a;
{
	u32 p = LIN(a[0]);

	if (p)
		memcpy(M + p, keystate, 256);
	return 0;
}

static u32
u_SetKeyboardState(a)
	u32 *a;
{
	u32 p = LIN(a[0]);

	if (p)
		memcpy(keystate, M + p, 256);
	return 0;
}

/*
 * MessageBeep, and USER's own beeps: the sound WIN.INI's [sounds] sets
 * for the kind, played by the user's MMSYSTEM, as Windows 3.1 does when
 * it is there; else a plain beep.  [windows] Beep=no silences them.
 */
void
user_beep(type)
	int type;
{
	extern void mm_beep();
	static char *names[] = { "SystemDefault", "SystemHand", "SystemQuestion", "SystemExclamation",
		"SystemAsterisk" };
	struct module *m = mod_find("MMSYSTEM");
	char yes[8];
	u32 fn, s;

	if (profile_get((char *)0, "windows", "Beep", "yes", yes, sizeof yes) > 0 && !w16_stricmp(yes, "no"))
		return;
	type &= 0xffff;
	if (type != 0xffff && m && !m->m_native && (fn = mod_proc(m, 0, "sndPlaySound")) != 0) {
		s = ustr(names[type >= 0x10 && type <= 0x40 ? type >> 4 : 0]);
		cb_begin();
		cb_push32(s);
		cb_push16(3);		/* SND_ASYNC | SND_NODEFAULT */
		if (cb_call(fn, 0) & 0xffff) {
			ufree(s);
			return;
		}
		ufree(s);
	}
	mm_beep();
}

static u32 u_MessageBeep(a) u32 *a; { user_beep(a[0]); return 0; }
static u32 u_GetDoubleClickTime(a) u32 *a; { return 500; }
static u32 u_GetSystemMetrics(a) u32 *a; { return sys_metric((short)a[0]); }
static u32 u_GetSysColor(a) u32 *a; { return sys_color(a[0]); }
static u32 u_GetSysColorBrush(a) u32 *a; { return sys_brush(a[0]); }

static u32
u_FlashWindow(a)
	u32 *a;
{
	return 0;
}

/* ---- resources ---- */

static u32
u_LoadCursor(a)
	u32 *a;
{
	return cur_load(a[0], a[1]);
}

static u32
u_LoadIcon(a)
	u32 *a;
{
	return icon_load(a[0], a[1]);
}

static u32 u_DestroyIcon(a) u32 *a; { ico_destroy(a[0]); return 1; }

static u32
u_CreateIcon(a)
	u32 *a;
{
	u32 and = LIN(a[5]), xor = LIN(a[6]);

	if (!and || !xor)
		return 0;
	return ico_create((short)a[1], (short)a[2], 16, 16, and, xor, a[3] * a[4], 0);
}

static u32
u_CreateCursor(a)
	u32 *a;
{
	u32 and = LIN(a[5]), xor = LIN(a[6]);

	if (!and || !xor)
		return 0;
	return ico_create((short)a[3], (short)a[4], (short)a[1], (short)a[2], and, xor, 1, 1);
}

static u32
u_LoadBitmap(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0]);
	u32 d, size;

	if (!LO16(a[0])) {
		extern u16 obm_load();

		return FPSEL(a[1]) ? 0 : obm_load(FPOFF(a[1]));	/* the system's (OBM_*) */
	}
	if (!m)
		return 0;
	d = res_data(m, FP(0, RT_BITMAP), a[1], &size);
	if (!d)
		return 0;
	return bitmap_handle(dib_bitmap(d, 0, 0, 1, 0, 0));
}

static u32
u_LoadString(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0]);
	u32 d, size, p;
	char *out = gptr(a[2]);
	int id = a[1] & 0xffff, i, n, max = (short)a[3];

	if (!m || !out || max <= 0)
		return 0;
	d = res_data(m, FP(0, RT_STRING), FP(0, (id >> 4) + 1), &size);
	if (!d)
		return 0;
	p = d;
	for (i = 0; i < (id & 15); i++)
		p += 1 + M[p];
	n = M[p];
	if (n > max - 1)
		n = max - 1;
	memcpy(out, M + p + 1, n);
	out[n] = 0;
	return n;
}

static u32
u_LoadAccelerators(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0]);
	u32 h = res_find(m, FP(0, RT_ACCELERATOR), a[1]);

	return h ? res_load(m, h) : 0;
}

/*
 * An accelerator's command, as Windows sends it: when it is an item of
 * the system menu or the window's menu, the program hears WM_INITMENU
 * (and WM_INITMENUPOPUP for its popup) to set the items up, and a
 * disabled or greyed item sends nothing (Program Manager counts on it:
 * Enter in an empty group is its Open, greyed); nor does a menu item
 * while the window is iconic.  Else WM_COMMAND.
 */
static void
accel(w, cmd)
	struct wnd *w;
	int cmd;
{
	extern int menu_cmdstate();
	u16 pop;
	int pos = 0, st, ok = !(w->style & WS_DISABLED) && !wnd_capture;
	u32 bar = (w->style & WS_CHILD) ? 0 : w->id;

	if (w->sysmenu && (st = menu_cmdstate(w->sysmenu, cmd, &pop, &pos)) >= 0) {
		if (!ok)
			return;
		wnd_send(w, WM_INITMENU, w->sysmenu, 0);
		if (pop != w->sysmenu)
			wnd_send(w, WM_INITMENUPOPUP, pop, FP(1, pos));
		if ((st = menu_cmdstate(w->sysmenu, cmd, &pop, &pos)) >= 0 && !(st & 3))
			wnd_send(w, WM_SYSCOMMAND, cmd, FP(1, 0));
		return;
	}
	if (bar && (st = menu_cmdstate(bar, cmd, &pop, &pos)) >= 0) {
		if (ok) {
			wnd_send(w, WM_INITMENU, bar, 0);
			if (pop != (u16)bar && wnd_get(w->h))
				wnd_send(w, WM_INITMENUPOPUP, pop, FP(0, pos));
			if (!wnd_get(w->h))
				return;
			st = menu_cmdstate(w->id, cmd, &pop, &pos);
		}
		if (st < 0 || (st & 3) || (w->style & WS_MINIMIZE))
			return;
	}
	wnd_send(w, WM_COMMAND, cmd, FP(1, 0));
}

static u32
u_TranslateAccelerator(a)
	u32 *a;
{
	u32 t = lin(a[1], 0), msg = LIN(a[2]), e;
	int m, key, virt, fl, shift, ctrl, alt;
	struct wnd *w = wnd_get(a[0]);

	if (!t || !msg || !w)
		return 0;
	m = GW(msg + MSG_MESSAGE);
	key = GW(msg + MSG_WPARAM);
	if (m != WM_KEYDOWN && m != WM_SYSKEYDOWN && m != WM_CHAR && m != WM_SYSCHAR)
		return 0;
	virt = m == WM_KEYDOWN || m == WM_SYSKEYDOWN;
	shift = (keystate[VK_SHIFT] & 0x80) != 0;
	ctrl = (keystate[VK_CONTROL] & 0x80) != 0;
	alt = (keystate[VK_MENU] & 0x80) != 0;
	for (e = t; ; e += 5) {
		fl = M[e];
		if (((fl & 1) != 0) == virt) {
			int k = GW(e + 1);

			if (virt ? k == key && ((fl & 4) != 0) == shift && ((fl & 8) != 0) == ctrl &&
			    ((fl & 0x10) != 0) == alt : (k == key && ((fl & 0x10) != 0) == alt)) {
				accel(w, GW(e + 3));
				return 1;
			}
		}
		if (fl & 0x80)
			break;
	}
	return 0;
}

/* ---- rectangles ---- */

static u32
u_SetRect(a)
	u32 *a;
{
	u32 p = LIN(a[0]);

	if (p) {
		PW(p, a[1]);
		PW(p + 2, a[2]);
		PW(p + 4, a[3]);
		PW(p + 6, a[4]);
	}
	return 0;
}

static u32 u_SetRectEmpty(a) u32 *a; { u32 p = LIN(a[0]); if (p) memset(M + p, 0, 8); return 0; }
static u32 u_CopyRect(a) u32 *a; { u32 d = LIN(a[0]), s = LIN(a[1]); if (d && s) memmove(M + d, M + s, 8); return 1; }

static u32
u_IsRectEmpty(a)
	u32 *a;
{
	u32 p = LIN(a[0]);
	struct rect r;

	if (!p)
		return 1;
	r_get(&r, p);
	return R_EMPTY(&r);
}

static u32
u_PtInRect(a)
	u32 *a;
{
	u32 p = LIN(a[0]);
	struct rect r;
	int x = (short)LO16(a[1]), y = (short)HI16(a[1]);

	if (!p)
		return 0;
	r_get(&r, p);
	return x >= r.l && x < r.r && y >= r.t && y < r.b;
}

static u32
u_OffsetRect(a)
	u32 *a;
{
	u32 p = LIN(a[0]);
	struct rect r;

	if (!p)
		return 0;
	r_get(&r, p);
	r.l += (short)a[1]; r.r += (short)a[1];
	r.t += (short)a[2]; r.b += (short)a[2];
	r_put(p, &r);
	return 0;
}

static u32
u_InflateRect(a)
	u32 *a;
{
	u32 p = LIN(a[0]);
	struct rect r;

	if (!p)
		return 0;
	r_get(&r, p);
	r.l -= (short)a[1]; r.r += (short)a[1];
	r.t -= (short)a[2]; r.b += (short)a[2];
	r_put(p, &r);
	return 0;
}

static u32
u_IntersectRect(a)
	u32 *a;
{
	u32 d = LIN(a[0]), x = LIN(a[1]), y = LIN(a[2]);
	struct rect r1, r2, r;
	int k;

	if (!d || !x || !y)
		return 0;
	r_get(&r1, x);
	r_get(&r2, y);
	k = r_and(&r, &r1, &r2);
	r_put(d, &r);
	return k;
}

static u32
u_UnionRect(a)
	u32 *a;
{
	u32 d = LIN(a[0]), x = LIN(a[1]), y = LIN(a[2]);
	struct rect r1, r2, r;

	if (!d || !x || !y)
		return 0;
	r_get(&r1, x);
	r_get(&r2, y);
	if (R_EMPTY(&r1))
		r = r2;
	else if (R_EMPTY(&r2))
		r = r1;
	else
		r_set(&r, r1.l < r2.l ? r1.l : r2.l, r1.t < r2.t ? r1.t : r2.t,
		    r1.r > r2.r ? r1.r : r2.r, r1.b > r2.b ? r1.b : r2.b);
	r_put(d, &r);
	return !R_EMPTY(&r);
}

static u32
u_SubtractRect(a)
	u32 *a;
{
	u32 d = LIN(a[0]), x = LIN(a[1]), y = LIN(a[2]);
	struct rect r1, r2;

	if (!d || !x || !y)
		return 0;
	r_get(&r1, x);
	r_get(&r2, y);
	if (r2.t <= r1.t && r2.b >= r1.b) {
		if (r2.l <= r1.l && r2.r > r1.l) r1.l = r2.r;
		else if (r2.r >= r1.r && r2.l < r1.r) r1.r = r2.l;
	} else if (r2.l <= r1.l && r2.r >= r1.r) {
		if (r2.t <= r1.t && r2.b > r1.t) r1.t = r2.b;
		else if (r2.b >= r1.b && r2.t < r1.b) r1.b = r2.t;
	}
	r_put(d, &r1);
	return !R_EMPTY(&r1);
}

static u32
u_EqualRect(a)
	u32 *a;
{
	u32 x = LIN(a[0]), y = LIN(a[1]);

	return x && y && memcmp(M + x, M + y, 8) == 0;
}

static u32
u_AdjustWindowRectEx(a)
	u32 *a;
{
	u32 p = LIN(a[0]);
	struct wnd tw;
	struct rect r, c;

	if (!p)
		return 0;
	memset(&tw, 0, sizeof tw);
	tw.style = a[1] & ~WS_VSCROLL & ~WS_HSCROLL;
	tw.exstyle = a[3];
	tw.id = a[2] ? 1 : 0;
	r_set(&r, 0, 0, 1000, 1000);
	wnd_calcclient(&tw, &r, &c);
	if (a[2] && !(tw.style & WS_CHILD))
		c.t += sys_metric(SM_CYMENU) - (tw.id && 0);
	r_get(&r, p);
	r.l -= c.l;
	r.t -= c.t;
	r.r += 1000 - c.r;
	r.b += 1000 - c.b;
	r_put(p, &r);
	return 1;
}

static u32
u_AdjustWindowRect(a)
	u32 *a;
{
	u32 b[4];

	b[0] = a[0];
	b[1] = a[1];
	b[2] = a[2];
	b[3] = 0;
	return u_AdjustWindowRectEx(b);
}

/* ---- strings ---- */

static u32
u_lstrcmp(a)
	u32 *a;
{
	return (u32)(s32)strcmp(STR(a[0]), STR(a[1]));
}

static u32
u_lstrcmpi(a)
	u32 *a;
{
	int r = w16_stricmp(STR(a[0]), STR(a[1]));

	return r < 0 ? 0xffff : r > 0;
}

static int
upper(c)
	int c;
{
	return (c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7) ? c - 32 : c;
}

static int
lower(c)
	int c;
{
	return (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7) ? c + 32 : c;
}

static u32
u_AnsiUpper(a)
	u32 *a;
{
	u8 *s;

	if (FPSEL(a[0]) == 0)
		return upper(a[0] & 0xff);
	for (s = (u8 *)gptr(a[0]); s && *s; s++)
		*s = upper(*s);
	return a[0];
}

static u32
u_AnsiLower(a)
	u32 *a;
{
	u8 *s;

	if (FPSEL(a[0]) == 0)
		return lower(a[0] & 0xff);
	for (s = (u8 *)gptr(a[0]); s && *s; s++)
		*s = lower(*s);
	return a[0];
}

static u32
u_AnsiUpperBuff(a)
	u32 *a;
{
	u8 *s = (u8 *)gptr(a[0]);
	int i, n = a[1] ? a[1] : 65536;

	for (i = 0; s && i < n; i++)
		s[i] = upper(s[i]);
	return n;
}

static u32
u_AnsiLowerBuff(a)
	u32 *a;
{
	u8 *s = (u8 *)gptr(a[0]);
	int i, n = a[1] ? a[1] : 65536;

	for (i = 0; s && i < n; i++)
		s[i] = lower(s[i]);
	return n;
}

static u32
u_AnsiNext(a)
	u32 *a;
{
	char *s = gptr(a[0]);

	return s && *s ? a[0] + 1 : a[0];
}

static u32
u_AnsiPrev(a)
	u32 *a;
{
	return FPOFF(a[1]) > FPOFF(a[0]) ? a[1] - 1 : a[0];
}

static u32 u_IsCharAlpha(a) u32 *a; { int c = a[0] & 0xff; return upper(c) != lower(c); }
static u32 u_IsCharAlphaNumeric(a) u32 *a; { int c = a[0] & 0xff; return upper(c) != lower(c) || (c >= '0' && c <= '9'); }
static u32 u_IsCharUpper(a) u32 *a; { int c = a[0] & 0xff; return upper(c) == c && lower(c) != c; }
static u32 u_IsCharLower(a) u32 *a; { int c = a[0] & 0xff; return lower(c) == c && upper(c) != c; }

/* wsprintf: %[-][#][0][width][.prec][l]{d i u x X c s %}, %s far */
static int
fmt(out, max, f, args)
	char *out, *f;
	int max;
	u32 args;		/* linear address of the arguments */
{
	char *o = out, num[40], *s;
	int left, zero, alt, width, prec, lng, n, i, neg;
	u32 v;

	while (*f && o - out < max - 1) {
		if (*f != '%') {
			*o++ = *f++;
			continue;
		}
		f++;
		left = zero = alt = 0;
		width = 0;
		prec = -1;
		lng = 0;
		for (;; f++) {
			if (*f == '-') left = 1;
			else if (*f == '0') zero = 1;
			else if (*f == '#') alt = 1;
			else break;
		}
		while (*f >= '0' && *f <= '9')
			width = width * 10 + *f++ - '0';
		if (*f == '.') {
			f++;
			prec = 0;
			while (*f >= '0' && *f <= '9')
				prec = prec * 10 + *f++ - '0';
		}
		if (*f == 'l' || *f == 'L') {
			lng = 1;
			f++;
		}
		neg = 0;
		switch (*f) {
		case 'd': case 'i':
			if (lng) { v = GL(args); args += 4; if ((s32)v < 0) { neg = 1; v = -(s32)v; } }
			else { v = GW(args); args += 2; if ((short)v < 0) { neg = 1; v = -(s32)(short)v; } }
			sprintf(num, "%s%u", neg ? "-" : "", v);
			s = num;
			break;
		case 'u':
			if (lng) { v = GL(args); args += 4; } else { v = GW(args); args += 2; }
			sprintf(num, "%u", v);
			s = num;
			break;
		case 'x': case 'X':
			if (lng) { v = GL(args); args += 4; } else { v = GW(args); args += 2; }
			sprintf(num, *f == 'x' ? "%s%x" : "%s%X", alt && v ? (*f == 'x' ? "0x" : "0X") : "", v);
			s = num;
			break;
		case 'c':
			num[0] = GW(args);
			num[1] = 0;
			args += 2;
			s = num;
			break;
		case 's':
			s = gptr(GL(args));
			args += 4;
			if (!s)
				s = "(null)";
			break;
		case '%':
			s = "%";
			break;
		default:
			*o++ = *f;
			if (*f)
				f++;
			continue;
		}
		f++;
		n = strlen(s);
		if (prec >= 0 && (f[-1] == 's') && n > prec)
			n = prec;
		if (!left)
			for (i = n; i < width && o - out < max - 1; i++)
				*o++ = zero && f[-1] != 's' ? '0' : ' ';
		if (zero && !left && neg && width > n) {
			/* the sign before the zeros */
			char *z = o - (width - n);

			if (z >= out) {
				*z = '-';
				s++;
				*o++ = '0';
				n--;
			}
		}
		for (i = 0; i < n && o - out < max - 1; i++)
			*o++ = s[i];
		if (left)
			for (i = n; i < width && o - out < max - 1; i++)
				*o++ = ' ';
	}
	*o = 0;
	return o - out;
}

static u32
u_wsprintf(a)
	u32 *a;
{
	char *out = gptr(a[0]), *f = gptr(a[1]);
	u32 args = lin(FPSEL(api_varargs), FPOFF(api_varargs));

	if (!out || !f)
		return 0;
	return fmt(out, 1024, f, args);
}

static u32
u_wvsprintf(a)
	u32 *a;
{
	char *out = gptr(a[0]), *f = gptr(a[1]);
	u32 args = LIN(a[2]);

	if (!out || !f)
		return 0;
	return fmt(out, 1024, f, args);
}

/* ---- the clipboard ---- */

#define	NCLIP	16
static struct {
	u16 fmt, h;
} clip[NCLIP];
static int nclip;
static u16 clipowner, clipopen;

static u32 u_OpenClipboard(a) u32 *a; { if (clipopen) return 0; clipopen = a[0] ? a[0] : 1; return 1; }
static u32 u_CloseClipboard(a) u32 *a; { clipopen = 0; return 1; }

static u32
u_EmptyClipboard(a)
	u32 *a;
{
	int i;

	for (i = 0; i < nclip; i++)
		if (clip[i].h && !gobj(clip[i].h, 0))
			g_free(clip[i].h);
		else if (clip[i].h)
			gobj_delete(clip[i].h);
	nclip = 0;
	clipowner = clipopen;
	return 1;
}

static u32
u_SetClipboardData(a)
	u32 *a;
{
	int i;

	for (i = 0; i < nclip; i++)
		if (clip[i].fmt == LO16(a[0]))
			break;
	if (i == nclip) {
		if (nclip >= NCLIP)
			return 0;
		nclip++;
	}
	clip[i].fmt = a[0];
	clip[i].h = a[1];
	return a[1];
}

static u32
u_GetClipboardData(a)
	u32 *a;
{
	int i;

	for (i = 0; i < nclip; i++)
		if (clip[i].fmt == LO16(a[0]))
			return clip[i].h;
	return 0;
}

static u32
u_IsClipboardFormatAvailable(a)
	u32 *a;
{
	int i;

	for (i = 0; i < nclip; i++)
		if (clip[i].fmt == LO16(a[0]))
			return 1;
	return 0;
}

static u32 u_CountClipboardFormats(a) u32 *a; { return nclip; }

static u32
u_EnumClipboardFormats(a)
	u32 *a;
{
	int i;

	if (!LO16(a[0]))
		return nclip ? clip[0].fmt : 0;
	for (i = 0; i + 1 < nclip; i++)
		if (clip[i].fmt == LO16(a[0]))
			return clip[i + 1].fmt;
	return 0;
}

static u32 u_RegisterClipboardFormat(a) u32 *a; { return atom_add(a[0]); }
static u32 u_GetClipboardOwner(a) u32 *a; { return clipowner; }
static u32 u_GetOpenClipboardWindow(a) u32 *a; { return clipopen; }

/* ---- the rest ---- */

static u32 u_InitApp(a) u32 *a; { return 1; }
static u32 u_zero(a) u32 *a; { return 0; }
static u32 u_one(a) u32 *a; { return 1; }

static u32
u_MessageBox(a)
	u32 *a;
{
	return user_messagebox(a[0], STR(a[1]), gptr(a[2]) ? STR(a[2]) : "Error", a[3]);
}

/*
 * ExitWindows: every top-level window is asked (WM_QUERYENDSESSION);
 * if none refuses, each hears WM_ENDSESSION and the session ends, all
 * its tasks with it.  A refusal: FALSE.
 */
static u32
u_ExitWindows(a)
	u32 *a;
{
	extern int task_endsession;
	struct wnd *w;
	u16 hs[256];
	int i, k = 0;

	for (w = desktop->child; w && k < 256; w = w->next)
		if (w->task)
			hs[k++] = w->h;
	for (i = 0; i < k; i++)
		if ((w = wnd_get(hs[i])) != 0 && !(wnd_send(w, WM_QUERYENDSESSION, 0, 0) & 0xffff))
			return 0;
	for (i = 0; i < k; i++)
		if ((w = wnd_get(hs[i])) != 0)
			wnd_send(w, WM_ENDSESSION, 1, 0);
	task_endsession = 1;
	w16_exit(0);
	return 0;
}

static u32 u_GetFreeSystemResources(a) u32 *a; { return 80; }

static u32
u_SystemParametersInfo(a)
	u32 *a;
{
	static int hspace = 75, vspace = 72, wrap = 1, grid = 0;
	u32 p = LIN(a[2]);
	int v = (short)a[1];

	/* Windows 3.1's settings, by number */
	switch (a[0]) {
	case 1:		/* SPI_GETBEEP */
	case 25:	/* SPI_GETICONTITLEWRAP */
		if (p) PW(p, a[0] == 1 ? 1 : wrap);
		return 1;
	case 3:		/* SPI_GETMOUSE: thresholds and acceleration */
		if (p) { PW(p, 6); PW(p + 2, 10); PW(p + 4, 1); }
		return 1;
	case 5:		/* SPI_GETBORDER */
		if (p) PW(p, 3);
		return 1;
	case 10:	/* SPI_GETKEYBOARDSPEED */
		if (p) PW(p, 31);
		return 1;
	case 13:	/* SPI_ICONHORIZONTALSPACING: get with a pointer, else set */
	case 24:	/* SPI_ICONVERTICALSPACING */
		if (p)
			PW(p, a[0] == 13 ? hspace : vspace);
		else if (v >= 32) {
			if (a[0] == 13)
				hspace = v;
			else
				vspace = v;
		}
		return 1;
	case 14:	/* SPI_GETSCREENSAVETIMEOUT */
	case 16:	/* SPI_GETSCREENSAVEACTIVE */
	case 27:	/* SPI_GETMENUDROPALIGNMENT */
		if (p) PW(p, 0);
		return 1;
	case 18:	/* SPI_GETGRIDGRANULARITY */
		if (p) PW(p, grid);
		return 1;
	case 19:	/* SPI_SETGRIDGRANULARITY */
		grid = v;
		return 1;
	case 22:	/* SPI_GETKEYBOARDDELAY */
		if (p) PW(p, 2);
		return 1;
	case 26:	/* SPI_SETICONTITLEWRAP */
		wrap = v != 0;
		return 1;
	case 31:	/* SPI_GETICONTITLELOGFONT: MS Sans Serif 8 point */
		if (p) {
			memset(M + p, 0, 50);
			PW(p, -11);
			PW(p + 8, 400);
			strcpy((char *)M + p + 18, "MS Sans Serif");
		}
		return 1;
	case 35:	/* SPI_GETFASTTASKSWITCH */
		if (p) PW(p, 1);
		return 1;
	case 2: case 4: case 6: case 11: case 15: case 17: case 20: case 21: case 23:
	case 28: case 29: case 30: case 32: case 33: case 34: case 36:
		return 1;	/* the settings: taken, not kept */
	}
	return 0;
}

/*
 * WinHelp(hwnd, file, command, data): to Windows' help viewer, WINHELP.EXE
 * from the user's Windows, started when not running; the request goes in
 * a WM_WINHELP message as Windows sends it (a shared block: size,
 * command, data, reserved, the file's offset, the data's offset).
 */
static u32
u_WinHelp(a)
	u32 *a;
{
	extern u32 kernel_winexec();
	struct wnd *w = 0;
	u32 sc, cls, msg, h, p, b[1];
	char *file = a[1] ? gptr(a[1]) : 0;
	int cmd = a[2] & 0xffff, tries, n, len;

	sc = ustr("MS_WINHELP");
	cls = cls_find(sc, 0) ? sc : 0;
	for (tries = 0; tries < 2 && !w; tries++) {
		w = 0;
		if (cls)
			for (w = desktop->child; w; w = w->next)
				if (w->cls == cls_find(sc, 0))
					break;
		if (!w) {
			if (cmd == 2 || tries)		/* HELP_QUIT with no viewer: done */
				break;
			if (kernel_winexec("WINHELP.EXE -x", 1) < 32)
				break;
			cls = cls_find(sc, 0) ? sc : 0;
		}
	}
	b[0] = ustr("WM_WINHELP");
	msg = u_RegisterWindowMessage(b);
	ufree(sc);
	if (!w)
		return cmd == 2;
	len = file ? strlen(file) + 1 : 0;
	n = 16 + len + ((cmd == 0x101 || cmd == 0x105) && a[3] ? gstrlen(a[3]) + 1 : 0);
	h = g_alloc(GMEM_MOVEABLE | GMEM_DDESHARE | GMEM_ZEROINIT, n, 0);
	p = sel_base(h);
	PW(p, n);
	PW(p + 2, cmd);
	PL(p + 4, (cmd == 0x101 || cmd == 0x105) ? 0 : a[3]);	/* HELP_KEY, HELP_PARTIALKEY carry a string */
	PW(p + 12, len ? 16 : 0);
	if (len)
		strcpy((char *)M + p + 16, file);
	if (n > 16 + len) {
		PW(p + 14, 16 + len);
		strcpy((char *)M + p + 16 + len, gptr(a[3]));
	}
	wnd_send(w, msg, a[0] & 0xffff, FP(0, h));
	return 1;
}

static u32 u_ArrangeIconicWindows(a) u32 *a; { return 0; }
static u32 u_WNetGetCaps(a) u32 *a; { return 0; }	/* no network */
static u32 u_WNetNotSupported(a) u32 *a; { return 1; }	/* WN_NOT_SUPPORTED */

/* WINDOWPLACEMENT: length, flags, showCmd, ptMinPosition, ptMaxPosition, rcNormalPosition */
static u32
u_GetWindowPlacement(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	struct rect r;
	int px = 0, py = 0;
	W(a[0]);

	if (!p)
		return 0;
	r = (w->style & (WS_MINIMIZE | WS_MAXIMIZE)) ? w->normal : w->wr;
	if ((w->style & WS_CHILD) && w->parent) {
		px = w->parent->cr.l;
		py = w->parent->cr.t;
	}
	PW(p, 22);
	PW(p + 2, 0);
	PW(p + 4, (w->style & WS_MINIMIZE) ? 2 : (w->style & WS_MAXIMIZE) ? 3 : 1);
	PW(p + 6, (w->style & WS_MINIMIZE) ? w->wr.l - px : -1);
	PW(p + 8, (w->style & WS_MINIMIZE) ? w->wr.t - py : -1);
	PW(p + 10, -1);
	PW(p + 12, -1);
	PW(p + 14, r.l - px);
	PW(p + 16, r.t - py);
	PW(p + 18, r.r - px);
	PW(p + 20, r.b - py);
	return 1;
}

static u32
u_SetWindowPlacement(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	int px = 0, py = 0, cmd;
	struct rect r;
	W(a[0]);

	if (!p)
		return 0;
	if ((w->style & WS_CHILD) && w->parent) {
		px = w->parent->cr.l;
		py = w->parent->cr.t;
	}
	r_set(&r, (short)GW(p + 14), (short)GW(p + 16), (short)GW(p + 18), (short)GW(p + 20));
	cmd = GW(p + 4);
	if (w->style & (WS_MINIMIZE | WS_MAXIMIZE)) {
		r_set(&w->normal, r.l + px, r.t + py, r.r + px, r.b + py);
	} else
		wnd_setpos(w, (struct wnd *)0, r.l, r.t, r.r - r.l, r.b - r.t, SWP_NOZORDER | SWP_NOACTIVATE);
	if (cmd != SW_SHOWNORMAL || (w->style & (WS_MINIMIZE | WS_MAXIMIZE)))
		wnd_show(w, cmd);
	return 1;
}
static u32 u_ShowOwnedPopups(a) u32 *a; { return 0; }
static u32 u_GetDialogBaseUnits(a) u32 *a; { extern u32 dlg_baseunits(); return dlg_baseunits(); }

struct impl u_impl[] = {
	{ "USER", "RegisterClass", u_RegisterClass },
	{ "USER", "UnregisterClass", u_UnregisterClass },
	{ "USER", "GetClassInfo", u_GetClassInfo },
	{ "USER", "CreateWindow", u_CreateWindow },
	{ "USER", "CreateWindowEx", u_CreateWindowEx },
	{ "USER", "DestroyWindow", u_DestroyWindow },
	{ "USER", "ShowWindow", u_ShowWindow },
	{ "USER", "UpdateWindow", u_UpdateWindow },
	{ "USER", "IsWindow", u_IsWindow },
	{ "USER", "IsWindowVisible", u_IsWindowVisible },
	{ "USER", "IsWindowEnabled", u_IsWindowEnabled },
	{ "USER", "IsIconic", u_IsIconic },
	{ "USER", "IsZoomed", u_IsZoomed },
	{ "USER", "IsChild", u_IsChild },
	{ "USER", "EnableWindow", u_EnableWindow },
	{ "USER", "GetWindowRect", u_GetWindowRect },
	{ "USER", "GetClientRect", u_GetClientRect },
	{ "USER", "ClientToScreen", u_ClientToScreen },
	{ "USER", "ScreenToClient", u_ScreenToClient },
	{ "USER", "MapWindowPoints", u_MapWindowPoints },
	{ "USER", "WindowFromPoint", u_WindowFromPoint },
	{ "USER", "ChildWindowFromPoint", u_ChildWindowFromPoint },
	{ "USER", "MoveWindow", u_MoveWindow },
	{ "USER", "SetWindowPos", u_SetWindowPos },
	{ "USER", "BringWindowToTop", u_BringWindowToTop },
	{ "USER", "CloseWindow", u_CloseWindow },
	{ "USER", "OpenIcon", u_OpenIcon },
	{ "USER", "GetParent", u_GetParent },
	{ "USER", "SetParent", u_SetParent },
	{ "USER", "GetWindow", u_GetWindow },
	{ "USER", "GetTopWindow", u_GetTopWindow },
	{ "USER", "GetNextWindow", u_GetNextWindow },
	{ "USER", "GetDesktopWindow", u_GetDesktopWindow },
	{ "USER", "GetDesktopHwnd", u_GetDesktopWindow },
	{ "USER", "GetLastActivePopup", u_GetLastActivePopup },
	{ "USER", "FindWindow", u_FindWindow },
	{ "USER", "EnumWindows", u_EnumWindows },
	{ "USER", "EnumChildWindows", u_EnumChildWindows },
	{ "USER", "EnumTaskWindows", u_EnumTaskWindows },
	{ "USER", "GetWindowTask", u_GetWindowTask },
	{ "USER", "GetWindowText", u_GetWindowText },
	{ "USER", "SetWindowText", u_SetWindowText },
	{ "USER", "GetWindowTextLength", u_GetWindowTextLength },
	{ "USER", "GetClassName", u_GetClassName },
	{ "USER", "GetWindowWord", u_GetWindowWord },
	{ "USER", "SetWindowWord", u_SetWindowWord },
	{ "USER", "GetWindowLong", u_GetWindowLong },
	{ "USER", "SetWindowLong", u_SetWindowLong },
	{ "USER", "GetClassWord", u_GetClassWord },
	{ "USER", "SetClassWord", u_SetClassWord },
	{ "USER", "GetClassLong", u_GetClassLong },
	{ "USER", "SetClassLong", u_SetClassLong },
	{ "USER", "SetProp", u_SetProp },
	{ "USER", "GetProp", u_GetProp },
	{ "USER", "RemoveProp", u_RemoveProp },
	{ "USER", "EnumProps", u_EnumProps },
	{ "USER", "SetFocus", u_SetFocus },
	{ "USER", "GetFocus", u_GetFocus },
	{ "USER", "SetActiveWindow", u_SetActiveWindow },
	{ "USER", "GetActiveWindow", u_GetActiveWindow },
	{ "USER", "SetCapture", u_SetCapture },
	{ "USER", "ReleaseCapture", u_ReleaseCapture },
	{ "USER", "GetCapture", u_GetCapture },
	{ "USER", "SetSysModalWindow", u_SetSysModalWindow },
	{ "USER", "BeginPaint", u_BeginPaint },
	{ "USER", "EndPaint", u_EndPaint },
	{ "USER", "GetDC", u_GetDC },
	{ "USER", "GetDCEx", u_GetDCEx },
	{ "USER", "GetWindowDC", u_GetWindowDC },
	{ "USER", "ReleaseDC", u_ReleaseDC },
	{ "USER", "WindowFromDC", u_WindowFromDC },
	{ "USER", "InvalidateRect", u_InvalidateRect },
	{ "USER", "InvalidateRgn", u_InvalidateRgn },
	{ "USER", "ValidateRect", u_ValidateRect },
	{ "USER", "ValidateRgn", u_ValidateRgn },
	{ "USER", "GetUpdateRect", u_GetUpdateRect },
	{ "USER", "GetUpdateRgn", u_GetUpdateRgn },
	{ "USER", "RedrawWindow", u_RedrawWindow },
	{ "USER", "ScrollWindow", u_ScrollWindow },
	{ "USER", "ScrollDC", u_ScrollDC },
	{ "USER", "LockWindowUpdate", u_LockWindowUpdate },
	{ "USER", "FillRect", u_FillRect },
	{ "USER", "FrameRect", u_FrameRect },
	{ "USER", "InvertRect", u_InvertRect },
	{ "USER", "DrawFocusRect", u_DrawFocusRect },
	{ "USER", "DrawText", u_DrawText },
	{ "USER", "TabbedTextOut", u_TabbedTextOut },
	{ "USER", "GetTabbedTextExtent", u_GetTabbedTextExtent },
	{ "USER", "GrayString", u_GrayString },
	{ "USER", "DrawIcon", u_DrawIcon },
	{ "USER", "GetMessage", u_GetMessage },
	{ "USER", "PeekMessage", u_PeekMessage },
	{ "USER", "WaitMessage", u_WaitMessage },
	{ "USER", "TranslateMessage", u_TranslateMessage },
	{ "USER", "DispatchMessage", u_DispatchMessage },
	{ "USER", "SendMessage", u_SendMessage },
	{ "USER", "PostMessage", u_PostMessage },
	{ "USER", "PostAppMessage", u_PostAppMessage },
	{ "USER", "PostQuitMessage", u_PostQuitMessage },
	{ "USER", "ReplyMessage", u_ReplyMessage },
	{ "USER", "InSendMessage", u_InSendMessage },
	{ "USER", "GetMessagePos", u_GetMessagePos },
	{ "USER", "GetMessageTime", u_GetMessageTime },
	{ "USER", "GetMessageExtraInfo", u_GetMessageExtraInfo },
	{ "USER", "CallWindowProc", u_CallWindowProc },
	{ "USER", "DefWindowProc", u_DefWindowProc },
	{ "USER", "RegisterWindowMessage", u_RegisterWindowMessage },
	{ "USER", "GetInputState", u_GetInputState },
	{ "USER", "GetQueueStatus", u_GetQueueStatus },
	{ "USER", "SetWindowsHook", u_SetWindowsHook },
	{ "USER", "SetWindowsHookEx", u_SetWindowsHookEx },
	{ "USER", "UnhookWindowsHook", u_UnhookWindowsHook },
	{ "USER", "UnhookWindowsHookEx", u_UnhookWindowsHook },
	{ "USER", "DefHookProc", u_DefHookProc },
	{ "USER", "CallNextHookEx", u_DefHookProc },
	{ "USER", "CallMsgFilter", u_CallMsgFilter },
	{ "USER", "SetTimer", u_SetTimer },
	{ "USER", "KillTimer", u_KillTimer },
	{ "USER", "SetSystemTimer", u_SetSystemTimer },
	{ "USER", "KillSystemTimer", u_KillTimer },
	{ "USER", "GetTickCount", u_GetTickCount },
	{ "USER", "GetCurrentTime", u_GetTickCount },
	{ "USER", "GetTimerResolution", u_GetTimerResolution },
	{ "USER", "CreateCaret", u_CreateCaret },
	{ "USER", "DestroyCaret", u_DestroyCaret },
	{ "USER", "SetCaretPos", u_SetCaretPos },
	{ "USER", "GetCaretPos", u_GetCaretPos },
	{ "USER", "HideCaret", u_HideCaret },
	{ "USER", "ShowCaret", u_ShowCaret },
	{ "USER", "SetCaretBlinkTime", u_zero },
	{ "USER", "GetCaretBlinkTime", u_GetCaretBlinkTime },
	{ "USER", "SetCursor", u_SetCursor },
	{ "USER", "GetCursor", u_GetCursor },
	{ "USER", "ShowCursor", u_ShowCursor },
	{ "USER", "GetCursorPos", u_GetCursorPos },
	{ "USER", "SetCursorPos", u_SetCursorPos },
	{ "USER", "ClipCursor", u_ClipCursor },
	{ "USER", "GetKeyState", u_GetKeyState },
	{ "USER", "GetAsyncKeyState", u_GetAsyncKeyState },
	{ "USER", "GetKeyboardState", u_GetKeyboardState },
	{ "USER", "SetKeyboardState", u_SetKeyboardState },
	{ "USER", "MessageBeep", u_MessageBeep },
	{ "USER", "GetDoubleClickTime", u_GetDoubleClickTime },
	{ "USER", "SetDoubleClickTime", u_zero },
	{ "USER", "SwapMouseButton", u_zero },
	{ "USER", "GetSystemMetrics", u_GetSystemMetrics },
	{ "USER", "GetSysColor", u_GetSysColor },
	{ "USER", "GetSysColorBrush", u_GetSysColorBrush },
	{ "USER", "SetSysColors", u_zero },
	{ "USER", "FlashWindow", u_FlashWindow },
	{ "USER", "LoadCursor", u_LoadCursor },
	{ "USER", "LoadIcon", u_LoadIcon },
	{ "USER", "DestroyIcon", u_DestroyIcon },
	{ "USER", "DestroyCursor", u_DestroyIcon },
	{ "USER", "CreateIcon", u_CreateIcon },
	{ "USER", "CreateCursor", u_CreateCursor },
	{ "USER", "LoadBitmap", u_LoadBitmap },
	{ "USER", "LoadString", u_LoadString },
	{ "USER", "LoadAccelerators", u_LoadAccelerators },
	{ "USER", "TranslateAccelerator", u_TranslateAccelerator },
	{ "USER", "SetRect", u_SetRect },
	{ "USER", "SetRectEmpty", u_SetRectEmpty },
	{ "USER", "CopyRect", u_CopyRect },
	{ "USER", "IsRectEmpty", u_IsRectEmpty },
	{ "USER", "PtInRect", u_PtInRect },
	{ "USER", "OffsetRect", u_OffsetRect },
	{ "USER", "InflateRect", u_InflateRect },
	{ "USER", "IntersectRect", u_IntersectRect },
	{ "USER", "UnionRect", u_UnionRect },
	{ "USER", "SubtractRect", u_SubtractRect },
	{ "USER", "EqualRect", u_EqualRect },
	{ "USER", "AdjustWindowRect", u_AdjustWindowRect },
	{ "USER", "AdjustWindowRectEx", u_AdjustWindowRectEx },
	{ "USER", "lstrcmp", u_lstrcmp },
	{ "USER", "lstrcmpi", u_lstrcmpi },
	{ "USER", "AnsiUpper", u_AnsiUpper },
	{ "USER", "AnsiLower", u_AnsiLower },
	{ "USER", "AnsiUpperBuff", u_AnsiUpperBuff },
	{ "USER", "AnsiLowerBuff", u_AnsiLowerBuff },
	{ "USER", "AnsiNext", u_AnsiNext },
	{ "USER", "AnsiPrev", u_AnsiPrev },
	{ "USER", "IsCharAlpha", u_IsCharAlpha },
	{ "USER", "IsCharAlphaNumeric", u_IsCharAlphaNumeric },
	{ "USER", "IsCharUpper", u_IsCharUpper },
	{ "USER", "IsCharLower", u_IsCharLower },
	{ "USER", "_wsprintf", u_wsprintf },
	{ "USER", "wvsprintf", u_wvsprintf },
	{ "USER", "OpenClipboard", u_OpenClipboard },
	{ "USER", "CloseClipboard", u_CloseClipboard },
	{ "USER", "EmptyClipboard", u_EmptyClipboard },
	{ "USER", "SetClipboardData", u_SetClipboardData },
	{ "USER", "GetClipboardData", u_GetClipboardData },
	{ "USER", "IsClipboardFormatAvailable", u_IsClipboardFormatAvailable },
	{ "USER", "CountClipboardFormats", u_CountClipboardFormats },
	{ "USER", "EnumClipboardFormats", u_EnumClipboardFormats },
	{ "USER", "RegisterClipboardFormat", u_RegisterClipboardFormat },
	{ "USER", "GetClipboardOwner", u_GetClipboardOwner },
	{ "USER", "GetOpenClipboardWindow", u_GetOpenClipboardWindow },
	{ "USER", "SetClipboardViewer", u_zero },
	{ "USER", "ChangeClipboardChain", u_zero },
	{ "USER", "InitApp", u_InitApp },
	{ "USER", "MessageBox", u_MessageBox },
	{ "USER", "ExitWindows", u_ExitWindows },
	{ "USER", "GetFreeSystemResources", u_GetFreeSystemResources },
	{ "USER", "SystemParametersInfo", u_SystemParametersInfo },
	{ "USER", "WinHelp", u_WinHelp },
	{ "USER", "WNetGetCaps", u_WNetGetCaps },
	{ "USER", "WNetGetConnection", u_WNetNotSupported },
	{ "USER", "WNetAddConnection", u_WNetNotSupported },
	{ "USER", "WNetCancelConnection", u_WNetNotSupported },
	{ "USER", "WNetGetUser", u_WNetNotSupported },
	{ "USER", "SetMessageQueue", u_one },	/* the queue is as long as it needs */
	{ "USER", "GetWindowPlacement", u_GetWindowPlacement },
	{ "USER", "SetWindowPlacement", u_SetWindowPlacement },
	{ "USER", "ArrangeIconicWindows", u_ArrangeIconicWindows },
	{ "USER", "ShowOwnedPopups", u_ShowOwnedPopups },
	{ "USER", "GetDialogBaseUnits", u_GetDialogBaseUnits },
	{ "USER", "SelectPalette", u_zero },
	{ "USER", "RealizePalette", u_zero },
	{ "USER", "UserYield", u_zero },
	{ "USER", "EnableHardwareInput", u_one },
	{ "USER", "LockInput", u_one },
	{ 0 }
};

/* the edit control's clipboard: text in and out */
void
clip_puttext(s)
	char *s;
{
	u16 h = g_alloc(GMEM_MOVEABLE | GMEM_DDESHARE, strlen(s) + 1, 0);
	u32 a[2];

	if (!h)
		return;
	strcpy((char *)M + sel_base(h), s);
	u_EmptyClipboard(a);
	a[0] = 1;		/* CF_TEXT */
	a[1] = h;
	u_SetClipboardData(a);
}

char *
clip_text()
{
	u32 a[1];
	u16 h;

	a[0] = 1;
	h = u_GetClipboardData(a);
	return h && g_block(h) ? (char *)M + sel_base(h) : (char *)0;
}
