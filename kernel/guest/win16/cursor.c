/*
 * cursor.c -- cursors and icons: the system's (drawn here, our own
 * shapes) and a program's from its RT_GROUP_CURSOR and RT_GROUP_ICON
 * resources.  An icon keeps an 8-bit colour image and a mask; a cursor
 * a monochrome AND and XOR pair for the device's pointer.
 */

#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "scr.h"

#define	NICO	512
#define	IBASE	0xc000

struct ico {
	int	w, h, hx, hy;
	u8	*pix;		/* palette indexes */
	u8	*mask;		/* 1: transparent */
	int	cursor;
	struct cursor c;	/* for the device */
	u16	hinst;
	u32	id;
};

static struct ico *icos[NICO];
static u16 cur_now;

/* the shapes: . transparent, # black, o white, x inverted; colours for icons by letter */
static char *arrow[] = {
	"#...........",
	"##..........",
	"#o#.........",
	"#oo#........",
	"#ooo#.......",
	"#oooo#......",
	"#ooooo#.....",
	"#oooooo#....",
	"#ooooooo#...",
	"#oooooooo#..",
	"#ooooo#####.",
	"#oo#oo#.....",
	"#o#.#oo#....",
	"##..#oo#....",
	"#....#oo#...",
	".....#oo#...",
	"......##....", 0
};
static char *ibeam[] = {
	"##.##", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..",
	"..#..", "..#..", "..#..", "..#..", "##.##", 0
};
static char *hourglass[] = {
	"###########",
	"#ooooooooo#",
	".#ooooooo#.",
	".#o#o#o#o#.",
	"..#o#o#o#..",
	"...#o#o#...",
	"....#o#....",
	"....#o#....",
	"...#ooo#...",
	"..#oo#oo#..",
	".#oo#o#oo#.",
	".#o#o#o#o#.",
	"#o#o#o#o#o#",
	"###########", 0
};
static char *cross[] = {
	"....#....", "....#....", "....#....", "....#....", "#########", "....#....", "....#....",
	"....#....", "....#....", 0
};
static char *uparrow[] = {
	"....#....", "...###...", "..#####..", ".#######.", "....#....", "....#....", "....#....",
	"....#....", "....#....", "....#....", 0
};
static char *sizewe[] = {
	"...#.......#...",
	"..##.......##..",
	".#o#########o#.",
	"#ooooooooooooo#",
	".#o#########o#.",
	"..##.......##..",
	"...#.......#...", 0
};
static char *sizens[] = {
	"...#...", "..#o#..", ".#ooo#.", "###o###", "..#o#..", "..#o#..", "..#o#..", "..#o#..",
	"..#o#..", "..#o#..", "###o###", ".#ooo#.", "..#o#..", "...#...", 0
};
static char *sizenwse[] = {
	"######....",
	"#oooo#....",
	"#ooo#.....",
	"#oooo#....",
	"#o#ooo#...",
	"##.#ooo#.#",
	"....#ooo##",
	".....#oooo#",
	"....#ooo#",
	"....#oooo#",
	"....######", 0
};
static char *sizenesw[] = {
	"....######",
	"....#oooo#",
	".....#ooo#",
	"....#oooo#",
	"...#ooo#o#",
	"#.#ooo#.##",
	"##ooo#....",
	"#oooo#....",
	"#ooo#.....",
	"#oooo#....",
	"######....", 0
};
static char *sizeall[] = {
	"....#....", "...###...", "..#.#.#..", "....#....", "#.#####.#", "....#....", "..#.#.#..",
	"...###...", "....#....", 0
};

