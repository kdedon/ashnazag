/*
 * user.c -- our USER: windows, classes, the message queue, painting,
 * input, focus and activation, timers and DCs on windows.
 *
 * Windows draw straight on the screen surface through their visible
 * region (their own rectangle less what covers it; children and
 * siblings above are always clipped).  Nothing is kept behind a
 * window: moving or hiding one invalidates what it uncovers, and the
 * windows there repaint, as in Windows 3.1.
 *
 * One queue for the one task.  Input from the device becomes messages
 * in the queue; WM_PAINT and WM_TIMER are made when the queue is empty.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "scr.h"

static void rawfill(), rawput();
static int timer_next();
extern void mm_tick(), ws_poll();
extern int mm_next(), ws_next();

#define	NWND	2048
#define	WBASE	0x2000

struct wnd *desktop;
struct wnd *wnd_focus, *wnd_active, *wnd_capture;
int vis_epoch = 1;
u16 cur_arrow;

static struct wnd *wtab[NWND];
static struct cls *classes;
u8 keystate[256];		/* as of the message in hand (GetKeyState) */
u8 asyncstate[256];		/* as of now (GetAsyncKeyState) */

#define	STR(p)		(gptr(p) ? gptr(p) : "")
#define	ISINT(p)	(FPSEL(p) == 0)

/* ---- guest scratch: structures we hand to window procedures ---- */

static u16 usel;
static u32 utop;

/* each task has its own scratch segment: allocations nest per task */
void
user_ctxsave(p)
	u32 *p;
{
	p[0] = usel;
	p[1] = utop;
}

void
user_ctxload(p)
	u32 *p;
{
	if (!p[0]) {
		/* a new task's */
		p[0] = g_alloc(GMEM_ZEROINIT, 0x10000, 0);
		p[1] = 16;
	}
	usel = p[0];
	utop = p[1];
}

u32
ualloc(n)
	u32 n;
{
	u32 p;

	n = (n + 3) & ~3;
	if (utop + n + 4 > 0xfff0)
		w16_fatal("USER scratch overflow");
	p = utop;
	PW(sel_base(usel) + p, n);
	utop += n + 4;
	memset(M + sel_base(usel) + p + 4, 0, n);
	return FP(usel, p + 4);
}

void
ufree(p)
	u32 p;
{
	u32 off = FPOFF(p) - 4;

	if (off + GW(sel_base(usel) + off) + 4 == utop)
		utop = off;
	else
		utop = off;	/* out of order: drop everything above */
}

u32
ulin(p)
	u32 p;
{
	return sel_base(usel) + FPOFF(p);
}

/* a host string as a far pointer in the scratch */
u32
ustr(s)
	char *s;
{
	u32 p = ualloc(strlen(s) + 1);

	strcpy((char *)M + ulin(p), s);
	return p;
}

/* ---- system colours and metrics ---- */

static COLORREF syscolor[NSYSCOLOR] = {
	RGB(192, 192, 192), RGB(192, 192, 192), RGB(0, 0, 128), RGB(255, 255, 255),
	RGB(255, 255, 255), RGB(255, 255, 255), RGB(0, 0, 0), RGB(0, 0, 0),
	RGB(0, 0, 0), RGB(255, 255, 255), RGB(192, 192, 192), RGB(192, 192, 192),
	RGB(255, 255, 255), RGB(0, 0, 128), RGB(255, 255, 255), RGB(192, 192, 192),
	RGB(128, 128, 128), RGB(128, 128, 128), RGB(0, 0, 0), RGB(0, 0, 0),
	RGB(255, 255, 255)
};
static u16 sysbrush[NSYSCOLOR];
static char *colornames[NSYSCOLOR] = {
	"Scrollbar", "Background", "ActiveTitle", "InactiveTitle", "Menu", "Window", "WindowFrame",
	"MenuText", "WindowText", "TitleText", "ActiveBorder", "InactiveBorder", "AppWorkspace",
	"Hilight", "HilightText", "ButtonFace", "ButtonShadow", "GrayText", "ButtonText",
	"InactiveTitleText", "ButtonHilight"
};

u32
sys_color(i)
	int i;
{
	return i >= 0 && i < NSYSCOLOR ? syscolor[i] : 0;
}

u16
sys_brush(i)
	int i;
{
	extern u32 g_solidbrush();

	if (i < 0 || i >= NSYSCOLOR)
		return stockobj[WHITE_BRUSH];
	if (!sysbrush[i]) {
		u32 a[1];
		struct impl *im;

		a[0] = syscolor[i];
		for (im = g_impl; im->im_mod; im++)
			if (strcmp(im->im_name, "CreateSolidBrush") == 0)
				sysbrush[i] = (*im->im_fn)(a);
		gobj(sysbrush[i], 0)->stock = 1;
	}
	return sysbrush[i];
}

int
sys_metric(i)
	int i;
{
	switch (i) {
	case SM_CXSCREEN: return screen.w;
	case SM_CYSCREEN: return screen.h;
	case SM_CXVSCROLL: case SM_CXHSCROLL: return 17;
	case SM_CYHSCROLL: case SM_CYVSCROLL: return 17;
	case SM_CYCAPTION: return 20;
	case SM_CXBORDER: case SM_CYBORDER: return 1;
	case SM_CXDLGFRAME: case SM_CYDLGFRAME: return 4;
	case SM_CYVTHUMB: case SM_CXHTHUMB: return 17;
	case SM_CXICON: case SM_CYICON: return 32;
	case SM_CXCURSOR: case SM_CYCURSOR: return 32;
	case SM_CYMENU: return 18;
	case SM_CXFULLSCREEN: return screen.w;
	case SM_CYFULLSCREEN: return screen.h - 20;
	case SM_MOUSEPRESENT: return 1;
	case SM_CXMIN: return 100;
	case SM_CYMIN: return 27;
	case SM_CXSIZE: case SM_CYSIZE: return 18;
	case SM_CXFRAME: case SM_CYFRAME: return 4;
	case SM_CXMINTRACK: return 100;
	case SM_CYMINTRACK: return 27;
	case SM_CXDOUBLECLK: case SM_CYDOUBLECLK: return 4;
	case SM_CXICONSPACING: return 75;
	case SM_CYICONSPACING: return 72;
	}
	return 0;
}

/* colours from WIN.INI [colors], "r g b" */
static void
readcolors()
{
	char buf[64];
	int i, r, g, b;

	for (i = 0; i < NSYSCOLOR; i++)
		if (profile_get((char *)0, "colors", colornames[i], "", buf, sizeof buf) &&
		    sscanf(buf, "%d %d %d", &r, &g, &b) == 3)
			syscolor[i] = RGB(r & 255, g & 255, b & 255);
}

/* ---- windows ---- */

struct wnd *
wnd_get(h)
	u32 h;
{
	int i;

	h &= 0xffff;
	if (h < WBASE || (h - WBASE) & 3)
		return 0;
	i = (h - WBASE) >> 2;
	return i < NWND ? wtab[i] : 0;
}

static struct wnd *
wnd_alloc()
{
	static int hint;
	int i, k;

	for (k = 0; k < NWND; k++) {
		i = (hint + k) % NWND;
		if (!wtab[i]) {
			wtab[i] = (struct wnd *)calloc(1, sizeof(struct wnd));
			wtab[i]->h = WBASE + 4 * i;
			rgn_init(&wtab[i]->upd);
			hint = i + 1;
			return wtab[i];
		}
	}
	return 0;
}

int
wnd_visible(w)
	struct wnd *w;
{
	for (; w && w != desktop; w = w->parent)
		if (!(w->style & WS_VISIBLE))
			return 0;
	return w == desktop;
}

struct wnd *
wnd_toplevel(w)
	struct wnd *w;
{
	while (w && w->parent && w->parent != desktop && (w->style & WS_CHILD))
		w = w->parent;
	return w;
}

static void
unlink_w(w)
	struct wnd *w;
{
	struct wnd **pp;

	if (!w->parent)
		return;
	for (pp = &w->parent->child; *pp; pp = &(*pp)->next)
		if (*pp == w) {
			*pp = w->next;
			break;
		}
	w->next = 0;
}

/* put w after `after' among its siblings (0 the top, (wnd *)1 the bottom) */
static void
link_w(w, after)
	struct wnd *w, *after;
{
	struct wnd **pp;

	if (!after) {
		w->next = w->parent->child;
		w->parent->child = w;
		return;
	}
	for (pp = &w->parent->child; *pp; pp = &(*pp)->next)
		if (*pp == after && after != (struct wnd *)1) {
			w->next = after->next;
			after->next = w;
			return;
		}
	/* the bottom */
	for (pp = &w->parent->child; *pp; pp = &(*pp)->next)
		;
	*pp = w;
	w->next = 0;
}

