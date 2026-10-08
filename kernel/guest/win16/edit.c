/*
 * edit.c -- the EDIT control, single and multiple lines.  The text
 * lives in guest memory: in the local heap of the instance that made
 * the control when it has one (EM_GETHANDLE gives that local handle, as
 * programs like Notepad expect), else in a global block.  Lines end in
 * CR LF; a multiple-line control without ES_AUTOHSCROLL wraps words.
 */

#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "scr.h"

extern u32 ualloc(), ulin(), ustr();
extern void ufree(), wnd_setfocus(), user_endpaint();
extern u16 user_beginpaint();
extern int user_createcaret();
extern void user_destroycaret(), user_setcaretpos(), user_showcaret(), caret_hide(), caret_show();
extern void clip_puttext();
extern char *clip_text();
extern u8 keystate[];
extern int user_caretwnd();

#define	ES_CENTER	0x0001
#define	ES_RIGHT	0x0002
#define	ES_MULTILINE	0x0004
#define	ES_UPPERCASE	0x0008
#define	ES_LOWERCASE	0x0010
#define	ES_PASSWORD	0x0020
#define	ES_AUTOVSCROLL	0x0040
#define	ES_AUTOHSCROLL	0x0080
#define	ES_NOHIDESEL	0x0100
#define	ES_READONLY	0x0800
#define	ES_WANTRETURN	0x1000

#define	EN_SETFOCUS	0x0100
#define	EN_KILLFOCUS	0x0200
#define	EN_CHANGE	0x0300
#define	EN_UPDATE	0x0400
#define	EN_MAXTEXT	0x0501
#define	EN_HSCROLL	0x0601
#define	EN_VSCROLL	0x0602

struct ed {
	u16	ds;		/* local: the segment; 0 global */
	u16	h;		/* the local or global handle */
	int	cap;		/* bytes the block holds */
	int	len;
	int	limit;
	int	caret, anchor;	/* the selection is between them */
	int	top, xoff;
	int	modified;
	int	pw;		/* password character */
	int	*line;		/* start of each line (visual) */
	int	nline, maxline;
	char	*undo;
	int	undocaret;
	int	drag;
	int	notify;		/* to the parent */
};

static struct ed *
edof(w)
	struct wnd *w;
{
	return (struct ed *)w->priv;
}

static char *
buf(e)
	struct ed *e;
{
	if (!e->ds)
		return (char *)M + sel_base(e->h);
	return (char *)M + sel_base(e->ds) + GW(sel_base(e->ds) + e->h);
}

static int
grow(e, need)
	struct ed *e;
	int need;
{
	int n = need + 64;

	if (need + 1 <= e->cap)
		return 1;
	if (e->ds) {
		if (!l_realloc(e->ds, e->h, n, LMEM_MOVEABLE))
			return 0;
	} else if (!g_realloc(e->h, n, GMEM_MOVEABLE))
		return 0;
	e->cap = n;
	return 1;
}

static struct bfont *
efont(w)
	struct wnd *w;
{
	struct gobj *o = gobj(w->dlgfont ? w->dlgfont : stockobj[SYSTEM_FONT], OBJ_FONT);

	return o->u.font.bf;
}

static int
multi(w)
	struct wnd *w;
{
	return (w->style & ES_MULTILINE) != 0;
}

/* the text area, inside the border */
static void
textrect(w, r)
	struct wnd *w;
	struct rect *r;
{
	*r = w->cr;
	r->l += (w->user & 0x20000) ? 3 : 1;
	r->r -= (w->user & 0x20000) ? 3 : 1;
	r->t += multi(w) ? 1 : ((w->cr.b - w->cr.t) - efont(w)->f_height) / 2;
}

/* width of n characters at s, tabs to 8 average characters */
static int
tw(w, s, n, x0)
	struct wnd *w;
	char *s;
	int n, x0;
{
	struct bfont *f = efont(w);
	struct ed *e = edof(w);
	int x = x0, i, tab = 8 * f->f_avgw;

	for (i = 0; i < n; i++) {
		if (s[i] == '\t' && multi(w))
			x = ((x - 0) / tab + 1) * tab;
		else
			x += text_width(f, e->pw ? (char *)&e->pw : s + i, 1);
	}
	return x - x0;
}