/* icons: a window, a stop sign, question, exclamation, information */
static char *i_app[] = {
	"################################",
	"#bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb#",
	"#bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb#",
	"#bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb#",
	"################################",
	"#oooooooooooooooooooooooooooooo#",
	"#oooooooooooooooooooooooooooooo#",
	"#ooggggggggggggggggggggggggggoo#",
	"#oogooooooooooooooooooooooooggo#",
	"#oogooooooooooooooooooooooooggo#",
	"#oogooccccccccccooooooooooooggo#",
	"#oogoooooooooooooooooooooooogoo#",
	"#oogooccccccccccccccccooooogoo#.",
	"#oogoooooooooooooooooooooooogoo#",
	"#oogooccccccccccccooooooooogoo#.",
	"#oogoooooooooooooooooooooooogoo#",
	"#oogooccccccccccccccccccooogoo#.",
	"#oogoooooooooooooooooooooooogoo#",
	"#oogooccccccccooooooooooooogoo#.",
	"#oogoooooooooooooooooooooooogoo#",
	"#oogggggggggggggggggggggggggggo#",
	"#oooooooooooooooooooooooooooooo#",
	"#oooooooooooooooooooooooooooooo#",
	"################################", 0
};
static char *i_hand[] = {
	"..........##########..........",
	"........##rrrrrrrrrr##........",
	"......##rrrrrrrrrrrrrr##......",
	".....#rrrrrrrrrrrrrrrrrr#.....",
	"....#rrrrrrrrrrrrrrrrrrrr#....",
	"...#rrrrrrrrrrrrrrrrrrrrrr#...",
	"..#rrrrooorrrrrrrrrrooorrrr#..",
	"..#rrrrroooorrrrrroooorrrrr#..",
	".#rrrrrrroooorrrroooorrrrrrr#.",
	".#rrrrrrrroooorroooorrrrrrrr#.",
	"#rrrrrrrrrroooooooorrrrrrrrrr#",
	"#rrrrrrrrrrroooooorrrrrrrrrrr#",
	"#rrrrrrrrrrroooooorrrrrrrrrrr#",
	"#rrrrrrrrrroooooooorrrrrrrrrr#",
	".#rrrrrrrroooorroooorrrrrrrr#.",
	".#rrrrrrroooorrrroooorrrrrrr#.",
	"..#rrrrroooorrrrrroooorrrrr#..",
	"..#rrrrooorrrrrrrrrrooorrrr#..",
	"...#rrrrrrrrrrrrrrrrrrrrrr#...",
	"....#rrrrrrrrrrrrrrrrrrrr#....",
	".....#rrrrrrrrrrrrrrrrrr#.....",
	"......##rrrrrrrrrrrrrr##......",
	"........##rrrrrrrrrr##........",
	"..........##########..........", 0
};
static char *i_quest[] = {
	"..........##########..........",
	"........##oooooooooo##........",
	"......##oooooooooooooo##......",
	".....#oooooobbbbbbooooooo#....",
	"....#ooooobbbbbbbbbbooooo#....",
	"...#ooooobbbboooobbbbooooo#...",
	"..#oooooobbbooooobbbbbooooo#..",
	"..#ooooooooooooooobbbbooooo#..",
	".#ooooooooooooooobbbbooooooo#.",
	".#oooooooooooooobbbbbooooooo#.",
	"#ooooooooooooobbbbboooooooooo#",
	"#oooooooooooobbbbooooooooooooo#",
	"#oooooooooooobbbboooooooooooo#",
	"#oooooooooooobbbboooooooooooo#",
	".#oooooooooooooooooooooooooo#.",
	".#oooooooooooobbbboooooooooo#.",
	"..#ooooooooooobbbbooooooooo#..",
	"..#ooooooooooobbbbooooooooo#..",
	"...#oooooooooooooooooooooo#...",
	"....#oooooooooooooooooooo#....",
	".....#oooooooooooooooooo#.....",
	"......##oooooooooooooo##......",
	"........##oooooooooo##........",
	"..........##########..........", 0
};
static char *i_excl[] = {
	"...............##...............",
	"..............#yy#..............",
	".............#yyyy#.............",
	"............#yyyyyy#............",
	"...........#yyyyyyyy#...........",
	"...........#yyy##yyy#...........",
	"..........#yyy####yyy#..........",
	".........#yyyy####yyyy#.........",
	".........#yyyy####yyyy#.........",
	"........#yyyyy####yyyyy#........",
	".......#yyyyyy####yyyyyy#.......",
	".......#yyyyyy####yyyyyy#.......",
	"......#yyyyyyy####yyyyyyy#......",
	".....#yyyyyyyyy##yyyyyyyyy#.....",
	".....#yyyyyyyyy##yyyyyyyyy#.....",
	"....#yyyyyyyyyyyyyyyyyyyyyy#....",
	"...#yyyyyyyyyy####yyyyyyyyyy#...",
	"...#yyyyyyyyyy####yyyyyyyyyy#...",
	"..#yyyyyyyyyyyy##yyyyyyyyyyyy#..",
	".#yyyyyyyyyyyyyyyyyyyyyyyyyyyy#.",
	"################################", 0
};
static char *i_info[] = {
	"..........##########..........",
	"........##oooooooooo##........",
	"......##oooooobbooooooo##.....",
	".....#oooooobbbbbooooooo#.....",
	"....#oooooobbbbbbbooooooo#....",
	"...#ooooooooobbbbooooooooo#...",
	"..#oooooooooooooooooooooooo#..",
	"..#oooooooooooooooooooooooo#..",
	".#ooooooooobbbbbbooooooooooo#.",
	".#oooooooooooobbbooooooooooo#.",
	"#ooooooooooooobbboooooooooooo#",
	"#ooooooooooooobbboooooooooooo#",
	"#ooooooooooooobbboooooooooooo#",
	"#ooooooooooooobbboooooooooooo#",
	".#oooooooooooobbbooooooooooo#.",
	".#oooooooooooobbbooooooooooo#.",
	"..#oooooooooobbbbbooooooooo#..",
	"..#ooooooooobbbbbbboooooooo#..",
	"...#oooooooooooooooooooooo#...",
	"....#oooooooooooooooooooo#....",
	".....#oooooooooooooooooo#.....",
	"......##oooooooooooooo##......",
	"........##oooooooooo##........",
	"..........##########..........", 0
};