/* the region of w on the screen nothing covers (client area or whole) */
void
wnd_visrgn(w, g, client)
	struct wnd *w;
	struct rgn *g;
	int client;
{
	struct wnd *p, *s, *c;
	struct rect r;

	if (!wnd_visible(w)) {
		g->n = 0;
		r_set(&g->box, 0, 0, 0, 0);
		return;
	}
	r = client ? w->cr : w->wr;
	for (p = w->parent; p && p != desktop; p = p->parent)
		r_and(&r, &r, &p->cr);
	r_and(&r, &r, &desktop->wr);
	rgn_set(g, &r);
	/*
	 * Siblings above w and above each of its ancestors, where they clip:
	 * always among top-level windows, else with WS_CLIPSIBLINGS.
	 */
	for (c = w; c && c->parent; c = c->parent)
		if (c->parent == desktop || (c->style & WS_CLIPSIBLINGS))
			for (s = c->parent->child; s && s != c; s = s->next)
				if ((s->style & WS_VISIBLE))
					rgn_subrect(g, &s->wr);
	/* children with WS_CLIPCHILDREN (the desktop always) */
	if (client == 1 ? (w->style & WS_CLIPCHILDREN) || w == desktop : client == 2)
		for (s = w->child; s; s = s->next)
			if ((s->style & WS_VISIBLE))
				rgn_subrect(g, &s->wr);
}

/* everything in rect (screen) repaints: every window there, frames too */
static void
expose_in(w, r, erase)
	struct wnd *w;
	struct rect *r;
	int erase;
{
	struct rect m;
	struct wnd *c;

	if (!(w->style & WS_VISIBLE) && w != desktop)
		return;
	if (!r_and(&m, r, &w->wr))
		return;
	if (r_and(&m, &m, &w->cr)) {
		rgn_addrect(&w->upd, &m);
		if (erase)
			w->erase = 1;
	}
	r_and(&m, r, &w->wr);
	if (m.l < w->cr.l || m.t < w->cr.t || m.r > w->cr.r || m.b > w->cr.b)
		w->ncpaint = 1;
	for (c = w->child; c; c = c->next)
		expose_in(c, r, erase);
}

void
expose(r)
	struct rect *r;
{
	expose_in(desktop, r, 1);
}

/* InvalidateRect: r in screen coordinates (0 all the client), the window only */
void
wnd_invalidate(w, r, erase)
	struct wnd *w;
	struct rect *r;
	int erase;
{
	struct rect m;

	if (!wnd_visible(w))
		return;
	if (r) {
		if (!r_and(&m, r, &w->cr))
			return;
	} else
		m = w->cr;
	rgn_addrect(&w->upd, &m);
	if (erase)
		w->erase = 1;
}

/* the client rectangle the frame leaves, from the styles */
void
wnd_calcclient(w, wr, cr)
	struct wnd *w;
	struct rect *wr, *cr;
{
	int b = 0, top = 0;
	u32 st = w->style;

	*cr = *wr;
	if (st & WS_MINIMIZE) {
		r_set(cr, wr->l, wr->t, wr->l, wr->t);
		return;
	}
	if ((st & WS_THICKFRAME))
		b = sys_metric(SM_CXFRAME);
	else if ((st & WS_CAPTION) == WS_DLGFRAME || (w->exstyle & WS_EX_DLGMODALFRAME))
		b = sys_metric(SM_CXDLGFRAME);
	else if (st & WS_BORDER)
		b = 1;
	if ((st & WS_CAPTION) == WS_CAPTION)
		top = sys_metric(SM_CYCAPTION) - 1;
	if ((st & WS_CAPTION) == WS_CAPTION && !(st & WS_THICKFRAME) && !(w->exstyle & WS_EX_DLGMODALFRAME))
		b = 1;
	cr->l += b;
	cr->r -= b;
	cr->t += b + top;
	cr->b -= b;
	if (!(st & WS_CHILD) && w->id && menu_barheight(w))
		cr->t += menu_barheight(w);
	if (st & WS_VSCROLL)
		cr->r -= sys_metric(SM_CXVSCROLL) - 1;
	if (st & WS_HSCROLL)
		cr->b -= sys_metric(SM_CYHSCROLL) - 1;
	if (cr->r < cr->l)
		cr->r = cr->l;
	if (cr->b < cr->t)
		cr->b = cr->t;
}

/* WM_NCCALCSIZE to the window procedure, which may move the client area */
static void
nccalc(w)
	struct wnd *w;
{
	u32 p = ualloc(8), l = ulin(p);
	struct rect r;

	r_put(l, &w->wr);
	wnd_send(w, WM_NCCALCSIZE, 0, p);
	r_get(&r, l);
	ufree(p);
	w->cr = r;
}

static void
offsetall(w, dx, dy)
	struct wnd *w;
	int dx, dy;
{
	struct wnd *c;

	w->wr.l += dx; w->wr.r += dx; w->wr.t += dy; w->wr.b += dy;
	w->cr.l += dx; w->cr.r += dx; w->cr.t += dy; w->cr.b += dy;
	rgn_offset(&w->upd, dx, dy);
	for (c = w->child; c; c = c->next)
		offsetall(c, dx, dy);
}

/* ---- classes ---- */

struct cls *
cls_find(name, hinst)
	u32 name;
	int hinst;
{
	struct cls *c;
	char *s = ISINT(name) ? 0 : STR(name);
	struct module *m = mod_byhandle(hinst);

	for (c = classes; c; c = c->next) {
		if (s ? w16_stricmp(c->name, s) != 0 : c->atom != FPOFF(name))
			continue;
		if (c->global || !hinst || c->hinst == hinst || (m && mod_byhandle(c->hinst) == m))
			return c;
	}
	/* a program's class any module may use */
	for (c = classes; c; c = c->next)
		if (s ? w16_stricmp(c->name, s) == 0 : c->atom == FPOFF(name))
			return c;
	return 0;
}

struct cls *
cls_register(name, style, proc, clsx, wndx, hinst, icon, cursor, bg, menu, global)
	char *name;
	u32 style, proc, menu;
	int clsx, wndx, hinst, icon, cursor, bg, global;
{
	struct cls *c = (struct cls *)calloc(1, sizeof *c);
	extern u32 atom_add();

	strncpy(c->name, name, sizeof c->name - 1);
	{
		u32 t = ustr(name);

		c->atom = atom_add(t);
		ufree(t);
	}
	c->style = style;
	c->proc = proc;
	c->clsextra = clsx;
	c->wndextra = wndx;
	c->hinst = hinst;
	c->icon = icon;
	c->cursor = cursor;
	c->bg = bg;
	c->menuname = menu;
	if (menu && !ISINT(menu))
		strncpy(c->menustr, STR(menu), sizeof c->menustr - 1);
	c->extra = (u8 *)calloc(1, clsx + 4);
	c->global = global;
	c->next = classes;
	classes = c;
	return c;
}

/* ---- calling window procedures ---- */

u32
wnd_call(proc, h, msg, wp, lp)
	u32 proc, h, msg, wp, lp;
{
	apifn fn;
	u32 a[4];

	if (!proc)
		return 0;
	if (thunk_isnative(proc, &fn) && fn) {
		a[0] = h & 0xffff;
		a[1] = msg & 0xffff;
		a[2] = wp & 0xffff;
		a[3] = lp;
		return (*fn)(a);
	}
	cb_begin();
	cb_push16(h);
	cb_push16(msg);
	cb_push16(wp);
	cb_push32(lp);
	return cb_call(proc, 0);
}

u32
wnd_send(w, msg, wp, lp)
	struct wnd *w;
	u32 msg, wp, lp;
{
	if (!w)
		return 0;
	if (w16_debug > 2)
		w16_log("send %04x %04x %04x %08x (%08x)\n", w->h, msg, wp, lp, w->proc);
	return wnd_call(w->proc, w->h, msg, wp, lp);
}

u32
wnd_sendh(h, msg, wp, lp)
	u32 h, msg, wp, lp;
{
	return wnd_send(wnd_get(h), msg, wp, lp);
}

/* ---- the queue ---- */

#define	QSIZE	256

struct qmsg {
	u16	hwnd, msg, wp;
	u32	lp, time;
	short	x, y;
	struct task *task;	/* whose: the window's, or the poster's for none */
};

static struct qmsg q[QSIZE];
static int qhead, qtail;
static int quitting;		/* the session was asked to end (EV_QUIT) */
int user_alttap;		/* Alt went down and nothing else since: its release opens the menu bar */

/* a window's task; marked as having something to do */
static struct task *
owner(h)
	u32 h;
{
	struct wnd *w = h ? wnd_get(h) : 0;

	return w ? w->task : curtask;
}

static void
wake(t)
	struct task *t;
{
	/* a message for a task is an event for it too, as Windows posts one (a WaitEvent ends) */
	if (t) {
		t->t_idle = 0;
		t->t_events++;
	}
}

static int
qpost(h, msg, wp, lp)
	u32 h, msg, wp, lp;
{
	struct qmsg *m;

	if ((qtail + 1) % QSIZE == qhead)
		return 0;
	m = &q[qtail];
	m->hwnd = h;
	m->msg = msg;
	m->wp = wp;
	m->lp = lp;
	m->time = w16_ticks();
	m->x = scr_mx;
	m->y = scr_my;
	m->task = owner(h);
	wake(m->task);
	qtail = (qtail + 1) % QSIZE;
	return 1;
}

