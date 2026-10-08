/*
 * menu.c -- menus: menu bars, popup menus (each a window of its own
 * while open), the system menu, and the loop that tracks them with the
 * mouse and the keyboard, sending WM_INITMENU, WM_INITMENUPOPUP,
 * WM_MENUSELECT and finally WM_COMMAND or WM_SYSCOMMAND.
 */

#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "scr.h"

extern int user_poll();

extern u32 ualloc(), ulin(), ustr(), thunk_internal();
extern void ufree(), user_flushpaint();

#define	MF_GRAYED	0x0001
#define	MF_DISABLED	0x0002
#define	MF_BITMAP	0x0004
#define	MF_CHECKED	0x0008
#define	MF_POPUP	0x0010
#define	MF_MENUBARBREAK	0x0020
#define	MF_MENUBREAK	0x0040
#define	MF_HILITE	0x0080
#define	MF_END		0x0080
#define	MF_OWNERDRAW	0x0100
#define	MF_BYPOSITION	0x0400
#define	MF_SEPARATOR	0x0800
#define	MF_CHANGE	0x0080
#define	MF_APPEND	0x0100
#define	MF_DELETE	0x0200
#define	MF_REMOVE	0x1000

#define	NMENU	512
#define	MBASE	0xa000

struct item {
	u16	flags;
	u16	id;		/* or the popup's handle */
	char	*text;
	u32	data;		/* bitmap or owner-draw data */
	struct rect r;		/* laid out: in the bar (window coordinates) or the popup (client) */
};

struct menu {
	u16	h;
	int	n, max;
	struct item *it;
	int	sys;		/* a system menu */
	int	popupw, popuph;
};

static struct menu *menus[NMENU];
static u32 popupproc;

static struct menu *
mget(h)
	u32 h;
{
	int i;

	h &= 0xffff;
	if (h < MBASE || (h - MBASE) & 3)
		return 0;
	i = (h - MBASE) >> 2;
	return i < NMENU ? menus[i] : 0;
}

static u16
mnew()
{
	int i;

	for (i = 1; i < NMENU; i++)
		if (!menus[i]) {
			menus[i] = (struct menu *)calloc(1, sizeof(struct menu));
			menus[i]->h = MBASE + 4 * i;
			return menus[i]->h;
		}
	return 0;
}

static void
mfree(h)
	u32 h;
{
	struct menu *m = mget(h);
	int i;

	if (!m)
		return;
	for (i = 0; i < m->n; i++) {
		if (m->it[i].flags & MF_POPUP)
			mfree(m->it[i].id);
		if (m->it[i].text)
			free(m->it[i].text);
	}
	free(m->it);
	menus[(m->h - MBASE) >> 2] = 0;
	free(m);
}

void
menu_destroy(h)
	u32 h;
{
	mfree(h);
}

static int
mins(m, pos, flags, id, text, data)
	struct menu *m;
	int pos, flags;
	u32 id, data;
	char *text;
{
	struct item *it;

	if (m->n == m->max) {
		m->max = m->max ? m->max * 2 : 8;
		m->it = (struct item *)realloc(m->it, m->max * sizeof *m->it);
	}
	if (pos < 0 || pos > m->n)
		pos = m->n;
	memmove(&m->it[pos + 1], &m->it[pos], (m->n - pos) * sizeof *m->it);
	m->n++;
	it = &m->it[pos];
	memset(it, 0, sizeof *it);
	it->flags = flags & ~(MF_BYPOSITION | MF_CHANGE | MF_APPEND | MF_DELETE | MF_REMOVE | 0x8000);
	it->id = id;
	it->text = text && !(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) ? strdup(text) : 0;
	it->data = data;
	if (!it->text && !(flags & (MF_BITMAP | MF_OWNERDRAW)) && !(flags & MF_SEPARATOR) && !text)
		it->flags |= MF_SEPARATOR;
	return 1;
}

/* the item by command (searching popups too) or position: its menu and index */
static struct menu *
mfind(m, what, flags, ip)
	struct menu *m;
	u32 what;
	int flags, *ip;
{
	int i;
	struct menu *s;

	if (!m)
		return 0;
	if (flags & MF_BYPOSITION) {
		if ((int)what >= m->n)
			return 0;
		*ip = what;
		return m;
	}
	for (i = 0; i < m->n; i++) {
		if (!(m->it[i].flags & MF_POPUP) && m->it[i].id == (what & 0xffff)) {
			*ip = i;
			return m;
		}
		if ((m->it[i].flags & MF_POPUP) && (s = mfind(mget(m->it[i].id), what, flags, ip)) != 0)
			return s;
	}
	for (i = 0; i < m->n; i++)
		if ((m->it[i].flags & MF_POPUP) && m->it[i].id == (what & 0xffff)) {
			*ip = i;
			return m;
		}
	return 0;
}

/* ---- templates ---- */