static int
letter(c)
	int c;
{
	switch (c) {
	case '#': return 0;
	case 'o': return 255;
	case 'b': return 4;		/* dark blue */
	case 'r': return 249;		/* red */
	case 'y': return 251;		/* yellow */
	case 'g': return 248;		/* grey */
	case 'c': return 6;		/* dark cyan */
	}
	return 0;
}

static u16
newico(i)
	struct ico *i;
{
	int k;

	for (k = 1; k < NICO; k++)
		if (!icos[k]) {
			icos[k] = i;
			return IBASE + 4 * k;
		}
	return 0;
}

static struct ico *
ico_get(h)
	u32 h;
{
	int k;

	h &= 0xffff;
	if (h < IBASE || (h - IBASE) & 3)
		return 0;
	k = (h - IBASE) >> 2;
	return k < NICO ? icos[k] : 0;
}

/* the device's cursor from the image: AND 1 where transparent, XOR 1 white or inverted */
static void
mkdev(i)
	struct ico *i;
{
	int n = i->w * i->h, k;

	i->c.w = i->w;
	i->c.h = i->h;
	i->c.hx = i->hx;
	i->c.hy = i->hy;
	i->c.and = (u8 *)malloc(n);
	i->c.xor = (u8 *)malloc(n);
	for (k = 0; k < n; k++) {
		i->c.and[k] = i->mask[k];
		i->c.xor[k] = i->mask[k] ? i->pix[k] == 2 : i->pix[k] != 0;
	}
}

static u16
fromart(art, w, h, hx, hy, cursor)
	char **art;
	int w, h, hx, hy, cursor;
{
	struct ico *i = (struct ico *)calloc(1, sizeof *i);
	int x, y, len;

	i->w = w;
	i->h = h;
	i->hx = hx;
	i->hy = hy;
	i->cursor = cursor;
	i->pix = (u8 *)calloc(1, w * h);
	i->mask = (u8 *)malloc(w * h);
	memset(i->mask, 1, w * h);
	for (y = 0; art[y] && y < h; y++) {
		len = strlen(art[y]);
		for (x = 0; x < len && x < w; x++) {
			int c = art[y][x];

			if (c == '.')
				continue;
			i->mask[y * w + x] = c == 'x';
			i->pix[y * w + x] = cursor ? (c == 'o' ? 1 : c == 'x' ? 2 : 0) : letter(c);
		}
	}
	if (cursor)
		mkdev(i);
	return newico(i);
}

static u16 sysc[16], sysi[8];
static u32 sysc_id[] = { 32512, 32513, 32514, 32515, 32516, 32640, 32641, 32642, 32643, 32644, 32645, 32646, 0 };

void
cursors_init()
{
	sysc[0] = fromart(arrow, 32, 32, 0, 0, 1);
	sysc[1] = fromart(ibeam, 32, 32, 2, 7, 1);
	sysc[2] = fromart(hourglass, 32, 32, 5, 7, 1);
	sysc[3] = fromart(cross, 32, 32, 4, 4, 1);
	sysc[4] = fromart(uparrow, 32, 32, 4, 0, 1);
	sysc[5] = fromart(sizeall, 32, 32, 4, 4, 1);
	sysc[6] = sysc[5];
	sysc[7] = fromart(sizenwse, 32, 32, 5, 5, 1);
	sysc[8] = fromart(sizenesw, 32, 32, 5, 5, 1);
	sysc[9] = fromart(sizewe, 32, 32, 7, 3, 1);
	sysc[10] = fromart(sizens, 32, 32, 3, 7, 1);
	sysc[11] = sysc[5];
	cur_arrow = sysc[0];
	sysi[0] = fromart(i_app, 32, 32, 16, 16, 0);
	sysi[1] = fromart(i_hand, 32, 32, 16, 16, 0);
	sysi[2] = fromart(i_quest, 32, 32, 16, 16, 0);
	sysi[3] = fromart(i_excl, 32, 32, 16, 16, 0);
	sysi[4] = fromart(i_info, 32, 32, 16, 16, 0);
	cur_set(cur_arrow);
}