int
wnd_post(h, msg, wp, lp)
	u32 h, msg, wp, lp;
{
	return qpost(h, msg, wp, lp);
}

/* PostAppMessage: a message with no window for task t */
int
user_posttask(t, msg, wp, lp)
	struct task *t;
	u32 msg, wp, lp;
{
	if (!t || !qpost((u32)0, msg, wp, lp))
		return 0;
	q[(qtail + QSIZE - 1) % QSIZE].task = t;
	wake(t);
	return 1;
}

/*
 * A task with nothing to do but wait (WaitEvent): the others run, or the
 * session waits for input, the drivers' work going on meanwhile.
 */
void
user_idle()
{
	struct ev e;
	int t;

	rawfill();
	mm_tick();
	ws_poll();
	if (task_othersready()) {
		if (curtask)
			curtask->t_idle = 1;
		task_yield();
		if (curtask)
			curtask->t_idle = 0;
		return;
	}
	t = timer_next();
	if ((t < 0 || t > 20) && mm_next() >= 0 && mm_next() < 20)
		t = mm_next();
	if (t < 0 || t > 50)
		t = 50;
	if (scr_poll(&e, t) == 1)
		rawput(&e);
}

/* ---- timers ---- */

#define	NTIMER	64

struct timer {
	u16	hwnd, id;
	u32	ms, due, proc;
	int	used, sys;
	struct task *task;
};
static struct timer timers[NTIMER];

static int
timer_set(h, id, ms, proc, sys)
	u32 h, id, ms, proc;
	int sys;
{
	int i, f = -1;

	if (ms < 1)
		ms = 1;
	for (i = 0; i < NTIMER; i++) {
		if (timers[i].used && timers[i].hwnd == h && timers[i].id == id && h) {
			f = i;
			break;
		}
		if (!timers[i].used && f < 0)
			f = i;
	}
	if (f < 0)
		return 0;
	if (!h) {
		/* a timer without a window: its id is ours to give */
		static int next = 1;

		id = 0x100 + next++;
	}
	timers[f].used = 1;
	timers[f].hwnd = h;
	timers[f].id = id;
	timers[f].ms = ms < 55 ? 55 : ms;	/* the PC's 18.2 Hz tick */
	timers[f].due = w16_ticks() + timers[f].ms;
	timers[f].proc = proc;
	timers[f].sys = sys;
	timers[f].task = owner(h);
	return h ? id : id;
}

static int
timer_kill(h, id)
	u32 h, id;
{
	int i;

	for (i = 0; i < NTIMER; i++)
		if (timers[i].used && timers[i].hwnd == (h & 0xffff) && timers[i].id == (id & 0xffff)) {
			timers[i].used = 0;
			return 1;
		}
	return 0;
}

/* ms until the next timer, -1 none */
static int
timer_next()
{
	u32 now = w16_ticks();
	int i, best = -1, d;

	for (i = 0; i < NTIMER; i++)
		if (timers[i].used) {
			d = (int)(timers[i].due - now);
			if (d < 0)
				d = 0;
			if (best < 0 || d < best)
				best = d;
		}
	return best;
}

/* ---- caret ---- */

static struct {
	u16 hwnd;
	int x, y, w, h;
	int shown;		/* ShowCaret count less HideCaret */
	int on;			/* drawn now */
	u32 next;
	u16 bitmap;
} caret;

static void
caret_draw()
{
	struct wnd *w = wnd_get(caret.hwnd);
	u16 hdc;
	struct dc *dc;
	struct rect r;

	if (!w)
		return;
	hdc = user_dc(w, DCK_WINDOW, (struct rgn *)0);
	dc = dc_get(hdc);
	r_set(&r, dc->ox + caret.x, dc->oy + caret.y, dc->ox + caret.x + caret.w, dc->oy + caret.y + caret.h);
	d_invert(dc, &r);
	user_releasedc(hdc);
	caret.on = !caret.on;
}

void
caret_hide()
{
	if (caret.on)
		caret_draw();
}

void
caret_show()
{
	if (caret.hwnd && caret.shown > 0 && !caret.on)
		caret_draw();
	caret.next = w16_ticks() + 500;
}

static void
caret_blink()
{
	if (!caret.hwnd || caret.shown <= 0)
		return;
	if ((int)(w16_ticks() - caret.next) >= 0) {
		caret_draw();
		caret.next = w16_ticks() + 500;
	}
}

/* ---- DCs on windows ---- */

void
dc_refresh(dc)
	struct dc *dc;
{
	struct wnd *w = wnd_get(dc->hwnd);

	if (!w) {
		dc->vis.n = 0;
		dc->epoch = vis_epoch;
		dc->effok = 0;
		return;
	}
	wnd_visrgn(w, &dc->vis, dc->kind == DCK_WINDOW);
	dc->ox = dc->kind == DCK_WINDOW ? w->cr.l : w->wr.l;
	dc->oy = dc->kind == DCK_WINDOW ? w->cr.t : w->wr.t;
	dc->epoch = vis_epoch;
	dc->effok = 0;
}

u16
user_dc(w, kind, paint)
	struct wnd *w;
	int kind;
	struct rgn *paint;
{
	u16 h;
	struct dc *dc;

	if (kind == DCK_WINDOW && w->cls && (w->cls->style & CS_OWNDC)) {
		if (!w->owndc || !dc_get(w->owndc)) {
			w->owndc = dc_new(DCK_WINDOW);
			dc_get(w->owndc)->ownwindow = 1;
		}
		h = w->owndc;
	} else
		h = dc_new(kind);
	dc = dc_get(h);
	dc->s = &screen;
	dc->hwnd = w->h;
	dc->kind = kind;
	if (dc->paint) {
		rgn_free(dc->paint);
		free(dc->paint);
		dc->paint = 0;
	}
	if (paint) {
		dc->paint = (struct rgn *)malloc(sizeof(struct rgn));
		rgn_init(dc->paint);
		rgn_copy(dc->paint, paint);
	}
	dc_refresh(dc);
	return h;
}

void
user_releasedc(h)
	u32 h;
{
	struct dc *dc = dc_get(h);

	if (!dc)
		return;
	if (dc->ownwindow) {
		if (dc->paint) {
			rgn_free(dc->paint);
			free(dc->paint);
			dc->paint = 0;
			dc->effok = 0;
		}
		return;
	}
	dc_free(h);
}

/* ---- creating and destroying ---- */

static void
sizemsgs(w)
	struct wnd *w;
{
	w->flags &= ~WF_NEEDSIZE;
	wnd_send(w, WM_SIZE, (w->style & WS_MAXIMIZE) ? 2 : (w->style & WS_MINIMIZE) ? 1 : 0,
	    FP(w->cr.b - w->cr.t, w->cr.r - w->cr.l));
	if (!wnd_get(w->h))
		return;
	wnd_send(w, WM_MOVE, 0, FP(w->cr.t - (w->parent == desktop ? 0 : w->parent->cr.t),
	    w->cr.l - (w->parent == desktop ? 0 : w->parent->cr.l)));
}

static int cascade;