static u32
parse(m, p, end)
	struct menu *m;
	u32 p, end;
{
	int flags, last;
	u32 id;
	char *text;
	u16 sub;

	do {
		if (p + 2 > end)
			break;
		flags = GW(p);
		p += 2;
		last = flags & MF_END;
		if (flags & MF_POPUP) {
			text = (char *)M + p;
			p += strlen(text) + 1;
			sub = mnew();
			p = parse(mget(sub), p, end);
			mins(m, -1, (flags & ~MF_END) | MF_POPUP, sub, text, 0);
		} else {
			id = GW(p);
			p += 2;
			text = (char *)M + p;
			p += strlen(text) + 1;
			mins(m, -1, flags & ~MF_END, id, *text || id ? text : (char *)0, 0);
			if (!*text && !id)
				m->it[m->n - 1].flags |= MF_SEPARATOR;
		}
	} while (!last);
	return p;
}

u16
menu_fromtemplate(p)
	u32 p;
{
	u16 h = mnew();

	parse(mget(h), p + 4 + GW(p + 2), p + 0x10000);
	return h;
}

u16
menu_load(hinst, name)
	u32 hinst, name;
{
	struct module *mod = mod_byhandle(hinst);
	u32 d, size;

	if (!mod)
		return 0;
	d = res_data(mod, FP(0, RT_MENU), name, &size);
	if (!d)
		return 0;
	return menu_fromtemplate(d);
}

u16
sysmenu_of(w)
	struct wnd *w;
{
	struct menu *m;

	if (w->sysmenu && mget(w->sysmenu))
		return w->sysmenu;
	w->sysmenu = mnew();
	m = mget(w->sysmenu);
	m->sys = 1;
	mins(m, -1, 0, SC_RESTORE, "&Restore", 0);
	mins(m, -1, 0, SC_MOVE, "&Move", 0);
	mins(m, -1, 0, SC_SIZE, "&Size", 0);
	mins(m, -1, 0, SC_MINIMIZE, "Mi&nimize", 0);
	mins(m, -1, 0, SC_MAXIMIZE, "Ma&ximize", 0);
	mins(m, -1, MF_SEPARATOR, 0, (char *)0, 0);
	mins(m, -1, 0, SC_CLOSE, "&Close\tAlt+F4", 0);
	return w->sysmenu;
}

/* grey what does not apply to the window now */
static void
sysmenu_update(w, m)
	struct wnd *w;
	struct menu *m;
{
	int i;

	for (i = 0; i < m->n; i++) {
		int g = 0;

		switch (m->it[i].id) {
		case SC_RESTORE: g = !(w->style & (WS_MINIMIZE | WS_MAXIMIZE)); break;
		case SC_MOVE: g = (w->style & WS_MAXIMIZE) != 0; break;
		case SC_SIZE: g = !(w->style & WS_THICKFRAME) || (w->style & (WS_MINIMIZE | WS_MAXIMIZE)); break;
		case SC_MINIMIZE: g = !(w->style & WS_MINIMIZEBOX) || (w->style & WS_MINIMIZE); break;
		case SC_MAXIMIZE: g = !(w->style & WS_MAXIMIZEBOX) || (w->style & WS_MAXIMIZE); break;
		default: continue;
		}
		if (g)
			m->it[i].flags |= MF_GRAYED;
		else
			m->it[i].flags &= ~MF_GRAYED;
	}
}

/* ---- drawing ---- */

static struct bfont *
mfont()
{
	return gobj(stockobj[SYSTEM_FONT], OBJ_FONT)->u.font.bf;
}

static int
itemw(it)
	struct item *it;
{
	char *t = it->text, *tab;

	if (!t)
		return it->flags & MF_SEPARATOR ? 0 : 16;
	tab = strchr(t, '\t');
	return draw_textw(mfont(), t, tab ? tab - t : strlen(t)) - (strchr(t, '&') ? draw_textw(mfont(), "&", 1) : 0);
}

/* lay the bar out in w; its height */
static int
barlayout(w, m)
	struct wnd *w;
	struct menu *m;
{
	int b = frame_width(w), x, y, h = sys_metric(SM_CYMENU), i, iw, left, right;

	left = w->wr.l + b;
	right = w->wr.r - b;
	y = w->wr.t + b + ((w->style & WS_CAPTION) == WS_CAPTION ? sys_metric(SM_CYCAPTION) - 1 : 0);
	if ((w->style & WS_CAPTION) == WS_CAPTION && !(w->style & WS_THICKFRAME) &&
	    !(w->exstyle & WS_EX_DLGMODALFRAME))
		y = w->wr.t + 1 + sys_metric(SM_CYCAPTION) - 1;
	x = left;
	for (i = 0; i < m->n; i++) {
		iw = itemw(&m->it[i]) + 16;
		if ((x + iw > right && x > left) || (m->it[i].flags & (MF_MENUBREAK | MF_MENUBARBREAK))) {
			x = left;
			y += h;
		}
		r_set(&m->it[i].r, x, y, x + iw, y + h);
		x += iw;
	}
	return m->n ? m->it[m->n - 1].r.b - (w->wr.t + b + ((w->style & WS_CAPTION) == WS_CAPTION ?
	    sys_metric(SM_CYCAPTION) - 1 : 0)) + 1 : h + 1;
}

int
menu_barheight(w)
	struct wnd *w;
{
	struct menu *m = mget(w->id);

	if (!m || (w->style & WS_CHILD) || (w->style & WS_MINIMIZE))
		return 0;
	return barlayout(w, m);
}

