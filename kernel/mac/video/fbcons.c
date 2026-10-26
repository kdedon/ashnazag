/*
 * fbcons.c -- text console on a linear frame buffer.
 *
 * 8x16 cells at 1, 2, 4, 8, 16 or 32 bits per pixel, in the mode the
 * boot firmware left.  Black on white, the Mac convention: in the
 * indexed depths pixel 0 is white and all-ones black (standard Mac
 * CLUTs, 1 bit = black); 16 bpp is x-5-5-5, 32 bpp x-8-8-8.
 *
 * Output side of the console: kernel printf (fbcons_kputc) and the
 * console tty (fbcons_putc) each have their own VT100 parser and share
 * the screen and cursor.  A caller that interrupts a drawing caller
 * queues its bytes; the owner draws them before it lets go.
 *
 * All state is initialised data: this runs from aux_entry, before BSS
 * is cleared.
 */
#include "fbcons.h"

#ifdef FB_HOST
extern unsigned long fbh_map();
#define FB_MAP(a)	fbh_map(a)
#define FB_ST4(p, v)	((p)[0] = (v) >> 24, (p)[1] = (v) >> 16, (p)[2] = (v) >> 8, (p)[3] = (v))
#define FB_U32		unsigned int
#define FB_IPLHI()	0
#define FB_IPLX(s)	((void)(s))
#define FB_PANIC()	0
#else
extern char *panicstr;
#define FB_PANIC()	(panicstr != 0)
#define FB_MAP(a)	(a)
#define FB_ST4(p, v)	(*(VOL unsigned long *)(p) = (v))
#define FB_U32		unsigned long
#define FB_IPLHI() ({ int __s; \
	__asm__ __volatile__("mov.w %%sr,%0\n\tmov.w &0x2700,%%sr" \
	    : "=d" (__s) : : "memory"); __s; })
#define FB_IPLX(s) __asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory")
#endif

#define VS_NORM		0
#define VS_ESC		1
#define VS_CSI		2
#define VS_SCS0		3	/* ESC ( */
#define VS_SCS1		4	/* ESC ) */
#define VS_SKIP		5	/* ESC # and other two-byte forms */

struct fbcons fbcons = { 0 };
struct fbvt fbvt_kern = { 0, 0, { 0 }, 0, { 0, 0 }, 0, 1 };
struct fbvt fbvt_tty = { 0, 0, { 0 }, 0, { 0, 0 }, 0, 0 };
void (*fbcons_panicfn)() = 0;
void (*fbcons_pollfn)() = 0;

#define FC	(&fbcons)
#define PIX(x, y) ((VOL unsigned char *)(FC->fc_m.fm_base + \
	(unsigned long)(y) * FC->fc_m.fm_row + (unsigned long)(x) * FC->fc_m.fm_depth / 8))
#define TXT(x, y) (FC->fc_text[(long)(y) * FC->fc_cols + (x)])
#define FB_POLL()	do { if (fbcons_pollfn) (*fbcons_pollfn)(); } while (0)

static void fb_vt();

/* ------------------------------------------------------------ pixels */

static unsigned long
fb_repl(v, d)
unsigned long v, d;
{
	for (; d < 32; d <<= 1)
		v |= v << d;
	return v;
}

/* nibble (bit 3 leftmost) -> mask of 4 pixels of fm_depth bits */
static void
fb_mkexp()
{
	register int n, i, k, d;
	register unsigned long w;

	d = FC->fc_m.fm_depth;
	for (n = 0; n < 16; n++) {
		for (k = 0; k < 4; k++)
			FC->fc_exp[n][k] = 0;
		for (i = 0; i < 4; i++) {
			if (!(n & (8 >> i)))
				continue;
			if (d <= 8) {
				w = (d == 8) ? 0xFF : ((1UL << d) - 1);
				FC->fc_exp[n][0] |= w << ((3 - i) * d);
			} else if (d == 16)
				FC->fc_exp[n][i >> 1] |= 0xFFFFUL << ((i & 1) ? 0 : 16);
			else
				FC->fc_exp[n][i] = 0xFFFFFFFFUL;
		}
	}
}