struct wnd *
wnd_create(ex, cname, title, style, x, y, cx, cy, hparent, hmenu, hinst, param)
	u32 ex, cname, title, style, hparent, hmenu, hinst, param;
	int x, y, cx, cy;
{
	struct cls *c = cls_find(cname, hinst);
	struct wnd *w, *parent = wnd_get(hparent), *owner = 0;
	u32 cs, l;
	char *t;
	struct rect r;

	if (!c) {
		w16_log("startwin: CreateWindow: no class \"%s\"\n", ISINT(cname) ? "#" : STR(cname));
		return 0;
	}
	if (style & WS_CHILD) {
		if (!parent)
			return 0;
	} else {
		owner = parent ? wnd_toplevel(parent) : 0;
		parent = desktop;
	}
	if (!(w = wnd_alloc()))
		return 0;
	w->cls = c;
	w->style = style;
	w->exstyle = ex;
	w->parent = parent;
	w->owner = owner;
	w->proc = c->proc;
	w->hinst = hinst;
	w->id = hmenu;
	w->extra = (u8 *)calloc(1, c->wndextra + 8);
	t = ISINT(title) ? "" : STR(title);
	w->text = strdup(t);
	w->task = curtask;
	w->hicon = c->icon;
	/* default places and sizes for top-level windows */
	if (!(style & WS_CHILD)) {
		if (x == CW_USEDEFAULT || (short)x == CW_USEDEFAULT) {
			x = 16 + 24 * (cascade % 8);
			y = 16 + 24 * (cascade % 8);
			cascade++;
		}
		if (cx == CW_USEDEFAULT || (short)cx == CW_USEDEFAULT) {
			cx = screen.w * 3 / 4;
			cy = screen.h * 3 / 4;
		}
		if (!w->id && c->menuname && !(style & WS_CHILD))
			w->id = menu_load(hinst, c->menuname);
		if (w->id && (style & WS_CHILD))
			w->id = 0;
	} else {
		if ((short)x == CW_USEDEFAULT)
			x = y = 0;
		if ((short)cx == CW_USEDEFAULT)
			cx = cy = 0;
		x += parent->cr.l;
		y += parent->cr.t;
	}
	if (cx < 0) cx = 0;
	if (cy < 0) cy = 0;
	r_set(&w->wr, x, y, x + cx, y + cy);
	wnd_calcclient(w, &w->wr, &w->cr);
	w->normal = w->wr;
	if (c->style & CS_SAVEBITS)
		;
	w->style &= ~WS_VISIBLE;
	/* children at the bottom, so siblings stay in the order they were made (dialog tab order) */
	link_w(w, (style & WS_CHILD) ? (struct wnd *)1 : (struct wnd *)0);
	vis_epoch++;
	/* CREATESTRUCT */
	cs = ualloc(34);
	l = ulin(cs);
	PL(l, param);
	PW(l + 4, hinst);
	PW(l + 6, w->id);
	PW(l + 8, (style & WS_CHILD) ? parent->h : owner ? owner->h : 0);
	PW(l + 10, cy);
	PW(l + 12, cx);
	PW(l + 14, (style & WS_CHILD) ? y - parent->cr.t : y);
	PW(l + 16, (style & WS_CHILD) ? x - parent->cr.l : x);
	PL(l + 18, style);
	PL(l + 22, title);
	PL(l + 26, cname);
	PL(l + 30, ex);
	if (!wnd_send(w, WM_NCCREATE, 0, cs)) {
		ufree(cs);
		wnd_destroy(w);
		return 0;
	}
	if (!wnd_get(w->h))
		return 0;
	nccalc(w);
	if ((s32)wnd_send(w, WM_CREATE, 0, cs) == -1) {
		ufree(cs);
		if (wnd_get(w->h))
			wnd_destroy(w);
		return 0;
	}
	ufree(cs);
	if (!wnd_get(w->h))
		return 0;
	w->flags |= WF_CREATED;
	/*
	 * as Windows 3.1: a window made hidden hears its size when first
	 * shown (programs set up in between), even when moved or sized
	 * meanwhile: PIF Editor fits its scroll bars only while visible
	 */
	if (style & WS_VISIBLE)
		sizemsgs(w);
	else
		w->flags |= WF_NEEDSIZE;
	if ((style & WS_CHILD) && !(ex & WS_EX_NOPARENTNOTIFY))
		wnd_send(parent, WM_PARENTNOTIFY, WM_CREATE, FP(w->h, w->id));
	if (!wnd_get(w->h))
		return 0;
	if (style & WS_MAXIMIZE)
		wnd_show(w, SW_SHOWMAXIMIZED);
	else if (style & WS_VISIBLE)
		wnd_show(w, SW_SHOW);
	(void)r;
	return w;
}

static void
destroy_r(w)
	struct wnd *w;
{
	struct wnd *c, *n;
	int i;

	w->flags |= WF_DESTROYING;
	wnd_send(w, WM_DESTROY, 0, 0);
	for (c = w->child; c; c = n) {
		n = c->next;
		if (wnd_get(c->h))
			destroy_r(c);
	}
	/* owned windows go too */
	for (i = 0; i < NWND; i++)
		if (wtab[i] && wtab[i]->owner == w && !(wtab[i]->flags & WF_DESTROYING))
			destroy_r(wtab[i]);
	wnd_send(w, WM_NCDESTROY, 0, 0);
	for (i = 0; i < NTIMER; i++)
		if (timers[i].used && timers[i].hwnd == w->h)
			timers[i].used = 0;
	if (caret.hwnd == w->h)
		caret.hwnd = 0;
	if (wnd_focus == w)
		wnd_focus = 0;
	if (wnd_capture == w)
		wnd_capture = 0;
	if (wnd_active == w)
		wnd_active = 0;
	unlink_w(w);
	if (w->owndc)
		dc_free(w->owndc);
	if (!(w->style & WS_CHILD) && w->id && !(w->flags & WF_MENUOWNED))
		menu_destroy(w->id);
	rgn_free(&w->upd);
	free(w->text);
	free(w->extra);
	wtab[(w->h - WBASE) >> 2] = 0;
	free(w);
}

void
wnd_destroy(w)
	struct wnd *w;
{
	struct rect r;
	int wasvis, top;
	struct wnd *parent, *owner;

	if (!w || w == desktop || (w->flags & WF_DESTROYING))
		return;
	wasvis = wnd_visible(w);
	r = w->wr;
	top = !(w->style & WS_CHILD);
	parent = w->parent;
	owner = w->owner;
	if ((w->style & WS_CHILD) && !(w->exstyle & WS_EX_NOPARENTNOTIFY) && parent)
		wnd_send(parent, WM_PARENTNOTIFY, WM_DESTROY, FP(w->h, w->id));
	if (!wnd_get(w->h))
		return;
	if (wasvis)
		w->style &= ~WS_VISIBLE;
	destroy_r(w);
	vis_epoch++;
	if (wasvis)
		expose(&r);
	/* activate another top-level window */
	if (top && !wnd_active) {
		struct wnd *n;

		if (owner && wnd_get(owner->h) && wnd_visible(owner))
			wnd_activate(owner, WA_ACTIVE);
		else
			for (n = desktop->child; n; n = n->next)
				if ((n->style & WS_VISIBLE) && !(n->style & WS_DISABLED)) {
					wnd_activate(n, WA_ACTIVE);
					break;
				}
	}
}

/* ---- position and visibility ---- */

void
wnd_setpos(w, after, x, y, cx, cy, flags)
	struct wnd *w, *after;
	int x, y, cx, cy, flags;
{
	struct rect old = w->wr, oldc = w->cr, nr;
	int vis = wnd_visible(w), dx, dy, moved, sized;
	u32 wp, l;

	if (w == desktop)
		return;
	/* WINDOWPOS for WM_WINDOWPOSCHANGING */
	wp = ualloc(14);
	l = ulin(wp);
	PW(l, w->h);
	PW(l + 2, after ? (after == (struct wnd *)1 ? 1 : after->h) : 0);
	PW(l + 4, x);
	PW(l + 6, y);
	PW(l + 8, cx);
	PW(l + 10, cy);
	PW(l + 12, flags);
	wnd_send(w, WM_WINDOWPOSCHANGING, 0, wp);
	if (!wnd_get(w->h)) {
		ufree(wp);
		return;
	}
	x = (short)GW(l + 4);
	y = (short)GW(l + 6);
	cx = (short)GW(l + 8);
	cy = (short)GW(l + 10);
	flags = GW(l + 12);
	if (flags & SWP_NOMOVE) {
		x = w->wr.l - (w->style & WS_CHILD ? w->parent->cr.l : 0);
		y = w->wr.t - (w->style & WS_CHILD ? w->parent->cr.t : 0);
	}
	if (flags & SWP_NOSIZE) {
		cx = w->wr.r - w->wr.l;
		cy = w->wr.b - w->wr.t;
	}
	if (cx < 0) cx = 0;
	if (cy < 0) cy = 0;
	if (w->style & WS_CHILD) {
		x += w->parent->cr.l;
		y += w->parent->cr.t;
	}
	r_set(&nr, x, y, x + cx, y + cy);
	dx = nr.l - old.l;
	dy = nr.t - old.t;
	moved = dx || dy;
	sized = nr.r - nr.l != old.r - old.l || nr.b - nr.t != old.b - old.t;
	if (moved)
		offsetall(w, dx, dy);
	w->wr = nr;
	if (sized || moved || (flags & SWP_FRAMECHANGED))
		nccalc(w);
	if (!(flags & SWP_NOZORDER)) {
		unlink_w(w);
		link_w(w, after);
	}
	if (flags & SWP_SHOWWINDOW)
		w->style |= WS_VISIBLE;
	if (flags & SWP_HIDEWINDOW)
		w->style &= ~WS_VISIBLE;
	vis_epoch++;
	if (!(flags & SWP_NOREDRAW)) {
		if (vis)
			expose(&old);
		if (wnd_visible(w)) {
			expose_in(w, &w->wr, 1);
			w->ncpaint = 1;
		}
	}
	(void)oldc;
	PW(l + 4, w->wr.l - (w->style & WS_CHILD ? w->parent->cr.l : 0));
	PW(l + 6, w->wr.t - (w->style & WS_CHILD ? w->parent->cr.t : 0));
	PW(l + 8, w->wr.r - w->wr.l);
	PW(l + 10, w->wr.b - w->wr.t);
	PW(l + 12, flags | (moved ? 0 : SWP_NOMOVE) | (sized ? 0 : SWP_NOSIZE));
	wnd_send(w, WM_WINDOWPOSCHANGED, 0, wp);
	ufree(wp);
}