static void
drawitem(dc, it, r, sel, bar)
	struct dc *dc;
	struct item *it;
	struct rect *r;
	int sel, bar;
{
	COLORREF kt = dc->st.text, kb = dc->st.bk;
	int km = dc->st.bkmode, kf = dc->st.font;
	struct rect t;
	char *tab;
	int gray = it->flags & (MF_GRAYED | MF_DISABLED);

	if (it->flags & MF_SEPARATOR) {
		d_fillcolor(dc, r, pal_index(sys_color(COLOR_MENU)));
		r_set(&t, r->l, (r->t + r->b) / 2, r->r, (r->t + r->b) / 2 + 1);
		d_fillcolor(dc, &t, 0);
		return;
	}
	d_fillcolor(dc, r, pal_index(sys_color(sel ? COLOR_HIGHLIGHT : COLOR_MENU)));
	dc->st.text = sys_color(gray ? COLOR_GRAYTEXT : sel ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT);
	dc->st.bkmode = TRANSPARENT;
	dc->st.font = stockobj[SYSTEM_FONT];
	if (it->flags & MF_BITMAP) {
		struct gobj *o = gobj(LO16(it->data ? it->data : it->text ? 0 : 0), OBJ_BITMAP);

		(void)o;
	} else if (it->text) {
		tab = strchr(it->text, '\t');
		t = *r;
		t.l += bar ? 8 : 18;
		draw_text(dc, it->text, tab ? tab - it->text : strlen(it->text), &t, DT_SINGLELINE | DT_VCENTER);
		if (tab) {
			t.r -= 12;
			draw_text(dc, tab + 1, strlen(tab + 1), &t, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
		}
	}
	if (!bar && (it->flags & MF_CHECKED)) {
		int cx = r->l + 8, cy = (r->t + r->b) / 2, i, idx = dc->s->mono ? 0 : pal_index(dc->st.text);
		struct rect c;

		for (i = 0; i < 3; i++) {
			r_set(&c, cx - 3 + i, cy + i - 1, cx - 2 + i, cy + i + 1);
			d_fillcolor(dc, &c, idx);
		}
		for (i = 0; i < 5; i++) {
			r_set(&c, cx + i, cy + 1 - i, cx + i + 1, cy + 3 - i);
			d_fillcolor(dc, &c, idx);
		}
	}
	if (!bar && (it->flags & MF_POPUP))
		draw_arrow(dc, r->r - 8, (r->t + r->b) / 2, 4, 3, pal_index(dc->st.text));
	dc->st.text = kt;
	dc->st.bk = kb;
	dc->st.bkmode = km;
	dc->st.font = kf;
}

static int barsel = -1;
static struct wnd *barwnd;

void
menu_drawbar(w, dc)
	struct wnd *w;
	struct dc *dc;
{
	struct menu *m = mget(w->id);
	int i, top, bottom;
	struct rect r;

	if (!m)
		return;
	barlayout(w, m);
	top = m->n ? m->it[0].r.t : w->cr.t - menu_barheight(w);
	bottom = w->cr.t - 1;
	r_set(&r, w->wr.l + frame_width(w), top, w->wr.r - frame_width(w), bottom);
	d_fillcolor(dc, &r, pal_index(sys_color(COLOR_MENU)));
	for (i = 0; i < m->n; i++)
		drawitem(dc, &m->it[i], &m->it[i].r, barwnd == w && i == barsel, 1);
	r_set(&r, w->wr.l + frame_width(w), bottom, w->wr.r - frame_width(w), bottom + 1);
	d_fillcolor(dc, &r, 0);
}

int
menu_barhit(w, x, y)
	struct wnd *w;
	int x, y;
{
	struct menu *m = mget(w->id);
	int i;

	if (!m)
		return -1;
	barlayout(w, m);
	for (i = 0; i < m->n; i++)
		if (x >= m->it[i].r.l && x < m->it[i].r.r && y >= m->it[i].r.t && y < m->it[i].r.b)
			return i;
	return -1;
}

static void
redrawbar(w)
	struct wnd *w;
{
	u16 hdc = user_dc(w, DCK_WINDOWNC, (struct rgn *)0);
	struct dc *dc = dc_get(hdc);

	menu_drawbar(w, dc);
	user_releasedc(hdc);
}

/* ---- popups ---- */

#define	ITEMH	(mfont()->f_height + 4)

static void
poplayout(m)
	struct menu *m;
{
	int i, y = 1, w = 80, iw, accw = 0;
	char *tab;

	for (i = 0; i < m->n; i++) {
		iw = itemw(&m->it[i]) + 36;
		if (m->it[i].text && (tab = strchr(m->it[i].text, '\t')) != 0)
			if (draw_textw(mfont(), tab + 1, strlen(tab + 1)) + 24 > accw)
				accw = draw_textw(mfont(), tab + 1, strlen(tab + 1)) + 24;
		if (iw > w)
			w = iw;
	}
	w += accw;
	for (i = 0; i < m->n; i++) {
		int h = (m->it[i].flags & MF_SEPARATOR) ? 8 : ITEMH;

		r_set(&m->it[i].r, 1, y, w - 1, y + h);
		y += h;
	}
	m->popupw = w;
	m->popuph = y + 1;
}

static void
drawpopup(pw, m, sel)
	struct wnd *pw;
	struct menu *m;
	int sel;
{
	u16 hdc = user_dc(pw, DCK_WINDOW, (struct rgn *)0);
	struct dc *dc = dc_get(hdc);
	struct rect r;
	int i;

	r = pw->cr;
	d_frame(dc, &r, 0);
	for (i = 0; i < m->n; i++) {
		r = m->it[i].r;
		r.l += pw->cr.l; r.r += pw->cr.l; r.t += pw->cr.t; r.b += pw->cr.t;
		drawitem(dc, &m->it[i], &r, i == sel, 0);
	}
	user_releasedc(hdc);
}

u32
menu_popupproc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);

	if (!w)
		return 0;
	switch (a[1]) {
	case WM_PAINT:
		{
			extern u16 user_beginpaint();
			extern void user_endpaint();
			u16 hdc = user_beginpaint(w, 0);

			user_releasedc(hdc);
			user_endpaint(w, 0);
			if (mget(w->user))
				drawpopup(w, mget(w->user), (int)(short)w->id);
		}
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_NCHITTEST:
		return HTCLIENT;
	case WM_MOUSEACTIVATE:
		return 3;	/* MA_NOACTIVATE */
	}
	return user_defproc(w, a[1], a[2], a[3]);
}