/* lines, visual: wrapped at the width when the control wraps */
static void
relayout(w)
	struct wnd *w;
{
	struct ed *e = edof(w);
	char *t = buf(e);
	int i = 0, start, brk, x, width, wrap;
	struct rect r;

	textrect(w, &r);
	width = r.r - r.l;
	wrap = multi(w) && !(w->style & ES_AUTOHSCROLL) && !(w->style & WS_HSCROLL);
	e->nline = 0;
	for (;;) {
		if (e->nline + 2 > e->maxline) {
			e->maxline = e->maxline ? e->maxline * 2 : 64;
			e->line = (int *)realloc(e->line, e->maxline * sizeof(int));
		}
		e->line[e->nline++] = i;
		if (!multi(w))
			break;
		start = i;
		brk = -1;
		x = 0;
		while (i < e->len && t[i] != '\r' && t[i] != '\n') {
			x += tw(w, t + i, 1, x);
			if (t[i] == ' ')
				brk = i + 1;
			if (wrap && x > width && i > start) {
				if (brk > start)
					i = brk;
				break;
			}
			i++;
		}
		if (i >= e->len)
			break;
		if (t[i] == '\r' || t[i] == '\n') {
			if (t[i] == '\r' && i + 1 < e->len && t[i + 1] == '\n')
				i++;
			i++;
			if (i > e->len)
				break;
		}
	}
	e->line[e->nline] = e->len;
}

/* the line holding position p */
static int
lineof(e, p)
	struct ed *e;
	int p;
{
	int k;

	for (k = e->nline - 1; k > 0 && e->line[k] > p; k--)
		;
	return k;
}

/* the end of line k, its CR LF left out */
static int
lineend(e, k)
	struct ed *e;
	int k;
{
	char *t = buf(e);
	int end = k + 1 < e->nline ? e->line[k + 1] : e->len;

	while (end > e->line[k] && (t[end - 1] == '\n' || t[end - 1] == '\r'))
		end--;
	if (k + 1 < e->nline && end == e->line[k + 1] && end > e->line[k] && t[end - 1] == ' ')
		;
	return end;
}

static int
rows(w)
	struct wnd *w;
{
	int h = efont(w)->f_height;
	struct rect r;

	textrect(w, &r);
	return h ? (r.b - r.t) / h : 1;
}

/* the x of position p on its line, from the text area's left */
static int
xof(w, p)
	struct wnd *w;
	int p;
{
	struct ed *e = edof(w);
	int k = lineof(e, p);

	return tw(w, buf(e) + e->line[k], p - e->line[k], 0);
}

/* the position on line k nearest x */
static int
posat(w, k, x)
	struct wnd *w;
	int k, x;
{
	struct ed *e = edof(w);
	char *t = buf(e);
	int p = e->line[k], end = lineend(e, k), cx = 0, cw;

	while (p < end) {
		cw = tw(w, t + p, 1, cx);
		if (cx + cw / 2 >= x)
			break;
		cx += cw;
		p++;
	}
	return p;
}

static void
setcaret(w)
	struct wnd *w;
{
	struct ed *e = edof(w);
	struct rect r;
	int k, x, y;

	if (user_caretwnd() != w->h)
		return;
	textrect(w, &r);
	k = lineof(e, e->caret);
	x = r.l + xof(w, e->caret) - e->xoff - w->cr.l;
	y = r.t + (k - e->top) * efont(w)->f_height - w->cr.t;
	user_setcaretpos(x, y);
}

static void
notify(w, code)
	struct wnd *w;
	int code;
{
	if (w->parent && w->parent != desktop)
		wnd_send(w->parent, WM_COMMAND, w->id, FP(code, w->h));
}

/* scroll so the caret shows */
static void
showcaret(w)
	struct wnd *w;
{
	struct ed *e = edof(w);
	int k = lineof(e, e->caret), n = rows(w), x;
	struct rect r;