void
wnd_show(w, cmd)
	struct wnd *w;
	int cmd;
{
	int was = (w->style & WS_VISIBLE) != 0, show = cmd != SW_HIDE;
	struct rect r;

	if (cmd == SW_SHOWMAXIMIZED || cmd == SW_MAXIMIZE) {
		if (!(w->style & WS_MAXIMIZE)) {
			int b = (w->style & WS_THICKFRAME) ? sys_metric(SM_CXFRAME) : 1;

			if (!(w->style & (WS_MINIMIZE)))
				w->normal = w->wr;
			w->style |= WS_MAXIMIZE;
			w->style &= ~WS_MINIMIZE;
			if (w->style & WS_CHILD)
				r = w->parent->cr;
			else
				r = desktop->wr;
			r.l -= b; r.t -= b; r.r += b; r.b += b;
			/* an MDI document's caption goes out of sight; the frame shows it */
			if ((w->exstyle & 0x40) && (w->style & WS_CAPTION) == WS_CAPTION)
				r.t -= sys_metric(SM_CYCAPTION) - 1;
			if (w->style & WS_CHILD) {
				r.l -= w->parent->cr.l; r.r -= w->parent->cr.l;
				r.t -= w->parent->cr.t; r.b -= w->parent->cr.t;
			}
			wnd_setpos(w, (struct wnd *)0, r.l, r.t, r.r - r.l, r.b - r.t,
			    SWP_SHOWWINDOW | SWP_FRAMECHANGED | (w->style & WS_CHILD ? SWP_NOZORDER : 0));
			wnd_send(w, WM_SIZE, 2, FP(w->cr.b - w->cr.t, w->cr.r - w->cr.l));
		}
	} else if (cmd == SW_SHOWMINIMIZED || cmd == SW_MINIMIZE || cmd == SW_SHOWMINNOACTIVE) {
		if (!(w->style & WS_MINIMIZE)) {
			static int slot;
			int ix = 8 + 76 * (slot++ % 8), iy = desktop->wr.b - 64;

			if (!(w->style & WS_MAXIMIZE))
				w->normal = w->wr;
			w->style |= WS_MINIMIZE;
			w->style &= ~WS_MAXIMIZE;
			/* a child's icon at the foot of its parent */
			if (w->style & WS_CHILD) {
				ix = 20 + 76 * (slot % 6);
				iy = w->parent->cr.b - w->parent->cr.t - 54;
			}
			wnd_setpos(w, (struct wnd *)0, ix, iy, 36, 36,
			    SWP_SHOWWINDOW | SWP_FRAMECHANGED);
			wnd_send(w, WM_SIZE, 1, 0);
			if (wnd_active == w && wnd_focus && wnd_toplevel(wnd_focus) == w)
				wnd_setfocus((struct wnd *)0);
		}
	} else if (cmd == SW_RESTORE || ((cmd == SW_SHOWNORMAL) && (w->style & (WS_MINIMIZE | WS_MAXIMIZE)))) {
		if (w->style & (WS_MINIMIZE | WS_MAXIMIZE)) {
			r = w->normal;
			w->style &= ~(WS_MINIMIZE | WS_MAXIMIZE);
			if (w->style & WS_CHILD) {
				r.l -= w->parent->cr.l; r.r -= w->parent->cr.l;
				r.t -= w->parent->cr.t; r.b -= w->parent->cr.t;
			}
			wnd_setpos(w, (struct wnd *)0, r.l, r.t, r.r - r.l, r.b - r.t,
			    SWP_SHOWWINDOW | SWP_FRAMECHANGED);
			wnd_send(w, WM_SIZE, 0, FP(w->cr.b - w->cr.t, w->cr.r - w->cr.l));
		}
	}
	if (show != was) {
		wnd_send(w, WM_SHOWWINDOW, show, 0);
		if (!wnd_get(w->h))
			return;
		wnd_setpos(w, (struct wnd *)0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
		    (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
	}
	if (show && (w->flags & WF_NEEDSIZE) && wnd_get(w->h))
		sizemsgs(w);
	if (!wnd_get(w->h))
		return;
	if (!(w->style & WS_CHILD)) {
		if (show && cmd != SW_SHOWNOACTIVATE && cmd != SW_SHOWNA && cmd != SW_SHOWMINNOACTIVE &&
		    !(w->style & WS_DISABLED))
			wnd_activate(w, WA_ACTIVE);
		else if (!show && wnd_active == w) {
			struct wnd *n;

			wnd_active = 0;
			if (w->owner && wnd_visible(w->owner))
				wnd_activate(w->owner, WA_ACTIVE);
			else
				for (n = desktop->child; n; n = n->next)
					if (n != w && (n->style & WS_VISIBLE)) {
						wnd_activate(n, WA_ACTIVE);
						break;
					}
		}
	} else if (!show && wnd_focus && (wnd_focus == w || IsChildOf(w, wnd_focus)))
		wnd_setfocus(w->parent);
	else if (show && (w->exstyle & WS_EX_MDICHILD) && (cmd == SW_SHOWNORMAL || cmd == SW_SHOW || cmd == SW_RESTORE ||
	    cmd == SW_SHOWMAXIMIZED || cmd == SW_SHOWMINIMIZED)) {
		/* an MDI document, as SetWindowPos without SWP_NOACTIVATE: to the top, and told, so activated */
		wnd_setpos(w, (struct wnd *)0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
		if (wnd_get(w->h))
			wnd_send(w, WM_CHILDACTIVATE, 0, 0);
	}
}

int
IsChildOf(p, c)
	struct wnd *p, *c;
{
	for (c = c ? c->parent : 0; c; c = c->parent)
		if (c == p)
			return 1;
	return 0;
}

/* ---- focus, activation, capture ---- */

void
wnd_setfocus(w)
	struct wnd *w;
{
	struct wnd *old = wnd_focus;

	if (w == old)
		return;
	if (w && (w->style & WS_MINIMIZE))
		w = 0;
	wnd_focus = w;
	if (old && wnd_get(old->h))
		wnd_send(old, WM_KILLFOCUS, w ? w->h : 0, 0);
	if (wnd_focus == w && w && wnd_get(w->h))
		wnd_send(w, WM_SETFOCUS, old && wnd_get(old->h) ? old->h : 0, 0);
}

void
wnd_activate(w, how)
	struct wnd *w;
	int how;
{
	struct wnd *old = wnd_active, *t;

	w = wnd_toplevel(w);
	if (w == old || !w)
		return;
	if (old && wnd_get(old->h)) {
		wnd_send(old, WM_NCACTIVATE, 0, 0);
		wnd_send(old, WM_ACTIVATE, WA_INACTIVE, FP(0, w->h));
	}
	wnd_active = w;
	/* to the top of the z-order, with the windows it owns above it */
	if (desktop->child != w) {
		unlink_w(w);
		link_w(w, (struct wnd *)0);
		vis_epoch++;
		expose_in(w, &w->wr, 1);
		w->ncpaint = 1;
		for (t = desktop->child; t; t = t->next)
			if (t->owner == w && t != w && (t->style & WS_VISIBLE)) {
				unlink_w(t);
				link_w(t, (struct wnd *)0);
				vis_epoch++;
				expose_in(t, &t->wr, 1);
				t->ncpaint = 1;
				break;
			}
	}
	wnd_send(w, WM_NCACTIVATE, 1, 0);
	wnd_send(w, WM_ACTIVATE, how, FP((w->style & WS_MINIMIZE) != 0, old ? old->h : 0));
	if (wnd_active == w && (!wnd_focus || wnd_toplevel(wnd_focus) != w))
		wnd_setfocus(w);
}

/* ---- hit testing ---- */

/* the deepest window at (x, y) under w, skipping hit-transparent ones */
static struct wnd *
frompoint(w, x, y, htp)
	struct wnd *w;
	int x, y, *htp;
{
	struct wnd *c, *r;
	struct rect *rr;
	int ht;

	for (c = w->child; c; c = c->next) {
		rr = &c->wr;
		if (!(c->style & WS_VISIBLE) || x < rr->l || x >= rr->r || y < rr->t || y >= rr->b)
			continue;
		if (!(c->style & WS_DISABLED) && x >= c->cr.l && x < c->cr.r && y >= c->cr.t && y < c->cr.b &&
		    (r = frompoint(c, x, y, htp)) != 0)
			return r;
		ht = (short)wnd_send(c, WM_NCHITTEST, 0, FP(y, x));
		if (ht == HTTRANSPARENT)
			continue;
		*htp = ht;
		return c;
	}
	return 0;
}

struct wnd *
wnd_frompoint(x, y, htp)
	int x, y, *htp;
{
	struct wnd *w;
	int ht = HTCLIENT;

	w = frompoint(desktop, x, y, &ht);
	if (!w) {
		w = desktop;
		ht = HTCLIENT;
	}
	if (htp)
		*htp = ht;
	return w;
}

/* ---- input ---- */

static u32 lastclick[3], lastclickpos[3];
static u16 lastclickwnd[3];
static int btnstate;

/* a modal loop of ours took clicks: the next one is not a double click */
void
user_resetclicks()
{
	memset(lastclick, 0, sizeof lastclick);
}

static int
mkeys()
{
	return (btnstate & 1 ? 1 : 0) | (btnstate & 2 ? 2 : 0) | (btnstate & 4 ? 0x10 : 0) |
	    (asyncstate[VK_SHIFT] & 0x80 ? 4 : 0) | (asyncstate[VK_CONTROL] & 0x80 ? 8 : 0);
}

static void
setcursor_for(w, ht, msg)
	struct wnd *w;
	int ht, msg;
{
	if (!wnd_send(w, WM_SETCURSOR, w->h, FP(msg, ht & 0xffff)))
		;
}

/* a disabled modal owner: clicks on it beep */
static int
blocked(w)
	struct wnd *w;
{
	struct wnd *t = wnd_toplevel(w);

	return t && t != desktop && (t->style & WS_DISABLED);
}

static void
input(e)
	struct ev *e;
{
	struct wnd *w;
	int ht, x, y, msg, k;
	u32 now = w16_ticks();
	static u16 lastmove;

	kernel_tick();
	switch (e->type) {
	case EV_MOVE:
		scr_mx = e->x;
		scr_my = e->y;
		/* moves since the last are one */
		if (qhead != qtail) {
			int last = (qtail + QSIZE - 1) % QSIZE;

			if ((q[last].msg == WM_MOUSEMOVE || q[last].msg == WM_NCMOUSEMOVE) && q[last].hwnd == lastmove)
				qtail = last;
		}
		if (wnd_capture) {
			w = wnd_capture;
			ht = HTCLIENT;
		} else {
			w = wnd_frompoint(e->x, e->y, &ht);
			setcursor_for(w, ht, WM_MOUSEMOVE);
		}
		lastmove = w->h;
		if (ht == HTCLIENT)
			qpost(w->h, WM_MOUSEMOVE, mkeys(), FP(e->y - w->cr.t, e->x - w->cr.l));
		else
			qpost(w->h, WM_NCMOUSEMOVE, ht, FP(e->y, e->x));
		return;
	case EV_BTN:
		user_alttap = 0;
		scr_mx = e->x;
		scr_my = e->y;
		k = e->btn > 2 ? 0 : e->btn;
		if (e->down)
			btnstate |= 1 << k;
		else
			btnstate &= ~(1 << k);
		asyncstate[k == 0 ? VK_LBUTTON : k == 1 ? VK_RBUTTON : VK_MBUTTON] = e->down ? 0x80 : 0;
		if (wnd_capture) {
			w = wnd_capture;
			ht = HTCLIENT;
		} else
			w = wnd_frompoint(e->x, e->y, &ht);
		if (e->down && blocked(w) && !wnd_capture) {
			user_beep(0);
			return;
		}
		msg = (k == 0 ? WM_LBUTTONDOWN : k == 1 ? WM_RBUTTONDOWN : WM_MBUTTONDOWN) + (e->down ? 0 : 1);
		if (e->down && !wnd_capture) {
			struct wnd *t = wnd_toplevel(w);

			/* a click on an inactive window activates it */
			if (t && t != desktop && t != wnd_active) {
				int ma = wnd_send(w, WM_MOUSEACTIVATE, t->h, FP(msg, ht));

				if (ma != 3 && ma != 4)		/* MA_NOACTIVATE(ANDEAT) */
					wnd_activate(t, WA_CLICKACTIVE);
				if (ma == 2 || ma == 4)		/* MA_ACTIVATEANDEAT */
					return;
			}
			/* and a click in an MDI document activates that */
			{
				extern void mdi_click();

				mdi_click(w);
			}
			setcursor_for(w, ht, msg);
			/* double clicks, for classes that want them and on the frame */
			if (lastclickwnd[k] == w->h && now - lastclick[k] < 500 &&
			    abs((int)(lastclickpos[k] & 0xffff) - e->x) <= 4 &&
			    abs((int)(lastclickpos[k] >> 16) - e->y) <= 4 &&
			    (ht != HTCLIENT || (w->cls && (w->cls->style & CS_DBLCLKS)))) {
				msg += 2;
				lastclick[k] = 0;
			} else {
				lastclick[k] = now;
				lastclickpos[k] = FP(e->y, e->x);
				lastclickwnd[k] = w->h;
			}
		}
		if (ht == HTCLIENT || wnd_capture) {
			x = e->x - w->cr.l;
			y = e->y - w->cr.t;
			qpost(w->h, msg, mkeys(), FP(y & 0xffff, x & 0xffff));
		} else
			qpost(w->h, msg - WM_MOUSEMOVE + WM_NCMOUSEMOVE, ht, FP(e->y, e->x));
		return;
	case EV_KEY:
		{
			int vk = e->vk & 0xff, sys, up = !e->down;
			u32 lp = 1 | (u32)(e->sc & 0xff) << 16;

			if (e->down) {
				if (!(asyncstate[vk] & 0x80))
					user_alttap = vk == VK_MENU;
				if (asyncstate[vk] & 0x80)
					lp |= 0x40000000;	/* repeat */
				else
					asyncstate[vk] ^= 1;	/* toggle */
				asyncstate[vk] |= 0x80;
			} else {
				asyncstate[vk] &= ~0x80;
				lp |= 0xc0000000;
			}
			if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
				/* left and right are one to Windows 3.1 */
			}
			sys = (asyncstate[VK_MENU] & 0x80) && !(asyncstate[VK_CONTROL] & 0x80);
			if (vk == VK_MENU || vk == VK_F1 + 9)
				sys = 1;
			if (sys)
				lp |= 0x20000000;
			w = wnd_focus;
			if (!w) {
				w = wnd_active;
				sys = 1;
			}
			if (!w)
				return;
			if (sys)
				qpost(w->h, up ? WM_SYSKEYUP : WM_SYSKEYDOWN, vk, lp);
			else
				qpost(w->h, up ? WM_KEYUP : WM_KEYDOWN, vk, lp);
		}
		return;
	case EV_QUIT:
		/* asked again (the script's end too) after WM_QUIT is posted: that stays */
		if (quitting)
			return;
		quitting = 1;
		if (wnd_active)
			qpost(wnd_active->h, WM_SYSCOMMAND, SC_CLOSE, 0);
		return;
	case EV_SHOWN:
		vis_epoch++;
		expose(&desktop->wr);
		return;
	}
}

/* ---- the system queue: raw input, made into messages as it is taken ---- */

#define	RAWQ	512
static struct ev rawq[RAWQ];
static int rhead, rtail;

static void
rawput(e)
	struct ev *e;
{
	if ((rtail + 1) % RAWQ == rhead)
		return;
	rawq[rtail] = *e;
	rtail = (rtail + 1) % RAWQ;
}

/* the device's events into the system queue */
static void
rawfill()
{
	struct ev e;

	while (scr_poll(&e, 0) == 1)
		rawput(&e);
}

/* our own modal loops take input here: what is queued first */
int
user_poll(e, timeout)
	struct ev *e;
	int timeout;
{
	if (rhead != rtail) {
		*e = rawq[rhead];
		rhead = (rhead + 1) % RAWQ;
		return 1;
	}
	return scr_poll(e, timeout);
}

/* ---- getting messages ---- */

/* the first window needing paint, frames first */
static struct wnd *
needpaint(w)
	struct wnd *w;
{
	struct wnd *c, *r;

	if (w != desktop && !(w->style & WS_VISIBLE))
		return 0;
	if (w->ncpaint || w->upd.n || (w->flags & WF_INTERNALPAINT)) {
		if (w == desktop || !w->task || w->task == curtask)
			return w;
		wake(w->task);		/* another task's to paint */
	}
	for (c = w->child; c; c = c->next)
		if ((r = needpaint(c)) != 0)
			return r;
	return 0;
}

/* the key state follows the input messages as they are taken */
static void
syncstate(m)
	struct qmsg *m;
{
	int vk = m->wp & 0xff;

	switch (m->msg) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (!(keystate[vk] & 0x80))
			keystate[vk] ^= 1;
		keystate[vk] |= 0x80;
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		keystate[vk] &= ~0x80;
		break;
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
		keystate[VK_LBUTTON] |= 0x80;
		break;
	case WM_LBUTTONUP: case WM_NCLBUTTONUP:
		keystate[VK_LBUTTON] &= ~0x80;
		break;
	case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_NCRBUTTONDOWN:
		keystate[VK_RBUTTON] |= 0x80;
		break;
	case WM_RBUTTONUP: case WM_NCRBUTTONUP:
		keystate[VK_RBUTTON] &= ~0x80;
		break;
	}
}

static int
filt(m, h, min, max)
	struct qmsg *m;
	u32 h;
	int min, max;
{
	if (m->task && m->task != curtask)
		return 0;
	if (h && m->hwnd != h && !IsChildOf(wnd_get(h), wnd_get(m->hwnd)))
		return 0;
	if ((min || max) && (m->msg < min || m->msg > max))
		return 0;
	return 1;
}

static void
putmsg(a, m)
	u32 a;
	struct qmsg *m;
{
	PW(a + MSG_HWND, m->hwnd);
	PW(a + MSG_MESSAGE, m->msg);
	PW(a + MSG_WPARAM, m->wp);
	PL(a + MSG_LPARAM, m->lp);
	PL(a + MSG_TIME, m->time);
	PW(a + MSG_PT, m->x);
	PW(a + MSG_PT + 2, m->y);
}

void
user_ncpaint(w)
	struct wnd *w;
{
	w->ncpaint = 0;
	if (wnd_visible(w))
		wnd_send(w, WM_NCPAINT, 0, 0);
}

/*
 * A message into the guest MSG at a: 1 one, 0 none (wait 0), -1 WM_QUIT.
 * Waiting, the screen is flushed and the device polled.
 */
int
user_getmessage(a, h, min, max, remove, wait)
	u32 a, h;
	int min, max, remove, wait;
{
	struct qmsg m;
	struct wnd *w;
	struct ev e;
	int i, t, polled = 0;

	h &= 0xffff;
	for (;;) {
		extern void mm_tick();
		extern int mm_next();

		/* input from the device; into messages one at a time, when the queue is empty */
		rawfill();
		caret_blink();
		mm_tick();
		ws_poll();
		/* the queue */
	again:
		for (i = qhead; i != qtail; i = (i + 1) % QSIZE)
			if ((q[i].hwnd && !wnd_get(q[i].hwnd)) || filt(&q[i], h, min, max)) {
				m = q[i];
				/* a destroyed window's messages go with it */
				if (m.hwnd && !wnd_get(m.hwnd)) {
					int j;

					for (j = i; j != qhead; j = (j + QSIZE - 1) % QSIZE)
						q[j] = q[(j + QSIZE - 1) % QSIZE];
					qhead = (qhead + 1) % QSIZE;
					goto again;
				}
				if (remove) {
					/* close the gap */
					int j;

					for (j = i; j != qhead; j = (j + QSIZE - 1) % QSIZE)
						q[j] = q[(j + QSIZE - 1) % QSIZE];
					qhead = (qhead + 1) % QSIZE;
				}
				putmsg(a, &m);
				if (remove)
					syncstate(&m);
				return m.msg == WM_QUIT ? -1 : 1;
			}
		/* nothing queued that fits: the next input becomes a message */
		if (rhead != rtail) {
			e = rawq[rhead];
			rhead = (rhead + 1) % RAWQ;
			input(&e);
			continue;
		}
		if (curtask && curtask->t_quit && !h) {
			memset(&m, 0, sizeof m);
			m.msg = WM_QUIT;
			m.wp = curtask->t_quitcode;
			putmsg(a, &m);
			if (remove)
				curtask->t_quit = 0;
			return -1;
		}
		/* paint */
		while ((w = needpaint(desktop)) != 0) {
			if (w->ncpaint) {
				user_ncpaint(w);
				continue;
			}
			if (w == desktop || (h && w->h != h && !IsChildOf(wnd_get(h), w)) ||
			    ((min || max) && (WM_PAINT < min || WM_PAINT > max))) {
				if (w == desktop) {
					wnd_send(w, WM_PAINT, 0, 0);
					continue;
				}
				break;
			}
			memset(&m, 0, sizeof m);
			m.hwnd = w->h;
			m.msg = WM_PAINT;
			m.time = w16_ticks();
			putmsg(a, &m);
			return 1;
		}
		/* timers */
		for (i = 0; i < NTIMER; i++)
			if (timers[i].used && (int)(w16_ticks() - timers[i].due) >= 0 && timers[i].task &&
			    timers[i].task != curtask)
				wake(timers[i].task);
			else if (timers[i].used && (int)(w16_ticks() - timers[i].due) >= 0 &&
			    (!h || timers[i].hwnd == h) && (!(min || max) || (WM_TIMER >= min && WM_TIMER <= max))) {
				memset(&m, 0, sizeof m);
				m.hwnd = timers[i].hwnd;
				m.msg = timers[i].sys ? 0x118 : WM_TIMER;
				m.wp = timers[i].id;
				m.lp = timers[i].proc;
				m.time = w16_ticks();
				if (remove)
					timers[i].due = w16_ticks() + timers[i].ms;
				putmsg(a, &m);
				return 1;
			}
		scr_flush();
		if (!wait) {
			if (!polled) {
				polled = 1;
				continue;
			}
			/* PeekMessage with nothing: the others have a turn, as in Windows */
			if (task_othersready())
				task_yield();
			return 0;
		}
		if (rhead != rtail)
			continue;
		/* Program Manager's first groups, while it waits */
		{
			extern void ddesetup_idle();

			ddesetup_idle();
			if (qhead != qtail)
				continue;
		}
		/* nothing here: another task's turn, while one has something to do */
		if (task_othersready()) {
			if (curtask)
				curtask->t_idle = 1;
			task_yield();
			if (curtask)
				curtask->t_idle = 0;
			continue;
		}
		t = timer_next();
		if (caret.hwnd && caret.shown > 0 && (t < 0 || t > 100))
			t = 100;
		if ((i = mm_next()) >= 0 && (t < 0 || i < t))
			t = i;
		if ((i = ws_next()) >= 0 && (t < 0 || i < t))
			t = i;
		if (scr_poll(&e, t) == 1)
			rawput(&e);
	}
}

/* ---- dispatching ---- */

u32
user_dispatch(a)
	u32 a;
{
	struct wnd *w = wnd_get(GW(a + MSG_HWND));
	u32 msg = GW(a + MSG_MESSAGE), wp = GW(a + MSG_WPARAM), lp = GL(a + MSG_LPARAM);

	if (msg == WM_TIMER && lp)
		return wnd_call(lp, GW(a + MSG_HWND), msg, wp, w16_ticks());
	if (!w)
		return 0;
	if (msg == WM_PAINT) {
		u32 r = wnd_send(w, msg, wp, lp);

		/* a window that did not BeginPaint is done all the same */
		if (wnd_get(w->h) && w->upd.n) {
			w->upd.n = 0;
			w->erase = 0;
		}
		w->flags &= ~WF_INTERNALPAINT;
		return r;
	}
	return wnd_send(w, msg, wp, lp);
}

/* US keyboard: the characters of keys, plain and shifted */
static char *vkplain = "0123456789";
static char *vkshift = ")!@#$%^&*(";

static int
vkchar(vk)
	int vk;
{
	int shift = (keystate[VK_SHIFT] & 0x80) != 0, caps = keystate[VK_CAPITAL] & 1;
	int ctrl = (keystate[VK_CONTROL] & 0x80) != 0;

	if (vk >= 'A' && vk <= 'Z') {
		if (ctrl)
			return vk - 'A' + 1;
		return (shift ^ caps) ? vk : vk + 32;
	}
	if (ctrl) {
		switch (vk) {
		case 0xdb: return 27;
		case 0xdc: return 28;
		case 0xdd: return 29;
		case VK_RETURN: return 10;
		case VK_BACK: return 127;
		}
		return 0;
	}
	if (vk >= '0' && vk <= '9')
		return shift ? vkshift[vk - '0'] : vkplain[vk - '0'];
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9)
		return (keystate[VK_NUMLOCK] & 1) ? '0' + vk - VK_NUMPAD0 : 0;
	switch (vk) {
	case VK_SPACE: return ' ';
	case VK_RETURN: return 13;
	case VK_BACK: return 8;
	case VK_TAB: return 9;
	case VK_ESCAPE: return 27;
	case VK_MULTIPLY: return '*';
	case VK_ADD: return '+';
	case VK_SUBTRACT: return '-';
	case VK_DECIMAL: return (keystate[VK_NUMLOCK] & 1) ? '.' : 0;
	case VK_DIVIDE: return '/';
	case 0xba: return shift ? ':' : ';';
	case 0xbb: return shift ? '+' : '=';
	case 0xbc: return shift ? '<' : ',';
	case 0xbd: return shift ? '_' : '-';
	case 0xbe: return shift ? '>' : '.';
	case 0xbf: return shift ? '?' : '/';
	case 0xc0: return shift ? '~' : '`';
	case 0xdb: return shift ? '{' : '[';
	case 0xdc: return shift ? '|' : '\\';
	case 0xdd: return shift ? '}' : ']';
	case 0xde: return shift ? '"' : '\'';
	}
	return 0;
}