/* ---- tracking ---- */

struct level {
	struct menu *m;
	struct wnd *w;		/* its window; 0 for the bar */
	int sel;
};

static struct level lv[8];
static int nlv;
static struct wnd *owner;
static int insys;

static void
select_(k, i)
	int k, i;
{
	struct level *l = &lv[k];
	struct item *it;

	if (l->sel == i)
		return;
	l->sel = i;
	if (l->w) {
		l->w->id = i;
		drawpopup(l->w, l->m, i);
	} else {
		barsel = i;
		redrawbar(barwnd);
	}
	it = i >= 0 ? &l->m->it[i] : 0;
	if (owner)
		wnd_send(owner, WM_MENUSELECT, it ? it->id : 0,
		    it ? FP(l->m->h, it->flags | (insys ? 0x2000 : 0)) : FP(0, 0xffff));
}

static void
closeto(k)
	int k;
{
	while (nlv > k) {
		nlv--;
		if (lv[nlv].w)
			wnd_destroy(lv[nlv].w);
		lv[nlv].w = 0;
	}
	user_flushpaint();
}

/* open the popup of item i of level k, below the bar item or right of the popup item */
static void
openpopup(k, i)
	int k, i;
{
	struct item *it = &lv[k].m->it[i];
	struct menu *sub;
	int x, y;
	struct wnd *pw;
	u32 cls;

	if (!(it->flags & MF_POPUP) || (it->flags & (MF_GRAYED | MF_DISABLED)) || nlv >= 8)
		return;
	if (!(sub = mget(it->id)))
		return;
	closeto(k + 1);
	if (owner)
		wnd_send(owner, WM_INITMENUPOPUP, sub->h, FP(insys, i));
	poplayout(sub);
	if (lv[k].w) {
		x = lv[k].w->cr.l + it->r.r - 4;
		y = lv[k].w->cr.t + it->r.t - 1;
	} else {
		x = it->r.l;
		y = it->r.b;
	}
	if (x + sub->popupw > screen.w)
		x = screen.w - sub->popupw;
	if (y + sub->popuph > screen.h)
		y = screen.h - sub->popuph;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	cls = ustr("#32768");
	pw = wnd_create(0, cls, 0, WS_POPUP, x, y, sub->popupw, sub->popuph, owner ? owner->h : 0, 0, 0, 0);
	ufree(cls);
	if (!pw)
		return;
	pw->user = sub->h;
	pw->id = 0xffff;
	wnd_show(pw, SW_SHOWNA);
	lv[nlv].m = sub;
	lv[nlv].w = pw;
	lv[nlv].sel = -1;
	nlv++;
	user_flushpaint();
	drawpopup(pw, sub, -1);
}

/* the level and item under (x, y): level in *kp, item back; -1 none */
static int
hit(x, y, kp)
	int x, y, *kp;
{
	int k, i;
	struct level *l;

	for (k = nlv - 1; k >= 0; k--) {
		l = &lv[k];
		if (l->w) {
			if (x < l->w->wr.l || x >= l->w->wr.r || y < l->w->wr.t || y >= l->w->wr.b)
				continue;
			*kp = k;
			for (i = 0; i < l->m->n; i++) {
				struct rect r = l->m->it[i].r;

				if (x >= r.l + l->w->cr.l && x < r.r + l->w->cr.l && y >= r.t + l->w->cr.t &&
				    y < r.b + l->w->cr.t)
					return i;
			}
			return -1;
		}
		*kp = k;
		i = menu_barhit(barwnd, x, y);
		if (i >= 0)
			return i;
	}
	*kp = -1;
	return -1;
}

static int
selectable(it)
	struct item *it;
{
	return !(it->flags & MF_SEPARATOR);
}

