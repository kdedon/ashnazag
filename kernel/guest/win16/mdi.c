/*
 * mdi.c -- the multiple document interface: the MDIClient class, which
 * keeps a frame's document windows, DefFrameProc, DefMDIChildProc and
 * TranslateMDISysAccel, as Windows 3.1 has them.
 *
 * The client lists its children in the order made; the active one has
 * the active caption.  A maximized child fills the client with its
 * caption out of sight: the frame's title gets " - [child]" and its
 * menu bar the child's system menu box and restore button.  The frame's
 * Window menu ends with the children's titles.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "win.h"

#define	MDIS_ALLCHILDSTYLES 0x0001
#define	SC_PREVWINDOW	0xf050
#define	NCHILD		64

struct mdi {
	u16	winmenu, idfirst;
	u16	active;
	int	maxed;			/* the active child is maximized */
	int	n;
	u16	child[NCHILD];
	char	*title;			/* the frame's own title, while a child is maximized */
};

static u32 clientproc;

static struct mdi *
mdiof(c)
	struct wnd *c;
{
	return c && c->cls && c->cls->proc == clientproc ? (struct mdi *)c->priv : 0;
}

static int
indexof(m, h)
	struct mdi *m;
	u16 h;
{
	int i;

	for (i = 0; i < m->n; i++)
		if (m->child[i] == h)
			return i;
	return -1;
}

/* the Window menu's list */
static void
winlist(m)
	struct mdi *m;
{
	extern void menu_mdilist();
	char *t[NCHILD];
	int i, n = 0, chk = -1;
	struct wnd *c;

	if (!m->winmenu)
		return;
	for (i = 0; i < m->n; i++)
		if ((c = wnd_get(m->child[i])) != 0) {
			if (c->h == m->active)
				chk = n;
			t[n++] = c->text;
		}
	menu_mdilist(m->winmenu, m->idfirst, n, t, chk);
}

/* the frame's title: its own, and the maximized child's in brackets */
static void
frametitle(client, m)
	struct wnd *client;
	struct mdi *m;
{
	struct wnd *f = client->parent, *c = wnd_get(m->active);
	char buf[300];

	if (!f || f == client)
		return;
	if (m->maxed && c) {
		if (!m->title)
			m->title = strdup(f->text ? f->text : "");
		sprintf(buf, "%.200s - [%.80s]", m->title, c->text ? c->text : "");
	} else if (m->title) {
		strcpy(buf, m->title);
		free(m->title);
		m->title = 0;
	} else
		return;
	free(f->text);
	f->text = strdup(buf);
	if (wnd_visible(f))
		user_drawnc(f);
}

/* the maximized child, shown in the frame or not */
static void
setmax(client, m, h)
	struct wnd *client;
	struct mdi *m;
	u16 h;
{
	extern void menu_mdimax();

	m->maxed = h != 0;
	if (client->parent && client->parent != client)
		menu_mdimax(client->parent, h);
	frametitle(client, m);
}

static void
activate(client, m, c)
	struct wnd *client;
	struct mdi *m;
	struct wnd *c;
{
	struct wnd *old = wnd_get(m->active);
	int wasmax = m->maxed && old && (old->style & WS_MAXIMIZE);