int
user_translate(a)
	u32 a;
{
	u32 msg = GW(a + MSG_MESSAGE), vk = GW(a + MSG_WPARAM);
	int c;

	if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN)
		return 0;
	c = vkchar(vk);
	if (msg == WM_SYSKEYDOWN && vk >= 'A' && vk <= 'Z')
		c = vk + 32;
	if (!c)
		return 0;
	/* ahead of what is queued, as Windows puts it next */
	qhead = (qhead + QSIZE - 1) % QSIZE;
	q[qhead].hwnd = GW(a + MSG_HWND);
	q[qhead].msg = msg == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR;
	q[qhead].wp = c;
	q[qhead].lp = GL(a + MSG_LPARAM);
	q[qhead].time = GL(a + MSG_TIME);
	q[qhead].x = scr_mx;
	q[qhead].y = scr_my;
	return 1;
}

void
user_yield()
{
	/* Yield: any other task with something to do runs (MCIWAVE yields while its playing task finishes) */
	rawfill();
	mm_tick();
	if (task_othersready())
		task_yield();
}

/* a modal loop of our own until (*done & mask) or the window goes; 0 normally, -1 WM_QUIT came */
int
user_modal(dlg, done, mask)
	struct wnd *dlg;
	int *done, mask;
{
	u32 p = ualloc(MSG_SIZE), a = ulin(p);
	int r = 0;
	u16 h = dlg ? dlg->h : 0;

	while ((!h || wnd_get(h)) && !(*done & mask)) {
		if (user_getmessage(a, 0, 0, 0, 1, 1) < 0) {
			/* WM_QUIT: back in the queue for the program's loop */
			if (curtask) {
				curtask->t_quit = 1;
				curtask->t_quitcode = GW(a + MSG_WPARAM);
			}
			r = -1;
			break;
		}
		if (dlg && dlg_ismsg(dlg, a))
			continue;
		user_translate(a);
		user_dispatch(a);
		a = ulin(p);
	}
	ufree(p);
	return r;
}