/* n bytes at p of the 32-bit pattern v (aligned to the frame-buffer base) */
static void
fb_fill(p, n, v)
register VOL unsigned char *p;
register long n;
register unsigned long v;
{
	register unsigned long o;

	while (n > 0 && ((unsigned long)p & 3)) {
		o = ((unsigned long)p - FC->fc_m.fm_base) & 3;
		*p++ = v >> (24 - 8 * o);
		n--;
	}
	if (((unsigned long)p - FC->fc_m.fm_base) & 3)
		v = fb_repl(v & 0xFF, 8);	/* odd base: byte patterns only */
	for (; n >= 4; n -= 4, p += 4)
		FB_ST4(p, v);
	for (; n > 0; n--, p++) {
		o = ((unsigned long)p - FC->fc_m.fm_base) & 3;
		*p = v >> (24 - 8 * o);
	}
}

/* memmove within the frame buffer */
static void
fb_move(d, s, n)
register VOL unsigned char *d, *s;
register long n;
{
	if (d < s) {
		if ((((unsigned long)d | (unsigned long)s) & 3) == 0)
			for (; n >= 4; n -= 4, d += 4, s += 4) {
				*(VOL FB_U32 *)d = *(VOL FB_U32 *)s;
				if ((n & 1023) == 0)
					FB_POLL();
			}
		for (; n > 0; n--)
			*d++ = *s++;
	} else if (d > s) {
		d += n;
		s += n;
		if ((((unsigned long)d | (unsigned long)s) & 3) == 0)
			for (; n >= 4; n -= 4) {
				d -= 4;
				s -= 4;
				*(VOL FB_U32 *)d = *(VOL FB_U32 *)s;
				if ((n & 1023) == 0)
					FB_POLL();
			}
		for (; n > 0; n--)
			*--d = *--s;
	}
}

/* cell v (glyph | FB_TINV | FB_TBOLD) drawn at (x, y) */
static void
fb_paint(x, y, v)
int x, y, v;
{
	register VOL unsigned char *p;
	register unsigned char *g;
	register unsigned long m, fg, bg, row;
	register int r, b, k, d;

	p = PIX(x * FB_CW, y * FB_CH);
	g = fb_font[v & 0xFF];
	row = FC->fc_m.fm_row;
	d = FC->fc_m.fm_depth;
	fg = v & FB_TINV ? FC->fc_bg : FC->fc_fg;
	bg = v & FB_TINV ? FC->fc_fg : FC->fc_bg;
	for (r = 0; r < FB_CH; r++, p += row) {
		b = g[r];
		if (v & FB_TBOLD)
			b |= b >> 1;
		switch (d) {
		case 1:
			*p = (b & fg) | (~b & bg);
			break;
		case 2:
			m = FC->fc_exp[b >> 4][0] << 8 | FC->fc_exp[b & 15][0];
			p[0] = ((m & fg) | (~m & bg)) >> 8;
			p[1] = (m & fg) | (~m & bg);
			break;
		case 4:
			m = FC->fc_exp[b >> 4][0] << 16 | FC->fc_exp[b & 15][0];
			m = (m & fg) | (~m & bg);
			p[0] = m >> 24; p[1] = m >> 16; p[2] = m >> 8; p[3] = m;
			break;
		default:	/* 8, 16, 32: d / 8 longs per nibble */
			for (k = 0; k < d / 4; k++) {
				m = FC->fc_exp[k < d / 8 ? b >> 4 : b & 15][k % (d / 8)];
				m = (m & fg) | (~m & bg);
				FB_ST4(p + 4 * k, m);
			}
			break;
		}
	}
}

/* one glyph at cell (x, y) in the current attributes */
static void
fb_glyph(x, y, c)
int x, y, c;
{
	register int v;

	v = (c & 0xFF) | (FC->fc_inv ? FB_TINV : 0) | (FC->fc_bold ? FB_TBOLD : 0);
	if (FC->fc_text)
		TXT(x, y) = v;
	if (!FC->fc_hide)
		fb_paint(x, y, v);
}

/* n cells of the text from index s to index d */
static void
fb_tmove(d, s, n)
register long d, s, n;
{
	register unsigned short *t = FC->fc_text;

	if (t == 0)
		return;
	if (d < s)
		for (; n > 0; n--) {
			t[d++] = t[s++];
			if ((n & 127) == 0)
				FB_POLL();
		}
	else
		for (d += n, s += n; n > 0; n--) {
			t[--d] = t[--s];
			if ((n & 127) == 0)
				FB_POLL();
		}
}