static int
nextitem(m, i, d)
	struct menu *m;
	int i, d;
{
	int k;

	for (k = 0; k < m->n; k++) {
		i = (i + d + m->n) % m->n;
		if (selectable(&m->it[i]))
			return i;
	}
	return -1;
}

static int
mnemonic(m, c)
	struct menu *m;
	int c;
{
	int i;
	char *p;

	c = c >= 'a' && c <= 'z' ? c - 32 : c;
	for (i = 0; i < m->n; i++)
		if (m->it[i].text && (p = strchr(m->it[i].text, '&')) != 0 &&
		    (p[1] >= 'a' && p[1] <= 'z' ? p[1] - 32 : p[1]) == c)
			return i;
	return -1;
}

/*
 * The loop: until an item is chosen (its id back) or the menu is left
 * (-1).  bar: the window whose bar starts it; pop: a popup alone.
 */
static int
loop(startitem, key, mouse)
	int startitem, key, mouse;
{
	struct ev e;
	int k, i, chosen = -1, down = mouse, moved = 0;
	struct item *it;

	if (startitem >= 0) {
		select_(0, startitem);
		if (lv[0].w == 0 && (lv[0].m->it[startitem].flags & MF_POPUP) && (mouse || key))
			openpopup(0, startitem);
	}
	for (;;) {
		scr_flush();
		if (user_poll(&e, -1) != 1)
			continue;
		if (e.type == EV_MOVE || e.type == EV_BTN) {
			scr_mx = e.x;
			scr_my = e.y;
			i = hit(e.x, e.y, &k);
			if (e.type == EV_MOVE) {
				moved = 1;
				if (k >= 0 && (down || lv[k].w || nlv > 1)) {
					if (i >= 0 && i != lv[k].sel) {
						closeto(k + 1);
						select_(k, i);
						if (lv[k].m->it[i].flags & MF_POPUP)
							openpopup(k, i);
					}
				}
				continue;
			}
			if (e.btn != 0)
				continue;
			if (e.down) {
				down = 1;
				if (k < 0)
					return -1;	/* outside: the menu closes */
				if (i >= 0 && !lv[k].w && i == lv[k].sel && nlv > 1 && !moved) {
					closeto(1);
					continue;
				}
				if (i >= 0) {
					closeto(k + 1);
					select_(k, i);
					if (lv[k].m->it[i].flags & MF_POPUP)
						openpopup(k, i);
				}
				continue;
			}
			down = 0;
			if (k < 0 || i < 0)
				continue;
			it = &lv[k].m->it[i];
			if ((it->flags & MF_POPUP) || !selectable(it))
				continue;
			if (it->flags & (MF_GRAYED | MF_DISABLED))
				continue;
			return it->id;
		}
		if (e.type != EV_KEY || !e.down)
			continue;
		k = nlv - 1;
		switch (e.vk) {
		case VK_ESCAPE:
			if (nlv > 1) {
				closeto(nlv - 1);
				continue;
			}
			return -1;
		case VK_MENU:
		case VK_F1 + 9:
			return -1;
		case VK_UP:
		case VK_DOWN:
			if (!lv[k].w) {
				if (lv[k].sel >= 0)
					openpopup(k, lv[k].sel);
				if (nlv > 1)
					select_(nlv - 1, nextitem(lv[nlv - 1].m, -1, 1));
				continue;
			}
			select_(k, nextitem(lv[k].m, lv[k].sel < 0 ? (e.vk == VK_DOWN ? -1 : 0) : lv[k].sel,
			    e.vk == VK_DOWN ? 1 : -1));
			continue;
		case VK_LEFT:
		case VK_RIGHT:
			if (lv[k].w && e.vk == VK_RIGHT && lv[k].sel >= 0 && (lv[k].m->it[lv[k].sel].flags & MF_POPUP)) {
				openpopup(k, lv[k].sel);
				if (nlv > k + 1)
					select_(nlv - 1, nextitem(lv[nlv - 1].m, -1, 1));
				continue;
			}
			if (lv[k].w && e.vk == VK_LEFT && k > 1) {
				closeto(k);
				continue;
			}
			if (lv[0].w == 0) {
				int wasopen = nlv > 1;

				closeto(1);
				select_(0, nextitem(lv[0].m, lv[0].sel, e.vk == VK_RIGHT ? 1 : -1));
				if (wasopen && lv[0].sel >= 0 && (lv[0].m->it[lv[0].sel].flags & MF_POPUP)) {
					openpopup(0, lv[0].sel);
					if (nlv > 1)
						select_(1, nextitem(lv[1].m, -1, 1));
				}
			}
			continue;
		case VK_RETURN:
			if (lv[k].sel < 0)
				continue;
			it = &lv[k].m->it[lv[k].sel];
			if (it->flags & MF_POPUP) {
				openpopup(k, lv[k].sel);
				if (nlv > k + 1)
					select_(nlv - 1, nextitem(lv[nlv - 1].m, -1, 1));
				continue;
			}
			if (it->flags & (MF_GRAYED | MF_DISABLED))
				continue;
			return it->id;
		default:
			if ((e.vk >= 'A' && e.vk <= 'Z') || (e.vk >= '0' && e.vk <= '9')) {
				i = mnemonic(lv[k].m, e.vk);
				if (i < 0)
					continue;
				select_(k, i);
				it = &lv[k].m->it[i];
				if (it->flags & MF_POPUP) {
					openpopup(k, i);
					if (nlv > k + 1)
						select_(nlv - 1, nextitem(lv[nlv - 1].m, -1, 1));
					continue;
				}
				if (!(it->flags & (MF_GRAYED | MF_DISABLED)))
					return it->id;
			}
		}
	}
	(void)chosen;
}