	if (c == old) {
		if (c && wnd_toplevel(c) == wnd_active && (!wnd_focus || (wnd_focus != c && !IsChildOf(c, wnd_focus))))
			wnd_setfocus(c);
		return;
	}
	m->active = c ? c->h : 0;
	if (old) {
		wnd_send(old, WM_NCACTIVATE, 0, 0);
		wnd_send(old, WM_MDIACTIVATE, 0, FP(old->h, c ? c->h : 0));
		/* a maximized window hands the maximizing on */
		if (wasmax && c && wnd_get(old->h)) {
			m->maxed = 0;
			wnd_show(old, SW_RESTORE);
		}
	}
	if (c) {
		wnd_setpos(c, (struct wnd *)0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		if (wasmax && !(c->style & WS_MAXIMIZE))
			wnd_show(c, SW_MAXIMIZE);
		wnd_send(c, WM_NCACTIVATE, 1, 0);
		wnd_send(c, WM_MDIACTIVATE, 1, FP(old ? old->h : 0, c->h));
		if (wnd_toplevel(c) == wnd_active && !(c->style & WS_MINIMIZE))
			wnd_setfocus(c);
		if (c->style & WS_MAXIMIZE)
			setmax(client, m, c->h);
	} else if (m->maxed)
		setmax(client, m, 0);
	winlist(m);
}

/* the next (or previous) child after h that can be activated */
static struct wnd *
nextof(m, h, prev)
	struct mdi *m;
	u16 h;
	int prev;
{
	int i = indexof(m, h), k, j;
	struct wnd *c;

	for (k = 1; k <= m->n; k++) {
		j = ((i < 0 ? 0 : i) + (prev ? m->n - k : k)) % m->n;
		if ((c = wnd_get(m->child[j])) != 0 && c->h != h && wnd_visible(c) && !(c->style & WS_DISABLED))
			return c;
	}
	return 0;
}

/* ---- arranging ---- */

static int
noniconic(m, list)
	struct mdi *m;
	struct wnd **list;
{
	int i, n = 0;
	struct wnd *c;

	for (i = 0; i < m->n; i++)
		if ((c = wnd_get(m->child[i])) != 0 && wnd_visible(c) && !(c->style & WS_MINIMIZE))
			list[n++] = c;
	return n;
}

static int
iconarrange(client, m)
	struct wnd *client;
	struct mdi *m;
{
	int i, x = 0, n = 0, ch = client->cr.b - client->cr.t, cw = client->cr.r - client->cr.l;
	struct wnd *c;

	for (i = 0; i < m->n; i++)
		if ((c = wnd_get(m->child[i])) != 0 && (c->style & WS_MINIMIZE)) {
			if (x + 76 > cw && x > 0)
				x = 0;
			wnd_setpos(c, (struct wnd *)0, x + 20, ch - 54, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			x += 76;
			n++;
		}
	return n ? 60 : 0;	/* the height the icons take */
}

static void
cascade(client, m)
	struct wnd *client;
	struct mdi *m;
{
	struct wnd *list[NCHILD];
	int n, i, step = sys_metric(SM_CYCAPTION) + sys_metric(SM_CXFRAME) - 1, cw, ch, w, h, ih;