	textrect(w, &r);
	if (k < e->top)
		e->top = k;
	else if (k >= e->top + n)
		e->top = k - n + 1;
	if (e->top < 0)
		e->top = 0;
	x = xof(w, e->caret);
	if (x - e->xoff > r.r - r.l - 2)
		e->xoff = x - (r.r - r.l) * 3 / 4;
	else if (x < e->xoff)
		e->xoff = x - (r.r - r.l) / 4;
	if (e->xoff < 0)
		e->xoff = 0;
}

static void
paint(w, hdc)
	struct wnd *w;
	u16 hdc;
{
	struct dc *dc = dc_get(hdc);
	struct ed *e = edof(w);
	struct bfont *f = efont(w);
	struct rect r, l, clip;
	char *t = buf(e);
	int k, y, ss = e->caret < e->anchor ? e->caret : e->anchor, se = e->caret < e->anchor ? e->anchor : e->caret;
	int showsel = wnd_focus == w || (w->style & ES_NOHIDESEL), i, end, x, cw;
	u16 br = 0;
	int bgidx, fgidx, hbg = pal_index(sys_color(COLOR_HIGHLIGHT)), hfg;
	COLORREF text;

	if (!dc)
		return;
	dc->st.font = w->dlgfont && gobj(w->dlgfont, OBJ_FONT) ? w->dlgfont : stockobj[SYSTEM_FONT];
	if (w->parent && w->parent != desktop)
		br = wnd_send(w->parent, WM_CTLCOLOR, hdc, FP(1, w->h));
	if (!br) {
		br = sys_brush(COLOR_WINDOW);
		dc->st.text = sys_color(COLOR_WINDOWTEXT);
	}
	text = (w->style & WS_DISABLED) ? sys_color(COLOR_GRAYTEXT) : dc->st.text;
	r = w->cr;
	d_fill(dc, &r, br, 0xf0);
	bgidx = brush_index(br);
	fgidx = pal_index(text);
	hfg = pal_index(sys_color(COLOR_HIGHLIGHTTEXT));
	textrect(w, &r);
	clip = r;
	clip.t = w->cr.t;
	clip.b = w->cr.b;
	dc->st.bkmode = TRANSPARENT;
	for (k = e->top, y = r.t; k < e->nline && y < w->cr.b; k++, y += f->f_height) {
		end = lineend(e, k);
		x = r.l - e->xoff;
		if (!multi(w)) {
			if (w->style & (ES_CENTER | ES_RIGHT)) {
				int lw = tw(w, t, e->len, 0);

				if (w->style & ES_RIGHT)
					x = r.r - lw;
				else
					x = (r.l + r.r - lw) / 2;
			}
		}
		for (i = e->line[k]; i < end; i++) {
			int sel = showsel && i >= ss && i < se;
			char ch = e->pw ? e->pw : t[i];

			cw = tw(w, t + i, 1, x - (r.l - e->xoff));
			if (x + cw > clip.l && x < clip.r) {
				r_set(&l, x, y, x + cw, y + f->f_height);
				if (sel) {
					struct rect m;

					r_and(&m, &l, &clip);
					d_fillcolor(dc, &m, hbg);
				}
				if (ch != '\t') {
					dc->st.text = sel ? sys_color(COLOR_HIGHLIGHTTEXT) : text;
					text_draw(dc, x, y, &ch, 1, &clip, (struct rect *)0, (int *)0);
				}
			}
			x += cw;
		}
		/* a selected line break shows as a bit of highlight */
		if (showsel && multi(w) && end >= ss && end < se && k + 1 < e->nline) {
			r_set(&l, x, y, x + f->f_avgw / 2 + 1, y + f->f_height);
			r_and(&l, &l, &clip);
			d_fillcolor(dc, &l, hbg);
		}
	}
	if (w->user & 0x20000) {
		r = w->cr;
		d_frame(dc, &r, 0);
	}
	(void)bgidx;
	(void)fgidx;
	(void)hfg;
}

static void
redraw(w)
	struct wnd *w;
{
	u16 h;

	if (!wnd_visible(w))
		return;
	caret_hide();
	h = user_dc(w, DCK_WINDOW, (struct rgn *)0);
	paint(w, h);
	user_releasedc(h);
	setcaret(w);
	caret_show();
}

static void
scrollbars(w)
	struct wnd *w;
{
	struct ed *e = edof(w);
	int n = rows(w);