void
cur_set(h)
	u32 h;
{
	struct ico *i = ico_get(h);

	cur_now = h;
	if (!h) {
		scr_setcursor((struct cursor *)0);
		return;
	}
	if (!i)
		return;
	if (!i->c.and)
		mkdev(i);
	scr_setcursor(&i->c);
}

u16
cur_get()
{
	return cur_now;
}

/* ---- from resources ---- */

/* a DIB icon or cursor image: header at a, the image twice as high (XOR then AND) */
static u16
fromdib(a, len, hx, hy, cursor)
	u32 a, len;
	int hx, hy, cursor;
{
	int w = GL(a + 4), h2 = GL(a + 8), h = h2 / 2, bpp = GW(a + 14), ncol, x, y, stride, astride;
	u32 ct, xb, ab;
	u8 map[256];
	struct ico *i;

	if (GL(a) != 40 || w <= 0 || w > 128 || h <= 0 || h > 128)
		return 0;
	ncol = GL(a + 32) ? GL(a + 32) : bpp <= 8 ? 1 << bpp : 0;
	ct = a + 40;
	for (x = 0; x < ncol && x < 256; x++)
		map[x] = pal_index(RGB(M[ct + 4 * x + 2], M[ct + 4 * x + 1], M[ct + 4 * x]));
	stride = ((w * bpp + 31) / 32) * 4;
	astride = ((w + 31) / 32) * 4;
	xb = ct + 4 * ncol;
	ab = xb + stride * h;
	if (ab + astride * h > a + len + 4)
		return 0;
	i = (struct ico *)calloc(1, sizeof *i);
	i->w = w;
	i->h = h;
	i->hx = hx;
	i->hy = hy;
	i->cursor = cursor;
	i->pix = (u8 *)calloc(1, w * h);
	i->mask = (u8 *)calloc(1, w * h);
	for (y = 0; y < h; y++) {
		u32 xr = xb + (h - 1 - y) * stride, ar = ab + (h - 1 - y) * astride;

		for (x = 0; x < w; x++) {
			int v;

			switch (bpp) {
			case 1: v = (M[xr + x / 8] >> (7 - x % 8)) & 1; break;
			case 4: v = (M[xr + x / 2] >> (x & 1 ? 0 : 4)) & 15; break;
			case 8: v = M[xr + x]; break;
			default: v = 0;
			}
			i->mask[y * w + x] = (M[ar + x / 8] >> (7 - x % 8)) & 1;
			if (cursor)
				i->pix[y * w + x] = v ? (i->mask[y * w + x] ? 2 : 1) : 0;
			else
				i->pix[y * w + x] = map[v];
		}
	}
	if (cursor)
		mkdev(i);
	return newico(i);
}

static u16
fromres(hinst, name, cursor)
	u32 hinst, name;
	int cursor;
{
	struct module *m = mod_byhandle(hinst);
	u32 dir, size, best = 0, img, ilen;
	int n, k, bw = 0, score, bs = -1, id, hx = 0, hy = 0;
	struct ico *i;

	if (!m)
		return 0;
	for (k = 1; k < NICO; k++)
		if ((i = icos[k]) && i->hinst == (hinst & 0xffff) && i->id == name && i->cursor == cursor)
			return IBASE + 4 * k;
	dir = res_data(m, FP(0, cursor ? RT_GROUP_CURSOR : RT_GROUP_ICON), name, &size);
	if (!dir)
		return 0;
	n = GW(dir + 4);
	for (k = 0; k < n; k++) {
		u32 e = dir + 6 + 14 * k;
		int w = cursor ? GW(e) : M[e], bpp = GW(e + 6);

		if (cursor && bpp == 0)
			bpp = 1;
		if (!cursor && bpp == 0)
			bpp = M[e + 2] == 16 ? 4 : M[e + 2] == 2 ? 1 : 8;
		score = (w == 32 ? 100 : w == 0 ? 50 : 10) + (bpp <= 8 ? bpp : 0);
		if (score > bs) {
			bs = score;
			best = e;
			bw = w;
		}
	}
	if (!best)
		return 0;
	id = GW(best + 12);
	img = res_data(m, FP(0, cursor ? RT_CURSOR : RT_ICON), FP(0, id), &ilen);
	if (!img)
		return 0;
	if (cursor) {
		hx = GW(img);
		hy = GW(img + 2);
		img += 4;
	}
	(void)bw;
	k = fromdib(img, ilen, hx, hy, cursor);
	if (k) {
		ico_get(k)->hinst = hinst;
		ico_get(k)->id = name;
	}
	return k;
}