	if (m->maxed && wnd_get(m->active))
		wnd_show(wnd_get(m->active), SW_RESTORE);
	ih = iconarrange(client, m);
	n = noniconic(m, list);
	cw = client->cr.r - client->cr.l;
	ch = client->cr.b - client->cr.t - ih;
	w = cw - step * (n - 1);
	h = ch - step * (n - 1);
	if (w < cw * 2 / 3)
		w = cw * 2 / 3;
	if (h < ch * 2 / 3)
		h = ch * 2 / 3;
	/* the active one last, on top */
	for (i = n - 1; i >= 0; i--)
		wnd_setpos(list[i], (struct wnd *)0, step * (n - 1 - i), step * (n - 1 - i), w, h, SWP_NOACTIVATE);
}

static void
tile(client, m, horz)
	struct wnd *client;
	struct mdi *m;
	int horz;
{
	struct wnd *list[NCHILD];
	int n, i, cols, rows, col, k, x, y, cw, ch, ih, inthis;

	if (m->maxed && wnd_get(m->active))
		wnd_show(wnd_get(m->active), SW_RESTORE);
	ih = iconarrange(client, m);
	n = noniconic(m, list);
	if (!n)
		return;
	cw = client->cr.r - client->cr.l;
	ch = client->cr.b - client->cr.t - ih;
	if (horz) {
		for (i = 0; i < n; i++)
			wnd_setpos(list[i], (struct wnd *)0, 0, ch * i / n, cw, ch * (i + 1) / n - ch * i / n,
			    SWP_NOACTIVATE | SWP_NOZORDER);
		return;
	}
	for (cols = 1; cols * cols < n; cols++)
		;
	rows = n / cols;
	for (i = 0, col = 0; col < cols; col++) {
		/* the last columns take the windows left over */
		inthis = rows + (col >= cols - n % cols ? 1 : 0);
		if (n % cols == 0)
			inthis = rows;
		x = cw * col / cols;
		for (k = 0; k < inthis && i < n; k++, i++) {
			y = ch * k / inthis;
			wnd_setpos(list[i], (struct wnd *)0, x, y, cw * (col + 1) / cols - x, ch * (k + 1) / inthis - y,
			    SWP_NOACTIVATE | SWP_NOZORDER);
		}
	}
}

/* ---- the MDIClient class ---- */

static struct wnd *
create(client, m, cs, fp)
	struct wnd *client;
	struct mdi *m;
	u32 cs, fp;		/* MDICREATESTRUCT, linear and far */
{
	u32 style = GL(cs + 18);
	int x = (short)GW(cs + 10), y = (short)GW(cs + 12), cx = (short)GW(cs + 14), cy = (short)GW(cs + 16);
	int cw = client->cr.r - client->cr.l, ch = client->cr.b - client->cr.t, step;
	struct wnd *c, *old = wnd_get(m->active);
	static int pos;

	if (m->n >= NCHILD)
		return 0;
	if (!(client->style & MDIS_ALLCHILDSTYLES))
		style |= WS_CHILD | WS_CLIPSIBLINGS | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX |
		    WS_MAXIMIZEBOX;
	style |= WS_CHILD | WS_VISIBLE;
	style &= ~WS_POPUP;
	step = sys_metric(SM_CYCAPTION) + sys_metric(SM_CXFRAME) - 1;
	if (x == CW_USEDEFAULT || y == CW_USEDEFAULT) {
		if ((pos + 4) * step > ch / 2)
			pos = 0;
		x = y = pos++ * step;
	}
	if (cx == CW_USEDEFAULT || cy == CW_USEDEFAULT || cx <= 0 || cy <= 0) {
		cx = cw * 3 / 4;
		cy = ch * 3 / 4;
	}
	/* a new window comes maximized when the active one is */
	if (m->maxed && old)
		style |= WS_MAXIMIZE;
	c = wnd_create(WS_EX_MDICHILD, GL(cs), GL(cs + 4), style & ~(WS_MAXIMIZE | WS_MINIMIZE), x, y, cx, cy,
	    (u32)client->h, (u32)(m->idfirst + m->n), (u32)GW(cs + 8), fp);
	if (!c)
		return 0;
	m->child[m->n++] = c->h;
	if (style & WS_MINIMIZE)
		wnd_show(c, SW_MINIMIZE);
	activate(client, m, c);
	if (style & WS_MAXIMIZE)
		wnd_show(c, SW_MAXIMIZE);
	winlist(m);
	return c;
}

static void
destroy(client, m, c)
	struct wnd *client;
	struct mdi *m;
	struct wnd *c;
{
	int i = indexof(m, c->h), j;
	struct wnd *n;