	if ((w->style & WS_VSCROLL) && multi(w)) {
		w->sb[1].min = 0;
		w->sb[1].max = e->nline > n ? e->nline - n : 0;
		w->sb[1].pos = e->top;
		if (wnd_visible(w))
			wnd_redrawframe(w);
	}
}

static void
changed(w)
	struct wnd *w;
{
	struct ed *e = edof(w);

	e->modified = 1;
	relayout(w);
	showcaret(w);
	notify(w, EN_UPDATE);
	redraw(w);
	scrollbars(w);
	notify(w, EN_CHANGE);
}

static void
saveundo(e)
	struct ed *e;
{
	if (e->undo)
		free(e->undo);
	e->undo = (char *)malloc(e->len + 1);
	memcpy(e->undo, buf(e), e->len);
	e->undo[e->len] = 0;
	e->undocaret = e->caret;
}

/* the selection replaced by s (n bytes); 0 if over the limit */
static int
replace(w, s, n)
	struct wnd *w;
	char *s;
	int n;
{
	struct ed *e = edof(w);
	int ss = e->caret < e->anchor ? e->caret : e->anchor, se = e->caret < e->anchor ? e->anchor : e->caret;
	int nl = e->len - (se - ss) + n, i;
	char *t;

	if (nl > e->limit) {
		notify(w, EN_MAXTEXT);
		n = e->limit - (e->len - (se - ss));
		if (n <= 0)
			return 0;
		nl = e->limit;
	}
	if (!grow(e, nl)) {
		notify(w, 0x0500);	/* EN_ERRSPACE */
		return 0;
	}
	t = buf(e);
	memmove(t + ss + n, t + se, e->len - se);
	memcpy(t + ss, s, n);
	if (w->style & ES_UPPERCASE)
		for (i = ss; i < ss + n; i++)
			if (t[i] >= 'a' && t[i] <= 'z') t[i] -= 32;
	if (w->style & ES_LOWERCASE)
		for (i = ss; i < ss + n; i++)
			if (t[i] >= 'A' && t[i] <= 'Z') t[i] += 32;
	e->len = nl;
	t[e->len] = 0;
	e->caret = e->anchor = ss + n;
	return 1;
}

static void
settext(w, s)
	struct wnd *w;
	char *s;
{
	struct ed *e = edof(w);
	int n = strlen(s);

	if (n > e->limit)
		n = e->limit;
	if (!grow(e, n))
		return;
	memcpy(buf(e), s, n);
	e->len = n;
	buf(e)[n] = 0;
	e->caret = e->anchor = 0;
	e->top = e->xoff = 0;
	e->modified = 0;
	relayout(w);
	scrollbars(w);
	notify(w, EN_UPDATE);
	redraw(w);
	notify(w, EN_CHANGE);
}

static int
isword(c)
	int c;
{
	return c != ' ' && c != '\t' && c != '\r' && c != '\n';
}

static int
wordleft(e, p)
	struct ed *e;
	int p;
{
	char *t = buf(e);

	while (p > 0 && !isword(t[p - 1]))
		p--;
	while (p > 0 && isword(t[p - 1]))
		p--;
	return p;
}

static int
wordright(e, p)
	struct ed *e;
	int p;
{
	char *t = buf(e);

	while (p < e->len && isword(t[p]))
		p++;
	while (p < e->len && !isword(t[p]))
		p++;
	return p;
}

/* the caret moves to p; with Shift the selection follows it */
static void
moveto(w, p, extend)
	struct wnd *w;
	int p, extend;
{
	struct ed *e = edof(w);

	if (p < 0)
		p = 0;
	if (p > e->len)
		p = e->len;
	e->caret = p;
	if (!extend)
		e->anchor = p;
	showcaret(w);
	redraw(w);
	scrollbars(w);
}

static void
cut(w, copy, del)
	struct wnd *w;
	int copy, del;
{
	struct ed *e = edof(w);
	int ss = e->caret < e->anchor ? e->caret : e->anchor, se = e->caret < e->anchor ? e->anchor : e->caret;
	char *s;

	if (ss == se)
		return;
	if (copy && !e->pw) {
		s = (char *)malloc(se - ss + 1);
		memcpy(s, buf(e) + ss, se - ss);
		s[se - ss] = 0;
		clip_puttext(s);
		free(s);
	}
	if (del && !(w->style & ES_READONLY)) {
		saveundo(e);
		replace(w, "", 0);
		changed(w);
	}
}