/* ---- the desktop ---- */

static u32
desk_proc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	struct rgn g;
	struct dc *dc;
	u16 hdc;
	struct rect r;
	int i;

	switch (a[1]) {
	case WM_PAINT:
		rgn_init(&g);
		rgn_copy(&g, &w->upd);
		w->upd.n = 0;
		w->erase = 0;
		hdc = user_dc(w, DCK_WINDOW, &g);
		dc = dc_get(hdc);
		for (i = 0; i < g.n; i++) {
			r = g.r[i];
			d_fill(dc, &r, sys_brush(COLOR_BACKGROUND), 0xf0);
		}
		/* minimized windows' titles under their icons */
		user_releasedc(hdc);
		rgn_free(&g);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	}
	return user_defproc(w, a[1], a[2], a[3]);
}

/* ---- init ---- */

void
user_init()
{
	extern void controls_init(), cursors_init();
	u32 p;

	usel = g_alloc(GMEM_ZEROINIT, 0x10000, 0);
	utop = 0x10;
	readcolors();
	cursors_init();
	controls_init();
	p = thunk_internal(desk_proc, "wwwl", 'l', "DesktopProc");
	cls_register("#32769", CS_GLOBALCLASS, p, 0, 0, 0, 0, cur_arrow, 0, (u32)0, 1);
	desktop = wnd_alloc();
	{
		u32 t = ustr("#32769");

		desktop->cls = cls_find(t, 0);
		ufree(t);
	}
	desktop->proc = p;
	desktop->style = WS_VISIBLE | WS_CLIPCHILDREN;
	desktop->text = strdup("");
	desktop->extra = (u8 *)calloc(1, 8);
	r_set(&desktop->wr, 0, 0, screen.w, screen.h);
	desktop->cr = desktop->wr;
	expose(&desktop->wr);
}