/* cells x0..x1 of rows y0..y1 to the background */
static void
fb_erase(x0, y0, x1, y1)
int x0, y0, x1, y1;
{
	register VOL unsigned char *p;
	register int r, n;

	if (x0 > x1 || y0 > y1)
		return;
	if (FC->fc_text)
		for (r = y0; r <= y1; r++)
			for (n = x0; n <= x1; n++)
				TXT(n, r) = FB_TBLANK;
	if (FC->fc_hide)
		return;
	n = (x1 - x0 + 1) * FC->fc_m.fm_depth;	/* 8 pixels = depth bytes */
	p = PIX(x0 * FB_CW, y0 * FB_CH);
	for (r = (y1 - y0 + 1) * FB_CH; r > 0; r--, p += FC->fc_m.fm_row) {
		fb_fill(p, (long)n, FC->fc_bg);
		FB_POLL();
	}
}

/* rows top..bot move up by n (n < 0: down), vacated rows erased */
static void
fb_scroll(top, bot, n)
int top, bot, n;
{
	register VOL unsigned char *d, *s;
	register long w, row;
	register int r, cnt;

	if (top > bot)
		return;
	if (n > bot - top + 1)
		n = bot - top + 1;
	if (n < -(bot - top + 1))
		n = -(bot - top + 1);
	w = (long)FC->fc_cols * FC->fc_m.fm_depth;
	row = FC->fc_m.fm_row;
	if (n > 0) {
		fb_tmove((long)top * FC->fc_cols, (long)(top + n) * FC->fc_cols,
		    (long)(bot - top + 1 - n) * FC->fc_cols);
		cnt = FC->fc_hide ? 0 : (bot - top + 1 - n) * FB_CH;
		d = PIX(0, top * FB_CH);
		s = PIX(0, (top + n) * FB_CH);
		if (w == row)
			fb_move(d, s, w * cnt);
		else
			for (r = 0; r < cnt; r++, d += row, s += row)
				fb_move(d, s, w);
		fb_erase(0, bot - n + 1, FC->fc_cols - 1, bot);
	} else if (n < 0) {
		n = -n;
		fb_tmove((long)(top + n) * FC->fc_cols, (long)top * FC->fc_cols,
		    (long)(bot - top + 1 - n) * FC->fc_cols);
		cnt = FC->fc_hide ? 0 : (bot - top + 1 - n) * FB_CH;
		if (w == row)
			fb_move(PIX(0, (top + n) * FB_CH), PIX(0, top * FB_CH), w * cnt);
		else {
			d = PIX(0, (bot + 1) * FB_CH - 1);
			s = PIX(0, (bot + 1 - n) * FB_CH - 1);
			for (r = 0; r < cnt; r++, d -= row, s -= row)
				fb_move(d, s, w);
		}
		fb_erase(0, top, FC->fc_cols - 1, top + n - 1);
	}
}

/* shift cells x.. of row y right by n (n < 0: left), fill with blanks */
static void
fb_shift(y, x, n)
int y, x, n;
{
	register VOL unsigned char *p;
	register int r, d, cols;
	int span;

	cols = FC->fc_cols;
	d = FC->fc_m.fm_depth;
	if (n > cols - x)
		n = cols - x;
	if (n < -(cols - x))
		n = -(cols - x);
	if (n == 0)
		return;
	span = cols - x - (n > 0 ? n : -n);
	if (n > 0)
		fb_tmove((long)y * cols + x + n, (long)y * cols + x, (long)span);
	else
		fb_tmove((long)y * cols + x, (long)y * cols + x - n, (long)span);
	p = PIX(x * FB_CW, y * FB_CH);
	for (r = FC->fc_hide ? FB_CH : 0; r < FB_CH; r++, p += FC->fc_m.fm_row)
		if (n > 0)
			fb_move(p + n * d, p, (long)span * d);
		else
			fb_move(p, p - n * d, (long)span * d);
	if (n > 0)
		fb_erase(x, y, x + n - 1, y);
	else
		fb_erase(cols + n, y, cols - 1, y);
}

/* swap foreground and background in the cursor cell */
static void
fb_cursor()
{
	register VOL unsigned char *p;
	register unsigned long v, o;
	register int r, k, n;

	if (FC->fc_hide || FC->fc_x >= FC->fc_cols || FC->fc_y >= FC->fc_rows)
		return;
	v = FC->fc_fg ^ FC->fc_bg;
	n = FC->fc_m.fm_depth;
	p = PIX(FC->fc_x * FB_CW, FC->fc_y * FB_CH);
	for (r = 0; r < FB_CH; r++, p += FC->fc_m.fm_row)
		for (k = 0; k < n; k++) {
			o = ((unsigned long)(p + k) - FC->fc_m.fm_base) & 3;
			p[k] ^= v >> (24 - 8 * o);
		}
	FC->fc_cdrawn = !FC->fc_cdrawn;
}

/* ---------------------------------------------------------- terminal */