	if (c->h == m->active) {
		n = nextof(m, c->h, 1);
		if (m->maxed && n && !(n->style & WS_MAXIMIZE)) {
			m->maxed = 0;
			setmax(client, m, 0);
			wnd_show(n, SW_MAXIMIZE);
			m->active = 0;
			activate(client, m, n);
		} else {
			if (m->maxed)
				setmax(client, m, 0);
			m->active = 0;
			activate(client, m, n);
		}
	}
	if (i >= 0) {
		for (j = i; j < m->n - 1; j++)
			m->child[j] = m->child[j + 1];
		m->n--;
	}
	/* the ids stay in a row */
	for (j = 0; j < m->n; j++)
		if ((n = wnd_get(m->child[j])) != 0)
			n->id = m->idfirst + j;
	wnd_destroy(c);
	winlist(m);
	if (!m->n && client->parent)
		wnd_setfocus(client);
}

u32
mdiclient_proc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]), *c;
	struct mdi *m;
	u32 lp = a[3], p;

	if (!w)
		return 0;
	m = (struct mdi *)w->priv;
	switch (a[1]) {
	case WM_NCCREATE:
		/* the scroll bars come and go with the children's extent in Windows; none here yet */
		w->style &= ~(WS_HSCROLL | WS_VSCROLL);
		break;
	case WM_CREATE:
		m = (struct mdi *)calloc(1, sizeof *m);
		w->priv = (void *)m;
		p = lin(FPSEL(lp), FPOFF(lp));
		/* CREATESTRUCT's lpCreateParams: a CLIENTCREATESTRUCT */
		if (p && (p = lin(FPSEL(GL(p)), FPOFF(GL(p)))) != 0) {
			m->winmenu = GW(p);
			m->idfirst = GW(p + 2);
		}
		return 0;
	case WM_DESTROY:
		if (m) {
			if (m->maxed)
				setmax(w, m, 0);
			free(m->title);
			free((char *)m);
			w->priv = 0;
		}
		return 0;
	}
	if (!m)
		return user_defproc(w, a[1], a[2], lp);
	switch (a[1]) {
	case WM_MDICREATE:
		p = lin(FPSEL(lp), FPOFF(lp));
		c = p ? create(w, m, p, lp) : 0;
		return c ? c->h : 0;
	case WM_MDIDESTROY:
		if ((c = wnd_get(a[2])) != 0 && indexof(m, c->h) >= 0)
			destroy(w, m, c);
		return 0;
	case WM_MDIACTIVATE:
		if ((c = wnd_get(a[2])) != 0 && indexof(m, c->h) >= 0)
			activate(w, m, c);
		return 0;
	case WM_MDIGETACTIVE:
		return FP(m->maxed ? 1 : 0, m->active);
	case WM_MDINEXT:
		c = nextof(m, a[2] ? (u16)a[2] : m->active, LO16(lp) != 0);
		if (c) {
			struct wnd *cur = wnd_get(m->active);

			activate(w, m, c);
			/* the one left goes to the bottom */
			if (cur && !LO16(lp) && !m->maxed)
				wnd_setpos(cur, (struct wnd *)1, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
		return 0;
	case WM_MDIMAXIMIZE:
		if ((c = wnd_get(a[2])) != 0)
			wnd_show(c, SW_MAXIMIZE);
		return 0;
	case WM_MDIRESTORE:
		if ((c = wnd_get(a[2])) != 0)
			wnd_show(c, SW_RESTORE);
		return 0;
	case WM_MDITILE:
		tile(w, m, a[2] & 1);
		return 1;
	case WM_MDICASCADE:
		cascade(w, m);
		return 1;
	case WM_MDIICONARRANGE:
		iconarrange(w, m);
		return 0;
	case WM_MDISETMENU:
		{
			struct wnd *f = w->parent;
			u16 old = f ? f->id : 0;

			if (HI16(lp) && HI16(lp) != m->winmenu) {
				u16 was = m->winmenu;

				if (was) {
					extern void menu_mdilist();

					menu_mdilist(was, m->idfirst, 0, (char **)0, -1);
				}
				m->winmenu = HI16(lp);
			}
			if (LO16(lp) && f && !(f->style & WS_CHILD)) {
				f->id = LO16(lp);
				f->flags |= WF_MENUOWNED;
			}
			winlist(m);
			if (f && LO16(lp))
				wnd_setpos(f, (struct wnd *)0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
				    SWP_NOACTIVATE | SWP_FRAMECHANGED);
			return old;
		}
	case WM_SIZE:
		if (m->maxed && (c = wnd_get(m->active)) != 0) {
			/* the maximized child follows the client's size */
			c->style &= ~WS_MAXIMIZE;
			wnd_show(c, SW_MAXIMIZE);
		}
		break;
	case WM_SETFOCUS:
		if ((c = wnd_get(m->active)) != 0 && !(c->style & WS_MINIMIZE))
			wnd_setfocus(c);
		return 0;
	}
	return user_defproc(w, a[1], a[2], lp);
}

/* ---- the frame's and the children's default procedures ---- */

/* DefFrameProc(hwnd, hwndMDIClient, msg, wParam, lParam) */
static u32
u_DefFrameProc(a)
	u32 *a;
{
	struct wnd *f = wnd_get(a[0]), *client = wnd_get(a[1]), *c;
	struct mdi *m = mdiof(client);
	u32 msg = a[2], wp = a[3], lp = a[4];

	if (!f)
		return 0;
	if (m)
		switch (msg) {
		case WM_COMMAND:
			if (wp >= m->idfirst && wp < (u32)m->idfirst + m->n) {
				if ((c = wnd_get(m->child[wp - m->idfirst])) != 0) {
					if (c->style & WS_MINIMIZE)
						wnd_show(c, SW_RESTORE);
					activate(client, m, c);
				}
				return 0;
			}
			/* the maximized child's system menu, in the frame's bar */
			if (wp >= 0xf000 && m->maxed && (c = wnd_get(m->active)) != 0)
				return wnd_send(c, WM_SYSCOMMAND, wp, lp);
			break;
		case WM_NCACTIVATE:
			if ((c = wnd_get(m->active)) != 0)
				wnd_send(c, WM_NCACTIVATE, wp, 0);
			break;
		case WM_SETFOCUS:
			if ((c = wnd_get(m->active)) != 0 && !(c->style & WS_MINIMIZE))
				wnd_setfocus(c);
			else
				wnd_setfocus(client);
			return 0;
		case WM_SIZE:
			if (wp != 1)	/* SIZE_MINIMIZED */
				wnd_setpos(client, (struct wnd *)0, 0, 0, LO16(lp), HI16(lp), SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		case WM_SETTEXT:
			if (m->maxed && m->title) {
				free(m->title);
				m->title = strdup(lp ? gptr(lp) : "");
				frametitle(client, m);
				return 1;
			}
			break;
		case WM_MENUCHAR:
			/* Alt+- : the active child's system menu */
			if (LO16(wp) == '-' && (c = wnd_get(m->active)) != 0) {
				wnd_send(c, WM_SYSCOMMAND, SC_KEYMENU, '-');
				return FP(1, 0);
			}
			break;
		}
	return user_defproc(f, msg, wp, lp);
}

static u32
u_DefMDIChildProc(a)
	u32 *a;
{
	struct wnd *c = wnd_get(a[0]), *client;
	struct mdi *m;
	u32 msg = a[1], wp = a[2], lp = a[3], r;

	if (!c)
		return 0;
	client = c->parent;
	m = mdiof(client);
	if (!m)
		return user_defproc(c, msg, wp, lp);
	switch (msg) {
	case WM_SETTEXT:
		r = user_defproc(c, msg, wp, lp);
		winlist(m);
		if (m->maxed && m->active == c->h)
			frametitle(client, m);
		return r;
	case WM_CLOSE:
		wnd_send(client, WM_MDIDESTROY, c->h, 0);
		return 0;
	case WM_CHILDACTIVATE:
		activate(client, m, c);
		return 0;
	case WM_SETFOCUS:
		if (m->active != c->h)
			activate(client, m, c);
		break;
	case WM_SYSCOMMAND:
		switch (wp & 0xfff0) {
		case SC_NEXTWINDOW:
		case SC_PREVWINDOW:
			wnd_send(client, WM_MDINEXT, c->h, (wp & 0xfff0) == SC_PREVWINDOW);
			return 0;
		case SC_KEYMENU:
			/* Alt alone and Alt+letters go to the frame's bar */
			if (LO16(lp) != '-' && client->parent)
				return wnd_send(client->parent, WM_SYSCOMMAND, wp, lp);
			break;
		}
		break;
	case WM_SIZE:
		r = user_defproc(c, msg, wp, lp);
		if (wp == 2) {			/* SIZE_MAXIMIZED */
			if (m->active != c->h)
				activate(client, m, c);
			setmax(client, m, c->h);
		} else if (m->maxed && m->active == c->h)
			setmax(client, m, 0);
		return r;
	case WM_MENUCHAR:
		if (LO16(wp) == '-')
			return FP(1, 0);
		if (client->parent)
			return wnd_send(client->parent, WM_MENUCHAR, wp, lp);
		break;
	}
	return user_defproc(c, msg, wp, lp);
}

/* TranslateMDISysAccel(hwndClient, lpMsg): Ctrl+F4 closes, Ctrl+F6 and Ctrl+Tab go to the next */
static u32
u_TranslateMDISysAccel(a)
	u32 *a;
{
	extern u8 keystate[];
	struct wnd *client = wnd_get(a[0]), *c;
	struct mdi *m = mdiof(client);
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	int msg, vk, cmd;

	if (!m || !p || !(c = wnd_get(m->active)) || (c->style & WS_DISABLED))
		return 0;
	msg = GW(p + 2);
	vk = GW(p + 4);
	if ((msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) || !(keystate[VK_CONTROL] & 0x80) ||
	    (keystate[VK_MENU] & 0x80))
		return 0;
	if (vk == VK_F1 + 3)
		cmd = SC_CLOSE;
	else if (vk == VK_F1 + 5 || vk == VK_TAB)
		cmd = (keystate[VK_SHIFT] & 0x80) ? SC_PREVWINDOW : SC_NEXTWINDOW;
	else
		return 0;
	wnd_send(c, WM_SYSCOMMAND, cmd, FP(0, vk));
	return 1;
}

/* a click on a maximized child's box (1) or restore button (2) in the frame's bar */
void
mdi_barbutton(f, k, lp)
	struct wnd *f;
	int k;
	u32 lp;
{
	struct wnd *client, *c = 0;
	struct mdi *m = 0;

	for (client = f->child; client; client = client->next)
		if ((m = mdiof(client)) != 0)
			break;
	if (!m || !(c = wnd_get(m->active)))
		return;
	if (k == 2)
		wnd_send(c, WM_SYSCOMMAND, SC_RESTORE, lp);
	else
		wnd_send(c, WM_SYSCOMMAND, SC_MOUSEMENU, lp);
}

/* the click activating a document window */
void
mdi_click(w)
	struct wnd *w;
{
	for (; w && w->parent; w = w->parent)
		if ((w->exstyle & WS_EX_MDICHILD) && mdiof(w->parent)) {
			if (mdiof(w->parent)->active != w->h)
				wnd_send(w, WM_CHILDACTIVATE, 0, 0);
			return;
		}
}

void
mdi_init()
{
	clientproc = thunk_internal(mdiclient_proc, "wwwl", 'l', "MDIClientWndProc");
	cls_register("MDIClient", CS_GLOBALCLASS, clientproc, 0, 0, 0, 0, cur_arrow, COLOR_APPWORKSPACE + 1,
	    (u32)0, 1);
}

/* CalcChildScroll(hwnd, sb), ScrollChildren: the client's scroll bars, which it does not show */
static u32 u_CalcChildScroll(a) u32 *a; { return 0; }
static u32 u_ScrollChildren(a) u32 *a; { return 0; }

struct impl md_impl[] = {
	{ "USER", "CalcChildScroll", u_CalcChildScroll },
	{ "USER", "ScrollChildren", u_ScrollChildren },
	{ "USER", "DefFrameProc", u_DefFrameProc },
	{ "USER", "DefMDIChildProc", u_DefMDIChildProc },
	{ "USER", "TranslateMDISysAccel", u_TranslateMDISysAccel },
	{ 0, 0, 0 }
};