/* a task's end: its windows, timers and messages go */
void
user_taskended(t)
	struct task *t;
{
	struct wnd *w, *n;
	int i, j;

	for (w = desktop->child; w; w = n) {
		n = w->next;
		if (w->task == t && !(w->flags & WF_DESTROYING)) {
			wnd_destroy(w);
			n = desktop->child;	/* the list changed */
		}
	}
	for (i = 0; i < NTIMER; i++)
		if (timers[i].task == t)
			timers[i].used = 0;
	for (i = qhead; i != qtail; )
		if (q[i].task == t) {
			for (j = i; j != qhead; j = (j + QSIZE - 1) % QSIZE)
				q[j] = q[(j + QSIZE - 1) % QSIZE];
			qhead = (qhead + 1) % QSIZE;
			i = qhead;
		} else
			i = (i + 1) % QSIZE;
}

/* ---- for the exports ---- */

void
user_postquit(code)
	int code;
{
	if (curtask) {
		curtask->t_quit = 1;
		curtask->t_quitcode = code;
	}
}

int
user_settimer(h, id, ms, proc)
	u32 h, id, ms, proc;
{
	return timer_set(h, id, ms, proc, 0);
}

int
user_killtimer(h, id)
	u32 h, id;
{
	return timer_kill(h, id);
}

int
user_setsystimer(h, id, ms, proc)
	u32 h, id, ms, proc;
{
	return timer_set(h, id, ms, proc, 1);
}

/* the caret */
int
user_createcaret(h, bm, w, ht)
	u32 h, bm;
	int w, ht;
{
	caret_hide();
	caret.hwnd = h;
	caret.w = w ? w : 1;
	caret.h = ht ? ht : 16;
	caret.x = caret.y = 0;
	caret.shown = 0;
	caret.on = 0;
	caret.bitmap = bm;
	return 1;
}

void
user_destroycaret()
{
	caret_hide();
	caret.hwnd = 0;
}

void
user_setcaretpos(x, y)
	int x, y;
{
	int was = caret.on;

	caret_hide();
	caret.x = x;
	caret.y = y;
	if (was)
		caret_show();
}

u32
user_getcaretpos()
{
	return FP(caret.y, caret.x);
}

void
user_showcaret(h, show)
	u32 h;
	int show;
{
	if (!caret.hwnd || (h && (h & 0xffff) != caret.hwnd))
		return;
	if (show) {
		caret.shown++;
		if (caret.shown > 1)
			caret.shown = 1;
		if (caret.shown == 1)
			caret_show();
	} else {
		caret.shown--;
		caret_hide();
	}
}

int
user_caretwnd()
{
	return caret.hwnd;
}

/* WM_PAINT handling: BeginPaint's work */
u16
user_beginpaint(w, ps)
	struct wnd *w;
	u32 ps;
{
	struct rgn g;
	u16 hdc;
	struct rect r;
	int erase;

	caret_hide();
	if (w->ncpaint)
		user_ncpaint(w);
	rgn_init(&g);
	rgn_copy(&g, &w->upd);
	erase = w->erase;
	w->upd.n = 0;
	w->erase = 0;
	w->flags &= ~WF_INTERNALPAINT;
	hdc = user_dc(w, DCK_WINDOW, &g);
	if (erase)
		erase = !wnd_send(w, WM_ERASEBKGND, hdc, 0);
	if (ps) {
		PW(ps, hdc);
		PW(ps + 2, erase);
		if (g.n) {
			r = g.box;
			r.l -= w->cr.l; r.r -= w->cr.l;
			r.t -= w->cr.t; r.b -= w->cr.t;
		} else
			r_set(&r, 0, 0, 0, 0);
		r_put(ps + 4, &r);
		memset(M + ps + 12, 0, 20);
	}
	rgn_free(&g);
	w->flags |= WF_PAINTING;
	return hdc;
}

void
user_endpaint(w, ps)
	struct wnd *w;
	u32 ps;
{
	if (ps)
		user_releasedc(GW(ps));
	if (w)
		w->flags &= ~WF_PAINTING;
	caret_show();
}

/* UpdateWindow: paint now, frames and all, and the children */
void
wnd_update(w)
	struct wnd *w;
{
	struct wnd *c;

	if (!wnd_visible(w))
		return;
	if (w->ncpaint)
		user_ncpaint(w);
	if (w->upd.n && wnd_get(w->h)) {
		wnd_send(w, WM_PAINT, 0, 0);
		if (wnd_get(w->h) && w->upd.n)
			w->upd.n = 0;
	}
	if (!wnd_get(w->h))
		return;
	for (c = w->child; c; c = c->next)
		wnd_update(c);
}

void
wnd_redrawframe(w)
	struct wnd *w;
{
	w->ncpaint = 1;
	if (wnd_visible(w))
		user_ncpaint(w);
}

int
user_keystate(vk)
	int vk;
{
	return keystate[vk & 0xff];
}

int
user_asyncstate(vk)
	int vk;
{
	return asyncstate[vk & 0xff];
}

/* SetParent: the window keeps its place on the screen relative to the new parent's client area */
void
wnd_reparent(w, p)
	struct wnd *w, *p;
{
	struct rect old = w->wr;
	int vis = wnd_visible(w);

	if (w->parent == p || w == desktop)
		return;
	unlink_w(w);
	offsetall(w, p->cr.l - w->parent->cr.l, p->cr.t - w->parent->cr.t);
	w->parent = p;
	if (p == desktop)
		w->style &= ~WS_CHILD;
	link_w(w, (struct wnd *)0);
	vis_epoch++;
	if (vis)
		expose(&old);
	if (wnd_visible(w))
		expose_in(w, &w->wr, 1);
}

/* paint what needs it now, as a modal loop of ours goes on */
void
user_flushpaint()
{
	struct wnd *w;
	int guard = 0;

	while ((w = needpaint(desktop)) != 0 && guard++ < 1000) {
		if (w->ncpaint) {
			user_ncpaint(w);
			continue;
		}
		wnd_send(w, WM_PAINT, 0, 0);
		if (wnd_get(w->h) && w->upd.n) {
			w->upd.n = 0;
			w->erase = 0;
		}
		w->flags &= ~WF_INTERNALPAINT;
	}
}