static void
fb_lf()
{
	if (FC->fc_y == FC->fc_bot)
		fb_scroll(FC->fc_top, FC->fc_bot, 1);
	else if (FC->fc_y < FC->fc_rows - 1)
		FC->fc_y++;
}

static void
fb_ri()
{
	if (FC->fc_y == FC->fc_top)
		fb_scroll(FC->fc_top, FC->fc_bot, -1);
	else if (FC->fc_y > 0)
		FC->fc_y--;
}

static void
fb_home()
{
	FC->fc_x = FC->fc_y = 0;
	FC->fc_wrap = 0;
}

static void
fb_reset()
{
	FC->fc_top = 0;
	FC->fc_bot = FC->fc_rows - 1;
	FC->fc_inv = FC->fc_bold = 0;
	FC->fc_curs = FC->fc_awm = 1;
	FC->fc_sx = FC->fc_sy = FC->fc_sinv = FC->fc_sbold = 0;
	fb_home();
	fb_erase(0, 0, FC->fc_cols - 1, FC->fc_rows - 1);
}

static void
fb_graph(c)
int c;
{
	if (FC->fc_wrap) {
		FC->fc_wrap = 0;
		FC->fc_x = 0;
		fb_lf();
	}
	fb_glyph(FC->fc_x, FC->fc_y, c);
	if (FC->fc_x < FC->fc_cols - 1)
		FC->fc_x++;
	else if (FC->fc_awm)
		FC->fc_wrap = 1;
}

static int
fb_clamp(v, lo, hi)
int v, lo, hi;
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void
fb_answer(s)
register char *s;
{
	while (*s)
		fbcons_input(*s++ & 0xFF);
}

static void
fb_answernum(n)
int n;
{
	char b[8];
	register int i = 0;

	if (n >= 100)
		b[i++] = '0' + n / 100 % 10;
	if (n >= 10)
		b[i++] = '0' + n / 10 % 10;
	b[i++] = '0' + n % 10;
	b[i] = 0;
	fb_answer(b);
}