static void
paste(w)
	struct wnd *w;
{
	char *s = clip_text(), *c;
	int n;

	if (!s || (w->style & ES_READONLY))
		return;
	n = strlen(s);
	if (!multi(w)) {
		/* one line only: up to the first break */
		c = strpbrk(s, "\r\n");
		if (c)
			n = c - s;
	}
	saveundo(edof(w));
	replace(w, s, n);
	changed(w);
}

static void
keydown(w, vk)
	struct wnd *w;
	int vk;
{
	struct ed *e = edof(w);
	int shift = (keystate[VK_SHIFT] & 0x80) != 0, ctrl = (keystate[VK_CONTROL] & 0x80) != 0;
	int k = lineof(e, e->caret), x, p;

	switch (vk) {
	case VK_LEFT:
		if (!shift && e->caret != e->anchor) {
			moveto(w, e->caret < e->anchor ? e->caret : e->anchor, 0);
			return;
		}
		p = ctrl ? wordleft(e, e->caret) : e->caret - 1;
		if (!ctrl && p > 0 && buf(e)[p] == '\n' && buf(e)[p - 1] == '\r')
			p--;
		moveto(w, p, shift);
		return;
	case VK_RIGHT:
		if (!shift && e->caret != e->anchor) {
			moveto(w, e->caret > e->anchor ? e->caret : e->anchor, 0);
			return;
		}
		p = ctrl ? wordright(e, e->caret) : e->caret + 1;
		if (!ctrl && e->caret < e->len && buf(e)[e->caret] == '\r' && p < e->len && buf(e)[p] == '\n')
			p++;
		moveto(w, p, shift);
		return;
	case VK_UP:
	case VK_DOWN:
	case VK_PRIOR:
	case VK_NEXT:
		if (!multi(w))
			return;
		x = xof(w, e->caret);
		k += vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -rows(w) : rows(w);
		if (k < 0)
			k = 0;
		if (k >= e->nline)
			k = e->nline - 1;
		moveto(w, posat(w, k, x), shift);
		return;
	case VK_HOME:
		moveto(w, ctrl ? 0 : e->line[k], shift);
		return;
	case VK_END:
		moveto(w, ctrl ? e->len : lineend(e, k), shift);
		return;
	case VK_DELETE:
		if (w->style & ES_READONLY)
			return;
		if (shift) {
			cut(w, 1, 1);
			return;
		}
		if (e->caret == e->anchor) {
			if (e->caret >= e->len)
				return;
			e->anchor = e->caret + (buf(e)[e->caret] == '\r' && e->caret + 1 < e->len &&
			    buf(e)[e->caret + 1] == '\n' ? 2 : 1);
		}
		saveundo(e);
		replace(w, "", 0);
		changed(w);
		return;
	case VK_INSERT:
		if (shift)
			paste(w);
		else if (ctrl)
			cut(w, 1, 0);
		return;
	}
}

static void
typed(w, c)
	struct wnd *w;
	int c;
{
	struct ed *e = edof(w);
	char ch = c;

	if (w->style & ES_READONLY) {
		if (c == 3)
			cut(w, 1, 0);
		return;
	}
	switch (c) {
	case 8:
		if (e->caret == e->anchor) {
			if (e->caret == 0)
				return;
			e->anchor = e->caret - 1;
			if (e->anchor > 0 && buf(e)[e->anchor] == '\n' && buf(e)[e->anchor - 1] == '\r')
				e->anchor--;
		}
		saveundo(e);
		replace(w, "", 0);
		changed(w);
		return;
	case 3:		/* Ctrl+C */
		cut(w, 1, 0);
		return;
	case 24:	/* Ctrl+X */
		cut(w, 1, 1);
		return;
	case 22:	/* Ctrl+V */
		paste(w);
		return;
	case 26:	/* Ctrl+Z */
		SendUndo(w);
		return;
	case 13:
	case 10:
		if (!multi(w))
			return;
		saveundo(e);
		replace(w, "\r\n", 2);
		changed(w);
		return;
	case 9:
		if (!multi(w))
			return;
		break;
	default:
		if (c < ' ')
			return;
	}
	saveundo(e);
	if (replace(w, &ch, 1))
		changed(w);
}