static void
finish(id, cmdwnd)
	int id;
	struct wnd *cmdwnd;
{
	extern void user_resetclicks();

	user_resetclicks();
	closeto(0);
	if (barwnd) {
		barsel = -1;
		redrawbar(barwnd);
	}
	barwnd = 0;
	if (cmdwnd && wnd_get(cmdwnd->h)) {
		wnd_send(cmdwnd, WM_MENUSELECT, 0, FP(0, 0xffff));
		wnd_send(cmdwnd, WM_EXITMENULOOP, insys, 0);
	}
	user_flushpaint();
	if (id >= 0 && cmdwnd && wnd_get(cmdwnd->h)) {
		if (insys)
			wnd_post(cmdwnd->h, WM_SYSCOMMAND, id, 0);
		else
			wnd_post(cmdwnd->h, WM_COMMAND, id, 0);
	}
}

/* the bar of w, from item (-1: by key) */
void
menu_trackbar(w, item, key)
	struct wnd *w;
	int item, key;
{
	struct menu *m = mget(w->id);
	int id, i;

	if (!m || nlv)
		return;
	owner = w;
	insys = 0;
	barwnd = w;
	barsel = -1;
	wnd_send(w, WM_ENTERMENULOOP, 0, 0);
	wnd_send(w, WM_INITMENU, m->h, 0);
	lv[0].m = m;
	lv[0].w = 0;
	lv[0].sel = -1;
	nlv = 1;
	if (item < 0 && key) {
		i = mnemonic(m, key);
		if (i < 0) {
			nlv = 0;
			barwnd = 0;
			scr_beep();
			return;
		}
		item = i;
		select_(0, item);
		if (m->it[item].flags & MF_POPUP) {
			openpopup(0, item);
			if (nlv > 1)
				select_(1, nextitem(lv[1].m, -1, 1));
		} else {
			id = m->it[item].id;
			finish(id, w);
			return;
		}
		id = loop(-1, 0, 0);
	} else if (item < 0)
		id = loop(nextitem(m, -1, 1), 0, 0);
	else
		id = loop(item, 0, 1);
	finish(id, w);
}

/* a popup alone (TrackPopupMenu, the system menu): the command, -1 none */
int
menu_track(h, flags, x, y, w, item)
	u32 h;
	int flags, x, y, item;
	struct wnd *w;
{
	struct menu *m = mget(h), host;
	struct item it;
	int id;

	if (!m || nlv)
		return 0;
	owner = w;
	insys = m->sys;
	if (m->sys && w)
		sysmenu_update(w, m);
	if (w)
		wnd_send(w, WM_INITMENU, m->h, 0);
	/* a hidden level 0 holding the popup as its only item */
	memset(&host, 0, sizeof host);
	memset(&it, 0, sizeof it);
	it.flags = MF_POPUP;
	it.id = h;
	r_set(&it.r, x, y, x + 1, y);
	host.n = 1;
	host.it = &it;
	host.sys = m->sys;
	lv[0].m = &host;
	lv[0].w = 0;
	lv[0].sel = 0;
	nlv = 1;
	barwnd = 0;
	if (w)
		wnd_send(w, WM_INITMENUPOPUP, h, FP(insys, 0));
	poplayout(m);
	{
		struct wnd *pw;
		u32 cls = ustr("#32768");

		if (x + m->popupw > screen.w) x = screen.w - m->popupw;
		if (y + m->popuph > screen.h) y = screen.h - m->popuph;
		pw = wnd_create(0, cls, 0, WS_POPUP, x, y, m->popupw, m->popuph, w ? w->h : 0, 0, 0, 0);
		ufree(cls);
		pw->user = h;
		pw->id = 0xffff;
		wnd_show(pw, SW_SHOWNA);
		lv[1].m = m;
		lv[1].w = pw;
		lv[1].sel = -1;
		nlv = 2;
		user_flushpaint();
		drawpopup(pw, m, -1);
	}
	id = loop(-1, 0, (flags & 0x8000) != 0);
	{
		extern void user_resetclicks();

		user_resetclicks();
	}
	closeto(0);
	user_flushpaint();
	if (id >= 0 && w && wnd_get(w->h)) {
		if (insys)
			wnd_post(w->h, WM_SYSCOMMAND, id, 0);
		else
			wnd_post(w->h, WM_COMMAND, id, 0);
	}
	return id >= 0;
}

int
menu_key(w, ch)
	struct wnd *w;
	int ch;
{
	menu_trackbar(w, -1, ch);
	return 1;
}

/* ---- the calls ---- */

#define	STR(p)		(gptr(p) ? gptr(p) : "")