static void
fb_csi(v, c)
register struct fbvt *v;
int c;
{
	register int n, i, p0, p1;

	p0 = v->v_npar > 0 ? v->v_par[0] : 0;
	p1 = v->v_npar > 1 ? v->v_par[1] : 0;
	n = p0 ? p0 : 1;
	if (c != 'r')		/* explicit moves cancel a pending wrap */
		FC->fc_wrap = 0;
	if (v->v_priv) {
		if (c == 'h' || c == 'l')
			for (i = 0; i < v->v_npar; i++)
				if (v->v_par[i] == 25)
					FC->fc_curs = (c == 'h');
				else if (v->v_par[i] == 7)
					FC->fc_awm = (c == 'h');
		return;
	}
	switch (c) {
	case 'A':
		i = FC->fc_y >= FC->fc_top ? FC->fc_top : 0;
		FC->fc_y = fb_clamp(FC->fc_y - n, i, FC->fc_rows - 1);
		break;
	case 'B':
		i = FC->fc_y <= FC->fc_bot ? FC->fc_bot : FC->fc_rows - 1;
		FC->fc_y = fb_clamp(FC->fc_y + n, 0, i);
		break;
	case 'C':
		FC->fc_x = fb_clamp(FC->fc_x + n, 0, FC->fc_cols - 1);
		break;
	case 'D':
		FC->fc_x = fb_clamp(FC->fc_x - n, 0, FC->fc_cols - 1);
		break;
	case 'E':
		FC->fc_x = 0;
		FC->fc_y = fb_clamp(FC->fc_y + n, 0, FC->fc_rows - 1);
		break;
	case 'F':
		FC->fc_x = 0;
		FC->fc_y = fb_clamp(FC->fc_y - n, 0, FC->fc_rows - 1);
		break;
	case 'G':
	case '`':
		FC->fc_x = fb_clamp(n - 1, 0, FC->fc_cols - 1);
		break;
	case 'd':
		FC->fc_y = fb_clamp(n - 1, 0, FC->fc_rows - 1);
		break;
	case 'H':
	case 'f':
		FC->fc_y = fb_clamp((p0 ? p0 : 1) - 1, 0, FC->fc_rows - 1);
		FC->fc_x = fb_clamp((p1 ? p1 : 1) - 1, 0, FC->fc_cols - 1);
		break;
	case 'J':
		if (p0 == 0) {
			fb_erase(FC->fc_x, FC->fc_y, FC->fc_cols - 1, FC->fc_y);
			fb_erase(0, FC->fc_y + 1, FC->fc_cols - 1, FC->fc_rows - 1);
		} else if (p0 == 1) {
			fb_erase(0, 0, FC->fc_cols - 1, FC->fc_y - 1);
			fb_erase(0, FC->fc_y, FC->fc_x, FC->fc_y);
		} else if (p0 == 2)
			fb_erase(0, 0, FC->fc_cols - 1, FC->fc_rows - 1);
		break;
	case 'K':
		if (p0 == 0)
			fb_erase(FC->fc_x, FC->fc_y, FC->fc_cols - 1, FC->fc_y);
		else if (p0 == 1)
			fb_erase(0, FC->fc_y, FC->fc_x, FC->fc_y);
		else if (p0 == 2)
			fb_erase(0, FC->fc_y, FC->fc_cols - 1, FC->fc_y);
		break;
	case 'L':
	case 'M':
		if (FC->fc_y >= FC->fc_top && FC->fc_y <= FC->fc_bot) {
			fb_scroll(FC->fc_y, FC->fc_bot, c == 'L' ? -n : n);
			FC->fc_x = 0;
		}
		break;
	case '@':
		fb_shift(FC->fc_y, FC->fc_x, n);
		break;
	case 'P':
		fb_shift(FC->fc_y, FC->fc_x, -n);
		break;
	case 'X':
		fb_erase(FC->fc_x, FC->fc_y,
		    fb_clamp(FC->fc_x + n - 1, 0, FC->fc_cols - 1), FC->fc_y);
		break;
	case 'm':
		if (v->v_npar == 0)
			v->v_par[v->v_npar++] = 0;
		for (i = 0; i < v->v_npar; i++)
			switch (v->v_par[i]) {
			case 0: FC->fc_inv = FC->fc_bold = 0; break;
			case 1: FC->fc_bold = 1; break;
			case 7: FC->fc_inv = 1; break;
			case 22: FC->fc_bold = 0; break;
			case 27: FC->fc_inv = 0; break;
			}
		break;
	case 'r':
		p0 = p0 ? p0 - 1 : 0;
		p1 = p1 ? p1 - 1 : FC->fc_rows - 1;
		if (p1 > FC->fc_rows - 1)
			p1 = FC->fc_rows - 1;
		if (p0 < p1) {
			FC->fc_top = p0;
			FC->fc_bot = p1;
			fb_home();
		}
		break;
	case 's':
		FC->fc_sx = FC->fc_x;
		FC->fc_sy = FC->fc_y;
		break;
	case 'u':
		FC->fc_x = FC->fc_sx;
		FC->fc_y = FC->fc_sy;
		break;
	case 'n':
		if (v->v_kern)
			break;
		if (p0 == 5)
			fb_answer("\033[0n");
		else if (p0 == 6) {
			fb_answer("\033[");
			fb_answernum(FC->fc_y + 1);
			fb_answer(";");
			fb_answernum(FC->fc_x + 1);
			fb_answer("R");
		}
		break;
	case 'c':
		if (!v->v_kern && p0 == 0)
			fb_answer("\033[?1;0c");	/* VT100, no options */
		break;
	}
}