int
SendUndo(w)
	struct wnd *w;
{
	struct ed *e = edof(w);
	char *u = e->undo;
	int c = e->undocaret;

	if (!u)
		return 0;
	e->undo = 0;
	saveundo(e);
	if (!grow(e, strlen(u))) {
		free(u);
		return 0;
	}
	strcpy(buf(e), u);
	e->len = strlen(u);
	free(u);
	e->caret = e->anchor = c > e->len ? e->len : c;
	changed(w);
	return 1;
}

/* the position under a client point */
static int
hitpos(w, x, y)
	struct wnd *w;
	int x, y;
{
	struct ed *e = edof(w);
	struct rect r;
	int k;

	textrect(w, &r);
	k = e->top + (y + w->cr.t - r.t) / efont(w)->f_height;
	if (y + w->cr.t < r.t)
		k = e->top - 1;
	if (k < 0)
		k = 0;
	if (k >= e->nline)
		k = e->nline - 1;
	return posat(w, k, x + w->cr.l - r.l + e->xoff);
}

u32
edit_proc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);
	struct ed *e;
	u32 p, r;
	int i, n, k;

	if (!w)
		return 0;
	e = edof(w);
	if (!e && a[1] != WM_NCCREATE && a[1] != WM_CREATE)
		return user_defproc(w, a[1], a[2], a[3]);
	switch (a[1]) {
	case WM_NCCREATE:
		if (w->style & WS_BORDER) {
			w->style &= ~WS_BORDER;
			w->user |= 0x20000;	/* we draw the border ourselves: in the client area */
		}
		return user_defproc(w, a[1], a[2], a[3]);
	case WM_CREATE:
		e = (struct ed *)calloc(1, sizeof *e);
		w->priv = e;
		e->limit = multi(w) ? 30000 : 30000;
		e->pw = (w->style & ES_PASSWORD) ? '*' : 0;
		/* the text in the instance's local heap if there is one */
		e->ds = 0;
		if (w->hinst && (e->h = l_alloc(w->hinst, LMEM_MOVEABLE | LMEM_ZEROINIT, 64)) != 0) {
			struct gblock *b = g_block(w->hinst);

			e->ds = b ? SEL(b - gblk) : w->hinst;
		} else
			e->h = g_alloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 64, w->hinst);
		e->cap = 64;
		e->len = 0;
		relayout(w);
		settext(w, w->text);
		e->modified = 0;
		return 0;
	case WM_NCDESTROY:
		if (e) {
			if (e->ds)
				l_free(e->ds, e->h);
			else
				g_free(e->h);
			free(e->line);
			if (e->undo)
				free(e->undo);
			free(e);
			w->priv = 0;
		}
		return user_defproc(w, a[1], a[2], a[3]);
	case WM_NCPAINT:
		user_defproc(w, a[1], a[2], a[3]);
		return 0;
	case WM_PAINT:
		{
			u16 h = user_beginpaint(w, 0);

			paint(w, h);
			user_releasedc(h);
			user_endpaint(w, 0);
			setcaret(w);
		}
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_SIZE:
		relayout(w);
		scrollbars(w);
		return 0;
	case WM_SETFONT:
		w->dlgfont = a[2];
		relayout(w);
		if (LO16(a[3]))
			wnd_invalidate(w, (struct rect *)0, 1);
		return 0;
	case WM_GETFONT:
		return w->dlgfont;
	case WM_GETDLGCODE:
		return 0x0001 | 0x0080 | 0x0008 | (multi(w) ? 0x0004 : 0);
	case WM_SETFOCUS:
		user_createcaret(w->h, 0, 1, efont(w)->f_height);
		setcaret(w);
		user_showcaret(w->h, 1);
		redraw(w);
		notify(w, EN_SETFOCUS);
		return 0;
	case WM_KILLFOCUS:
		user_destroycaret();
		redraw(w);
		notify(w, EN_KILLFOCUS);
		return 0;
	case WM_ENABLE:
		redraw(w);
		return 0;
	case WM_LBUTTONDOWN:
		wnd_setfocus(w);
		moveto(w, hitpos(w, (short)LO16(a[3]), (short)HI16(a[3])), (a[2] & 4) != 0);
		e->drag = 1;
		wnd_capture = w;
		return 0;
	case WM_LBUTTONDBLCLK:
		i = hitpos(w, (short)LO16(a[3]), (short)HI16(a[3]));
		e->anchor = wordleft(e, i < e->len && isword(buf(e)[i]) ? i + 1 : i);
		e->caret = i;
		while (e->caret < e->len && isword(buf(e)[e->caret]))
			e->caret++;
		redraw(w);
		return 0;
	case WM_MOUSEMOVE:
		if (e->drag && wnd_capture == w)
			moveto(w, hitpos(w, (short)LO16(a[3]), (short)HI16(a[3])), 1);
		return 0;
	case WM_LBUTTONUP:
		if (wnd_capture == w)
			wnd_capture = 0;
		e->drag = 0;
		return 0;
	case WM_KEYDOWN:
		keydown(w, a[2]);
		return 0;
	case WM_CHAR:
		typed(w, a[2]);
		return 0;
	case WM_VSCROLL:
		n = rows(w);
		switch (LO16(a[2])) {
		case 0: e->top--; break;
		case 1: e->top++; break;
		case 2: e->top -= n; break;
		case 3: e->top += n; break;
		case 4: case 5: e->top = LO16(a[3]); break;
		default: return 0;
		}
		if (e->top > e->nline - n) e->top = e->nline - n;
		if (e->top < 0) e->top = 0;
		redraw(w);
		scrollbars(w);
		notify(w, EN_VSCROLL);
		return 0;
	case WM_HSCROLL:
		switch (LO16(a[2])) {
		case 0: e->xoff -= 8; break;
		case 1: e->xoff += 8; break;
		case 2: e->xoff -= 64; break;
		case 3: e->xoff += 64; break;
		default: return 0;
		}
		if (e->xoff < 0) e->xoff = 0;
		redraw(w);
		notify(w, EN_HSCROLL);
		return 0;
	case WM_SETTEXT:
		settext(w, gptr(a[3]) ? gptr(a[3]) : "");
		return 1;
	case WM_GETTEXT:
		p = lin(FPSEL(a[3]), FPOFF(a[3]));
		n = (short)a[2];
		if (!p || n <= 0)
			return 0;
		if (n > e->len + 1)
			n = e->len + 1;
		memcpy(M + p, buf(e), n - 1);
		M[p + n - 1] = 0;
		return n - 1;
	case WM_GETTEXTLENGTH:
		return e->len;
	case WM_CUT: cut(w, 1, 1); return 0;
	case WM_COPY: cut(w, 1, 0); return 0;
	case WM_PASTE: paste(w); return 0;
	case WM_CLEAR: cut(w, 0, 1); return 0;
	case WM_UNDO:
	case WM_USER + 23:	/* EM_UNDO */
		return SendUndo(w);
	/* EM_* */
	case WM_USER + 0:	/* EM_GETSEL */
		return FP(e->caret > e->anchor ? e->caret : e->anchor, e->caret < e->anchor ? e->caret : e->anchor);
	case WM_USER + 1:	/* EM_SETSEL */
		i = (short)LO16(a[3]);
		k = (short)HI16(a[3]);
		if (i < 0) {
			e->anchor = e->caret;
		} else {
			if (k < 0 || k > e->len)
				k = e->len;
			if (i > e->len)
				i = e->len;
			e->anchor = i;
			e->caret = k;
		}
		if (!a[2])
			showcaret(w);
		redraw(w);
		return 1;
	case WM_USER + 2:	/* EM_GETRECT */
		p = lin(FPSEL(a[3]), FPOFF(a[3]));
		if (p) {
			struct rect rr;

			textrect(w, &rr);
			rr.l -= w->cr.l; rr.r -= w->cr.l; rr.t -= w->cr.t; rr.b -= w->cr.t;
			r_put(p, &rr);
		}
		return 0;
	case WM_USER + 3:	/* EM_SETRECT */
	case WM_USER + 4:
		return 0;
	case WM_USER + 5:	/* EM_SCROLL */
		return wnd_send(w, WM_VSCROLL, a[2], 0), 1;
	case WM_USER + 6:	/* EM_LINESCROLL: lines in the low word of lParam */
		e->top += (short)LO16(a[3]);
		if (e->top > e->nline - 1) e->top = e->nline - 1;
		if (e->top < 0) e->top = 0;
		e->xoff += (short)HI16(a[3]) * efont(w)->f_avgw;
		if (e->xoff < 0) e->xoff = 0;
		redraw(w);
		scrollbars(w);
		return 1;
	case WM_USER + 8:	/* EM_GETMODIFY */
		return e->modified;
	case WM_USER + 9:	/* EM_SETMODIFY */
		e->modified = a[2] != 0;
		return 0;
	case WM_USER + 10:	/* EM_GETLINECOUNT */
		return e->nline;
	case WM_USER + 11:	/* EM_LINEINDEX */
		k = (short)a[2];
		if (k < 0)
			k = lineof(e, e->caret);
		return k < e->nline ? e->line[k] : 0xffff;
	case WM_USER + 12:	/* EM_SETHANDLE */
		if (e->ds && a[2]) {
			l_free(e->ds, e->h);
			e->h = a[2];
			e->cap = l_size(e->ds, e->h);
			e->len = strlen(buf(e));
			e->caret = e->anchor = 0;
			e->top = e->xoff = 0;
			e->modified = 0;
			relayout(w);
			scrollbars(w);
			wnd_invalidate(w, (struct rect *)0, 1);
		}
		return 0;
	case WM_USER + 13:	/* EM_GETHANDLE */
		return e->h;
	case WM_USER + 14:	/* EM_GETTHUMB */
		return e->top;
	case WM_USER + 17:	/* EM_LINELENGTH */
		i = (short)a[2];
		if (i < 0) {
			int ss = e->caret < e->anchor ? e->caret : e->anchor;

			k = lineof(e, ss);
			return lineend(e, k) - e->line[k] - ((e->caret > e->anchor ? e->caret : e->anchor) - ss);
		}
		k = lineof(e, i);
		return lineend(e, k) - e->line[k];
	case WM_USER + 18:	/* EM_REPLACESEL */
		{
			char *s = gptr(a[3]) ? gptr(a[3]) : "";

			saveundo(e);
			replace(w, s, strlen(s));
			changed(w);
		}
		return 0;
	case WM_USER + 20:	/* EM_GETLINE */
		k = (short)a[2];
		p = lin(FPSEL(a[3]), FPOFF(a[3]));
		if (!p || k < 0 || k >= e->nline)
			return 0;
		n = lineend(e, k) - e->line[k];
		if (n > GW(p))
			n = GW(p);
		memcpy(M + p, buf(e) + e->line[k], n);
		return n;
	case WM_USER + 21:	/* EM_LIMITTEXT */
		e->limit = a[2] ? a[2] : 30000;
		return 0;
	case WM_USER + 22:	/* EM_CANUNDO */
		return e->undo != 0;
	case WM_USER + 24:	/* EM_FMTLINES */
		return 1;
	case WM_USER + 25:	/* EM_LINEFROMCHAR */
		i = (short)a[2];
		return lineof(e, i < 0 ? e->caret : i);
	case WM_USER + 27:	/* EM_SETTABSTOPS */
		return 1;
	case WM_USER + 28:	/* EM_SETPASSWORDCHAR */
		e->pw = a[2] & 0xff;
		relayout(w);
		redraw(w);
		return 0;
	case WM_USER + 29:	/* EM_EMPTYUNDOBUFFER */
		if (e->undo)
			free(e->undo);
		e->undo = 0;
		return 0;
	case WM_USER + 30:	/* EM_GETFIRSTVISIBLELINE */
		return e->top;
	case WM_USER + 31:	/* EM_SETREADONLY */
		if (a[2])
			w->style |= ES_READONLY;
		else
			w->style &= ~ES_READONLY;
		return 1;
	}
	r = user_defproc(w, a[1], a[2], a[3]);
	return r;
}