static u32 m_LoadMenu(a) u32 *a; { return menu_load(a[0], a[1]); }

static u32
m_LoadMenuIndirect(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0]));

	return p ? menu_fromtemplate(p) : 0;
}

static u32 m_CreateMenu(a) u32 *a; { return mnew(); }
static u32 m_DestroyMenu(a) u32 *a; { mfree(a[0]); return 1; }
static u32 m_IsMenu(a) u32 *a; { return mget(a[0]) != 0; }

static char *
itemtext(flags, p)
	int flags;
	u32 p;
{
	if (flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))
		return 0;
	return gptr(p) ? gptr(p) : "";
}

static u32
m_AppendMenu(a)
	u32 *a;
{
	struct menu *m = mget(a[0]);

	if (!m)
		return 0;
	return mins(m, -1, a[1], a[2], itemtext(a[1], a[3]), a[3]);
}

static u32
m_InsertMenu(a)
	u32 *a;
{
	struct menu *m = mget(a[0]), *t;
	int i = -1;

	if (!m)
		return 0;
	if (LO16(a[1]) != 0xffff && (t = mfind(m, a[1], a[2], &i)) != 0)
		m = t;
	else
		i = -1;
	return mins(m, i, a[2], a[3], itemtext(a[2], a[4]), a[4]);
}

static u32
m_ModifyMenu(a)
	u32 *a;
{
	struct menu *m = mget(a[0]), *t;
	int i;
	struct item *it;

	if (!m || !(t = mfind(m, a[1], a[2], &i)))
		return 0;
	it = &t->it[i];
	if (it->text)
		free(it->text);
	it->flags = a[2] & ~(MF_BYPOSITION | MF_CHANGE | MF_APPEND | MF_DELETE | MF_REMOVE);
	it->id = a[3];
	it->text = itemtext(a[2], a[4]) ? strdup(itemtext(a[2], a[4])) : 0;
	it->data = a[4];
	return 1;
}

static u32
removeitem(a, del)
	u32 *a;
	int del;
{
	struct menu *m = mget(a[0]), *t;
	int i;

	if (!m || !(t = mfind(m, a[1], a[2], &i)))
		return 0;
	if (del && (t->it[i].flags & MF_POPUP))
		mfree(t->it[i].id);
	if (t->it[i].text)
		free(t->it[i].text);
	memmove(&t->it[i], &t->it[i + 1], (t->n - i - 1) * sizeof *t->it);
	t->n--;
	return 1;
}

static u32 m_RemoveMenu(a) u32 *a; { return removeitem(a, 0); }
static u32 m_DeleteMenu(a) u32 *a; { return removeitem(a, 1); }

static u32
m_ChangeMenu(a)
	u32 *a;
{
	u32 b[5];
	int f = a[4];

	if (f & MF_APPEND) {
		b[0] = a[0]; b[1] = f & ~MF_APPEND; b[2] = a[3]; b[3] = a[2];
		return m_AppendMenu(b);
	}
	if (f & MF_DELETE) {
		b[0] = a[0]; b[1] = a[1]; b[2] = f & ~MF_DELETE;
		return m_DeleteMenu(b);
	}
	if (f & MF_REMOVE) {
		b[0] = a[0]; b[1] = a[1]; b[2] = f & ~MF_REMOVE;
		return m_RemoveMenu(b);
	}
	if (f & MF_CHANGE) {
		b[0] = a[0]; b[1] = a[1]; b[2] = f & ~MF_CHANGE; b[3] = a[3]; b[4] = a[2];
		return m_ModifyMenu(b);
	}
	b[0] = a[0]; b[1] = a[1]; b[2] = f; b[3] = a[3]; b[4] = a[2];
	return m_InsertMenu(b);
}

static u32
m_CheckMenuItem(a)
	u32 *a;
{
	struct menu *t;
	int i, old;

	if (!(t = mfind(mget(a[0]), a[1], a[2], &i)))
		return 0xffffffff;
	old = t->it[i].flags & MF_CHECKED;
	if (a[2] & MF_CHECKED)
		t->it[i].flags |= MF_CHECKED;
	else
		t->it[i].flags &= ~MF_CHECKED;
	return old;
}

static u32
m_EnableMenuItem(a)
	u32 *a;
{
	struct menu *t;
	int i, old;

	if (!(t = mfind(mget(a[0]), a[1], a[2], &i)))
		return 0xffffffff;
	old = t->it[i].flags & (MF_GRAYED | MF_DISABLED);
	t->it[i].flags = (t->it[i].flags & ~(MF_GRAYED | MF_DISABLED)) | (a[2] & (MF_GRAYED | MF_DISABLED));
	return old;
}

static u32
m_HiliteMenuItem(a)
	u32 *a;
{
	struct menu *t;
	int i;

	if (!(t = mfind(mget(a[1]), a[2], a[3], &i)))
		return 0;
	if (a[3] & MF_HILITE)
		t->it[i].flags |= MF_HILITE;
	else
		t->it[i].flags &= ~MF_HILITE;
	return 1;
}

static u32
m_GetMenu(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);

	return w && !(w->style & WS_CHILD) ? w->id : 0;
}