/* one byte through parser v */
static void
fb_vt(v, c)
register struct fbvt *v;
register int c;
{
	c &= 0xFF;
	if (c == 0x18 || c == 0x1A) {		/* CAN, SUB */
		v->v_state = VS_NORM;
		return;
	}
	if (c == 0x1B) {
		v->v_state = VS_ESC;
		return;
	}
	if (c < 0x20) {				/* C0 controls act in any state */
		switch (c) {
		case '\r':
			FC->fc_x = 0;
			FC->fc_wrap = 0;
			break;
		case '\n': case '\v': case '\f':
			if (v->v_kern)
				FC->fc_x = 0;
			FC->fc_wrap = 0;
			fb_lf();
			break;
		case '\b':
			if (FC->fc_x > 0)
				FC->fc_x--;
			FC->fc_wrap = 0;
			break;
		case '\t':
			FC->fc_x = (FC->fc_x + 8) & ~7;
			if (FC->fc_x > FC->fc_cols - 1)
				FC->fc_x = FC->fc_cols - 1;
			break;
		case 0x0E:
			v->v_gl = 1;
			break;
		case 0x0F:
			v->v_gl = 0;
			break;
		}
		return;
	}
	switch (v->v_state) {
	case VS_NORM:
		if (c == 0x7F || (c >= 0x80 && c < 0xA0))
			return;
		if (v->v_g[v->v_gl] && c >= 0x5F && c <= 0x7E)
			c -= 0x5F;			/* font 0x00-0x1F */
		fb_graph(c);
		return;
	case VS_ESC:
		v->v_state = VS_NORM;
		switch (c) {
		case '[':
			v->v_state = VS_CSI;
			v->v_npar = 0;
			v->v_par[0] = 0;
			v->v_priv = 0;
			break;
		case '(': v->v_state = VS_SCS0; break;
		case ')': v->v_state = VS_SCS1; break;
		case '#': v->v_state = VS_SKIP; break;
		case '7':
			FC->fc_sx = FC->fc_x; FC->fc_sy = FC->fc_y;
			FC->fc_sinv = FC->fc_inv; FC->fc_sbold = FC->fc_bold;
			break;
		case '8':
			FC->fc_x = FC->fc_sx; FC->fc_y = FC->fc_sy;
			FC->fc_inv = FC->fc_sinv; FC->fc_bold = FC->fc_sbold;
			FC->fc_wrap = 0;
			break;
		case 'D': FC->fc_wrap = 0; fb_lf(); break;
		case 'E': FC->fc_wrap = 0; FC->fc_x = 0; fb_lf(); break;
		case 'M': FC->fc_wrap = 0; fb_ri(); break;
		case 'c':
			v->v_g[0] = v->v_g[1] = v->v_gl = 0;
			fb_reset();
			break;
		}
		return;
	case VS_CSI:
		if (c >= '0' && c <= '9') {
			if (v->v_npar == 0)
				v->v_npar = 1;
			if (v->v_npar <= FB_NPAR && v->v_par[v->v_npar - 1] < 10000)
				v->v_par[v->v_npar - 1] = v->v_par[v->v_npar - 1] * 10 + c - '0';
			return;
		}
		if (c == ';') {
			if (v->v_npar == 0)
				v->v_npar = 1;
			if (v->v_npar < FB_NPAR)
				v->v_par[v->v_npar] = 0;
			if (v->v_npar <= FB_NPAR)
				v->v_npar++;		/* FB_NPAR + 1: extra ones dropped */
			return;
		}
		if (c == '?') {
			v->v_priv = 1;
			return;
		}
		if (c >= 0x20 && c < 0x40)		/* other intermediates */
			return;
		if (v->v_npar > FB_NPAR)
			v->v_npar = FB_NPAR;
		v->v_state = VS_NORM;
		fb_csi(v, c);
		return;
	case VS_SCS0:
	case VS_SCS1:
		v->v_g[v->v_state == VS_SCS1] = (c == '0');
		v->v_state = VS_NORM;
		return;
	default:
		v->v_state = VS_NORM;
		return;
	}
}

/* ------------------------------------------------------- ownership */

/*
 * Take the renderer, or queue the byte for the caller that has it.
 * Returns 1 if the caller now owns the renderer.
 */
static int
fb_own(c, kern)
int c, kern;
{
	register int s, next;
	void (*f)();

	if (kern && FB_PANIC() && (f = fbcons_panicfn) != 0) {
		fbcons_panicfn = 0;
		(*f)();
	}
	s = FB_IPLHI();
	if (!FC->fc_busy || (kern && FB_PANIC())) {
		/* a panic takes the screen: its owner will not come back */
		FC->fc_busy = 1;
		FB_IPLX(s);
		return 1;
	}
	next = (FC->fc_pput + 1) % FB_NPEND;
	if (next == FC->fc_pget)
		FC->fc_lost++;
	else {
		FC->fc_pend[FC->fc_pput] = (c & 0xFF) | (kern ? 0x100 : 0);
		FC->fc_pput = next;
	}
	FB_IPLX(s);
	return 0;
}

/* Draw what nested callers queued, then let go, cursor back on. */
static void
fb_release()
{
	register int s, e;

	for (;;) {
		s = FB_IPLHI();
		if (FC->fc_pget == FC->fc_pput) {
			if (FC->fc_curs && !FC->fc_cdrawn)
				fb_cursor();
			FC->fc_busy = 0;
			FB_IPLX(s);
			return;
		}
		e = FC->fc_pend[FC->fc_pget];
		FC->fc_pget = (FC->fc_pget + 1) % FB_NPEND;
		FB_IPLX(s);
		fb_vt((e & 0x100) ? &fbvt_kern : &fbvt_tty, e & 0xFF);
		FB_POLL();
	}
}

/*
 * Line feeds that follow in buf before anything that could move the
 * cursor by row (ESC), at most max.
 */