u16
cur_load(hinst, name)
	u32 hinst, name;
{
	int k;

	if (!(hinst & 0xffff) || FPSEL(name) == 0) {
		if (!(hinst & 0xffff) || (FPOFF(name) >= 32512 && FPOFF(name) <= 32646))
			for (k = 0; sysc_id[k]; k++)
				if (sysc_id[k] == FPOFF(name))
					return sysc[k];
		if (!(hinst & 0xffff))
			return 0;
	}
	return fromres(hinst, name, 1);
}

u16
icon_load(hinst, name)
	u32 hinst, name;
{
	if (!(hinst & 0xffff) || (FPSEL(name) == 0 && FPOFF(name) >= 32512 && FPOFF(name) <= 32516)) {
		if (FPSEL(name) == 0 && FPOFF(name) >= 32512 && FPOFF(name) <= 32516)
			return sysi[FPOFF(name) - 32512];
		return 0;
	}
	return fromres(hinst, name, 0);
}

void
icon_draw(dc, h, x, y)
	struct dc *dc;
	u32 h;
	int x, y;
{
	struct ico *i = ico_get(h);
	struct rgn *g;
	int px, py, k;
	struct rect r;

	if (!i || !dc)
		return;
	g = dc_clip(dc);
	for (py = 0; py < i->h; py++)
		for (px = 0; px < i->w; px++) {
			k = py * i->w + px;
			if (i->mask[k])
				continue;
			if (!rgn_ptin(g, x + px, y + py))
				continue;
			dc->s->pix[(y + py) * dc->s->rowb + x + px] = dc->s->mono ?
			    (syspal[i->pix[k]][0] + syspal[i->pix[k]][1] + syspal[i->pix[k]][2] >= 384) : i->pix[k];
		}
	if (dc->s == &screen) {
		r_set(&r, x, y, x + i->w, y + i->h);
		scr_dirty(&r);
	}
}

int
ico_valid(h)
	u32 h;
{
	return ico_get(h) != 0;
}

void
ico_destroy(h)
	u32 h;
{
	struct ico *i = ico_get(h);
	int k;

	if (!i)
		return;
	for (k = 0; k < 16; k++)
		if (sysc[k] == (h & 0xffff))
			return;
	for (k = 0; k < 8; k++)
		if (sysi[k] == (h & 0xffff))
			return;
	if (i->hinst)
		return;		/* resource ones stay with their module */
	free(i->pix);
	free(i->mask);
	if (i->c.and) {
		free(i->c.and);
		free(i->c.xor);
	}
	icos[((h & 0xffff) - IBASE) >> 2] = 0;
	free(i);
}

/* CreateIcon/CreateCursor from monochrome or device bits */
u16
ico_create(w, h, hx, hy, and, xor, bpp, cursor)
	int w, h, hx, hy, bpp, cursor;
	u32 and, xor;
{
	struct ico *i = (struct ico *)calloc(1, sizeof *i);
	int x, y, astride = ((w + 15) / 16) * 2, xstride = bpp == 1 ? astride : (w + 1) & ~1;

	i->w = w;
	i->h = h;
	i->hx = hx;
	i->hy = hy;
	i->cursor = cursor;
	i->pix = (u8 *)calloc(1, w * h);
	i->mask = (u8 *)calloc(1, w * h);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int a = (M[and + y * astride + x / 8] >> (7 - x % 8)) & 1, v;

			v = bpp == 1 ? (M[xor + y * xstride + x / 8] >> (7 - x % 8)) & 1 : M[xor + y * xstride + x];
			i->mask[y * w + x] = a;
			if (cursor)
				i->pix[y * w + x] = v ? (a ? 2 : 1) : 0;
			else
				i->pix[y * w + x] = bpp == 1 ? (v ? 255 : 0) : v;
		}
	if (cursor)
		mkdev(i);
	return newico(i);
}