static u32
m_SetMenu(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);

	if (!w || (w->style & WS_CHILD))
		return 0;
	w->id = a[1];
	w->flags |= WF_MENUOWNED;
	wnd_setpos(w, (struct wnd *)0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
	    SWP_FRAMECHANGED);
	return 1;
}

static u32
m_DrawMenuBar(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);

	if (w && wnd_visible(w))
		wnd_redrawframe(w);
	return 0;
}

static u32
m_GetSubMenu(a)
	u32 *a;
{
	struct menu *m = mget(a[0]);

	if (!m || (int)a[1] >= m->n || !(m->it[a[1]].flags & MF_POPUP))
		return 0;
	return m->it[a[1]].id;
}

static u32
m_GetMenuItemCount(a)
	u32 *a;
{
	struct menu *m = mget(a[0]);

	return m ? m->n : 0xffff;
}

static u32
m_GetMenuItemID(a)
	u32 *a;
{
	struct menu *m = mget(a[0]);

	if (!m || (int)a[1] >= m->n)
		return 0xffff;
	return m->it[a[1]].flags & MF_POPUP ? 0xffff : m->it[a[1]].id;
}

static u32
m_GetMenuState(a)
	u32 *a;
{
	struct menu *t;
	int i;

	if (!(t = mfind(mget(a[0]), a[1], a[2], &i)))
		return 0xffff;
	if (t->it[i].flags & MF_POPUP) {
		struct menu *s = mget(t->it[i].id);

		return (s ? s->n : 0) << 8 | (t->it[i].flags & 0xff);
	}
	return t->it[i].flags;
}

static u32
m_GetMenuString(a)
	u32 *a;
{
	struct menu *t;
	int i, n = (short)a[3];
	char *d = gptr(a[2]);

	if (!(t = mfind(mget(a[0]), a[1], a[4], &i)) || !t->it[i].text)
		return 0;
	if (!d || n <= 0)
		return strlen(t->it[i].text);
	strncpy(d, t->it[i].text, n - 1);
	d[n - 1] = 0;
	return strlen(d);
}

static u32
m_GetSystemMenu(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);

	if (!w)
		return 0;
	if (a[1]) {
		if (w->sysmenu)
			mfree(w->sysmenu);
		w->sysmenu = 0;
		return 0;
	}
	return sysmenu_of(w);
}

static u32
m_CreatePopupMenu(a)
	u32 *a;
{
	return mnew();
}

static u32
m_TrackPopupMenu(a)
	u32 *a;
{
	return menu_track(a[0], a[1] | 0x8000, (short)a[2], (short)a[3], wnd_get(a[5]), -1);
}

static u32 m_EndMenu(a) u32 *a; { return 0; }
static u32 m_GetMenuCheckMarkDimensions(a) u32 *a; { return FP(12, 12); }
static u32 m_SetMenuItemBitmaps(a) u32 *a; { return 1; }

void
menu_init()
{
	popupproc = thunk_internal(menu_popupproc, "wwwl", 'l', "PopupMenuProc");
	cls_register("#32768", CS_GLOBALCLASS | CS_SAVEBITS, popupproc, 0, 0, 0, 0, cur_arrow,
	    0, (u32)0, 1);
}

struct impl mn_impl[] = {
	{ "USER", "LoadMenu", m_LoadMenu },
	{ "USER", "LoadMenuIndirect", m_LoadMenuIndirect },
	{ "USER", "CreateMenu", m_CreateMenu },
	{ "USER", "CreatePopupMenu", m_CreatePopupMenu },
	{ "USER", "DestroyMenu", m_DestroyMenu },
	{ "USER", "IsMenu", m_IsMenu },
	{ "USER", "AppendMenu", m_AppendMenu },
	{ "USER", "InsertMenu", m_InsertMenu },
	{ "USER", "ModifyMenu", m_ModifyMenu },
	{ "USER", "RemoveMenu", m_RemoveMenu },
	{ "USER", "DeleteMenu", m_DeleteMenu },
	{ "USER", "ChangeMenu", m_ChangeMenu },
	{ "USER", "CheckMenuItem", m_CheckMenuItem },
	{ "USER", "EnableMenuItem", m_EnableMenuItem },
	{ "USER", "HiliteMenuItem", m_HiliteMenuItem },
	{ "USER", "GetMenu", m_GetMenu },
	{ "USER", "SetMenu", m_SetMenu },
	{ "USER", "DrawMenuBar", m_DrawMenuBar },
	{ "USER", "GetSubMenu", m_GetSubMenu },
	{ "USER", "GetMenuItemCount", m_GetMenuItemCount },
	{ "USER", "GetMenuItemID", m_GetMenuItemID },
	{ "USER", "GetMenuState", m_GetMenuState },
	{ "USER", "GetMenuString", m_GetMenuString },
	{ "USER", "GetSystemMenu", m_GetSystemMenu },
	{ "USER", "TrackPopupMenu", m_TrackPopupMenu },
	{ "USER", "EndMenu", m_EndMenu },
	{ "USER", "GetMenuCheckMarkDimensions", m_GetMenuCheckMarkDimensions },
	{ "USER", "SetMenuItemBitmaps", m_SetMenuItemBitmaps },
	{ 0 }
};