static int
fb_lfahead(buf, n, max)
register unsigned char *buf;
register int n, max;
{
	register int k = 0, c;

	while (n-- > 0 && k < max) {
		c = *buf++;
		if (c == 0x1B)
			break;
		if (c == '\n' || c == '\v' || c == '\f')
			k++;
	}
	return k;
}

static void
fb_emit(buf, n, kern)
register unsigned char *buf;
register int n, kern;
{
	register struct fbvt *v;
	register int c, k;

	if (!FC->fc_on || n <= 0)
		return;
	if (!fb_own(*buf, kern)) {
		while (--n > 0)
			(void)fb_own(*++buf, kern);
		return;
	}
	if (FC->fc_cdrawn)
		fb_cursor();
	v = kern ? &fbvt_kern : &fbvt_tty;
	while (n-- > 0) {
		c = *buf++;
		/*
		 * Jump scroll: a line feed on the bottom line scrolls once for
		 * it and the k that follow in this block; those k then only
		 * move the cursor down.  The screen ends as with k + 1 single
		 * scrolls.
		 */
		if ((c == '\n' || c == '\v' || c == '\f') && !v->v_kern &&
		    v->v_state == VS_NORM && FC->fc_y == FC->fc_bot &&
		    (k = fb_lfahead(buf, n, FC->fc_bot - FC->fc_top)) > 0) {
			FC->fc_wrap = 0;
			fb_scroll(FC->fc_top, FC->fc_bot, k + 1);
			FC->fc_y = FC->fc_bot - k;
			continue;
		}
		fb_vt(v, c);
		FB_POLL();
	}
	fb_release();
}

/* ------------------------------------------------------------ entries */

void
fbcons_kputc(c)
int c;
{
	unsigned char b = c;

	fb_emit(&b, 1, 1);
}

void
fbcons_putc(c)
int c;
{
	unsigned char b = c;

	fb_emit(&b, 1, 0);
}

void
fbcons_write(buf, n)
unsigned char *buf;
int n;
{
	fb_emit(buf, n, 0);
}

int
fbcons_active()
{
	return FC->fc_on;
}

/* Let go of the renderer, drawing what was queued meanwhile. */
void
fbcons_unlock()
{
	if (!FC->fc_on)
		return;
	FC->fc_busy = 1;
	fb_release();
}

/*
 * Stop, MMU off: whatever held the renderer will not finish; draw what
 * was queued so the last messages are on the screen.  The cells are in
 * mapped memory, out of reach now.
 */
void
fbcons_stop()
{
	FC->fc_text = 0;
	fbcons_unlock();
}

/*
 * Take the renderer for a caller that moves the screen (fc_m.fm_base);
 * nested output queues until fbcons_unlock().
 */
int
fbcons_grab()
{
	register int s, ok;

	s = FB_IPLHI();
	ok = FC->fc_on && !FC->fc_busy;
	if (ok)
		FC->fc_busy = 1;
	FB_IPLX(s);
	return ok;
}

/* pixel i of the cell row at p */
static unsigned long
fb_getpix(p, i)
register VOL unsigned char *p;
register int i;
{
	register int d = FC->fc_m.fm_depth;

	switch (d) {
	case 8:
		return p[i];
	case 16:
		return (unsigned long)p[2 * i] << 8 | p[2 * i + 1];
	case 32:
		p += 4 * i;
		return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
		    (unsigned long)p[2] << 8 | p[3];
	default:
		i *= d;
		return p[i >> 3] >> (8 - d - (i & 7)) & ((1 << d) - 1);
	}
}

/* the cell fbcons drew at (x, y), read back from the screen; blank if none */
static int
fb_readcell(x, y)
int x, y;
{
	unsigned char b[FB_CH];
	register VOL unsigned char *p;
	register unsigned char *g;
	register unsigned long v, fg, bg;
	register int r, i, c, k, gb, any, all;

	v = FC->fc_m.fm_depth == 32 ? ~0UL : (1UL << FC->fc_m.fm_depth) - 1;
	fg = FC->fc_fg & v;
	bg = FC->fc_bg & v;
	any = 0;
	all = 0xFF;
	p = PIX(x * FB_CW, y * FB_CH);
	for (r = 0; r < FB_CH; r++, p += FC->fc_m.fm_row) {
		b[r] = 0;
		for (i = 0; i < FB_CW; i++)
			if ((v = fb_getpix(p, i)) == fg)
				b[r] |= 0x80 >> i;
			else if (v != bg)
				return FB_TBLANK;
		any |= b[r];
		all &= b[r];
	}
	if (any == 0)
		return FB_TBLANK;
	if (all == 0xFF)
		return FB_TBLANK | FB_TINV;
	/* k: bit 0 bold, bit 1 inverse; the middle row rules most glyphs out */
	for (k = 0; k < 4; k++)
		for (c = 0x20; c < 0x120; c++) {
			g = fb_font[c & 0xFF];
			for (r = FB_CH / 2; r < FB_CH + FB_CH / 2; r++) {
				gb = g[r % FB_CH];
				if (k & 1)
					gb |= gb >> 1;
				if (k & 2)
					gb = ~gb & 0xFF;
				if (gb != b[r % FB_CH])
					break;
			}
			if (r == FB_CH + FB_CH / 2)
				return (c & 0xFF) | (k & 1 ? FB_TBOLD : 0) | (k & 2 ? FB_TINV : 0);
		}
	return FB_TBLANK;
}

/*
 * Keep cells (fc_rows by fc_cols) from now on, filled from what is on
 * the screen.  The caller holds the renderer.
 */
void
fbcons_text(t)
unsigned short *t;
{
	register int x, y;

	if (FC->fc_cdrawn)
		fb_cursor();
	for (y = 0; y < FC->fc_rows; y++)
		for (x = 0; x < FC->fc_cols; x++)
			t[(long)y * FC->fc_cols + x] = fb_readcell(x, y);
	FC->fc_text = t;
}

/* Another owner has the screen: draw only into the cells.  Renderer held. */
void
fbcons_hide()
{
	if (FC->fc_cdrawn)
		fb_cursor();
	FC->fc_hide = 1;
}

/* The screen is back: repaint it from the cells.  Renderer held, or a panic. */
void
fbcons_show()
{
	register unsigned long w;
	register int x, y, v;

	FC->fc_hide = 0;
	FC->fc_cdrawn = 0;
	w = (FC->fc_m.fm_width * FC->fc_m.fm_depth + 7) / 8;
	for (y = 0; y < FC->fc_m.fm_height; y++)
		fb_fill(PIX(0, y), (long)w, FC->fc_bg);
	if (FC->fc_text == 0)
		return;
	for (y = 0; y < FC->fc_rows; y++)
		for (x = 0; x < FC->fc_cols; x++)
			if ((v = TXT(x, y)) != FB_TBLANK)
				fb_paint(x, y, v);
}

/*
 * Accept a mode, set the colours and clear the screen.  The caller has
 * checked that the region is frame-buffer memory.
 */
int
fbcons_attach(m)
register struct fbmode *m;
{
	register unsigned long d, w;
	register int y;

	d = m->fm_depth;
	if (d != 1 && d != 2 && d != 4 && d != 8 && d != 16 && d != 32)
		return 0;
	if (m->fm_width < 20 * FB_CW || m->fm_height < 5 * FB_CH ||
	    m->fm_width > 4096 || m->fm_height > 4096)
		return 0;
	if (m->fm_row < m->fm_width * d / 8 || (m->fm_row & 1) || (m->fm_base & 1))
		return 0;
	FC->fc_on = 0;
	FC->fc_text = 0;
	FC->fc_hide = 0;
	FC->fc_m = *m;
	FC->fc_m.fm_base = FB_MAP(m->fm_base);
	FC->fc_cols = m->fm_width / FB_CW;
	FC->fc_rows = m->fm_height / FB_CH;
#ifdef BOOTDIAG
	FC->fc_rows -= 6;		/* status lines below the console */
#endif
	if (d <= 8) {
		FC->fc_fg = fb_repl(d == 8 ? 0xFFUL : (1UL << d) - 1, d);
		FC->fc_bg = 0;
	} else {
		FC->fc_fg = 0;
#ifdef ATA060
		FC->fc_bg = d == 16 ? 0xFFFFFFFFUL : 0x00FFFFFFUL;	/* RGB565 */
#else
		FC->fc_bg = d == 16 ? 0x7FFF7FFFUL : 0x00FFFFFFUL;
#endif
	}
	fb_mkexp();
	FC->fc_cdrawn = 0;
	FC->fc_busy = 0;
	FC->fc_pput = FC->fc_pget = 0;
	/* whole visible area, including the margins beyond the last cell */
	w = (m->fm_width * d + 7) / 8;
	for (y = 0; y < m->fm_height; y++)
		fb_fill(PIX(0, y), (long)w, FC->fc_bg);
	fb_reset();
	fbvt_kern.v_state = fbvt_tty.v_state = VS_NORM;
	FC->fc_on = 1;
	if (FC->fc_curs)
		fb_cursor();
	return 1;
}
