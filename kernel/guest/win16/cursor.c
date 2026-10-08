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

struct ico {
	int	w, h, hx, hy;
	u8	*pix;		/* palette indexes */
	u8	*mask;		/* 1: transparent */
	int	cursor;
	struct cursor c;	/* for the device */
	u16	hinst;
	u32	id;
	u16	hnd;		/* the handle: a global block, laid out as Windows' (CURSORICONINFO, AND, XOR) */
};

static struct ico *icos[NICO];
static struct ico **bysel;	/* by the handle's selector index */
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

/*
 * A handle for the image: a global block as Windows 3.1 keeps an icon or
 * cursor (programs lock it: DumpIcon, Program Manager's groups).  The
 * header is CURSORICONINFO (hot spot, width, height, bytes a row,
 * planes, bits a pixel), then the AND mask, then the XOR bits: one bit a
 * pixel for cursors, a byte (a palette index) for icons.
 */
static u16
newico_in(i, h)
	struct ico *i;
	u16 h;			/* a block to hold it, or 0 for a new one */
{
	int k, x, y, ab = ((i->w + 15) / 16) * 2, xb = i->cursor ? ab : (i->w + 1) & ~1;
	u32 p, a, xo, size = 12 + ab * i->h + xb * i->h;

	for (k = 1; k < NICO; k++)
		if (!icos[k])
			break;
	if (k == NICO)
		return 0;
	if (!bysel)
		bysel = (struct ico **)calloc(LDTSIZE, sizeof *bysel);
	if (h) {
		if (!g_realloc(h, size, GMEM_MOVEABLE))
			return 0;
		memset(M + sel_base(h), 0, size);
	} else
		h = g_alloc(GMEM_MOVEABLE | GMEM_DDESHARE | GMEM_ZEROINIT, size, 0);
	if (!h)
		return 0;
	p = sel_base(h);
	PW(p, i->hx);
	PW(p + 2, i->hy);
	PW(p + 4, i->w);
	PW(p + 6, i->h);
	PW(p + 8, xb);
	PB(p + 10, 1);
	PB(p + 11, i->cursor ? 1 : 8);
	a = p + 12;
	xo = a + ab * i->h;
	for (y = 0; y < i->h; y++)
		for (x = 0; x < i->w; x++) {
			int px = i->pix[y * i->w + x];

			if (i->mask[y * i->w + x])
				M[a + y * ab + x / 8] |= 0x80 >> (x & 7);
			if (i->cursor) {
				if (px)
					M[xo + y * xb + x / 8] |= 0x80 >> (x & 7);
			} else
				M[xo + y * xb + x] = px;
		}
	icos[k] = i;
	i->hnd = h;
	bysel[SELIX(h)] = i;
	return h;
}

static u16
newico(i)
	struct ico *i;
{
	return newico_in(i, 0);
}

static struct ico *adopt();
static void mkdev();

static struct ico *
ico_get(h)
	u32 h;
{
	int ix = SELIX(h & 0xffff);

	if (!(h & 0xffff) || ix <= 0 || ix >= LDTSIZE || !gblk[ix].gb_used || gblk[ix].gb_discarded)
		return 0;
	if (!bysel)
		bysel = (struct ico **)calloc(LDTSIZE, sizeof *bysel);
	/* a block a program made in the icon layout (Program Manager's from its groups) is an icon too */
	return bysel[ix] ? bysel[ix] : adopt(h & 0xffff);
}

/* a global block holding CURSORICONINFO, AND and XOR, taken as the icon it describes */
/* the image from the block as it is now into i (0), or -1 if it is not an icon's layout */
static int
decode(h, i)
	u16 h;
	struct ico *i;
{
	struct gblock *b = g_block(h);
	u32 p, a, xo;
	int w, ht, xb, planes, bpp, ab, x, y, v;

	if (!b || b->gb_discarded || b->gb_size < 12)
		return -1;
	p = b->gb_base;
	w = GW(p + 4);
	ht = GW(p + 6);
	xb = GW(p + 8);
	planes = M[p + 10];
	bpp = M[p + 11];
	ab = ((w + 15) / 16) * 2;
	if (w < 1 || w > 128 || ht < 1 || ht > 128 || planes != 1 || (bpp != 1 && bpp != 4 && bpp != 8) ||
	    xb < (w * bpp + 7) / 8 || 12 + (ab + xb) * ht > (int)b->gb_size)
		return -1;
	if (i->w != w || i->h != ht || !i->pix) {
		free(i->pix);
		free(i->mask);
		i->pix = (u8 *)calloc(1, w * ht);
		i->mask = (u8 *)calloc(1, w * ht);
	}
	i->w = w;
	i->h = ht;
	i->hx = GW(p);
	i->hy = GW(p + 2);
	if (!i->hnd)
		i->cursor = bpp == 1;
	a = p + 12;
	xo = a + ab * ht;
	for (y = 0; y < ht; y++)
		for (x = 0; x < w; x++) {
			i->mask[y * w + x] = (M[a + y * ab + x / 8] >> (7 - x % 8)) & 1;
			switch (bpp) {
			case 1:
				v = (M[xo + y * xb + x / 8] >> (7 - x % 8)) & 1;
				v = i->cursor ? (v ? (i->mask[y * w + x] ? 2 : 1) : 0) : (v ? 255 : 0);
				break;
			case 4:
				v = (M[xo + y * xb + x / 2] >> (x & 1 ? 0 : 4)) & 15;
				v = v < 8 ? v : 240 + v;
				break;
			default:
				v = M[xo + y * xb + x];
			}
			i->pix[y * w + x] = v;
		}
	return 0;
}

static struct ico *
adopt(h)
	u16 h;
{
	struct ico *i;
	int k;

	for (k = 1; k < NICO; k++)
		if (!icos[k])
			break;
	if (k == NICO)
		return 0;
	i = (struct ico *)calloc(1, sizeof *i);
	if (decode(h, i) != 0) {
		free(i->pix);
		free(i->mask);
		free((char *)i);
		return 0;
	}
	if (i->cursor)
		mkdev(i);
	icos[k] = i;
	i->hnd = h;
	bysel[SELIX(h)] = i;
	return i;
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
static u16 fromdib_in();

static u16
fromdib(a, len, hx, hy, cursor)
	u32 a, len;
	int hx, hy, cursor;
{
	return fromdib_in(a, len, hx, hy, cursor, 0);
}

static u16
fromdib_in(a, len, hx, hy, cursor, into)
	u32 a, len;
	int hx, hy, cursor;
	u16 into;
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
	return newico_in(i, into);
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
			return i->hnd;
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
	/* as Windows draws it: from the block as it is now (programs write icons into it) */
	if (!i->cursor)
		decode(i->hnd, i);
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
	for (k = 1; k < NICO; k++)
		if (icos[k] == i)
			icos[k] = 0;
	bysel[SELIX(h & 0xffff)] = 0;
	g_free(h & 0xffff);
	free(i);
}

/* CreateIcon/CreateCursor from monochrome or device bits */
u16
ico_create(w, h, hx, hy, and, xor, bpp, cursor)
	int w, h, hx, hy, bpp, cursor;
	u32 and, xor;
{
	struct ico *i = (struct ico *)calloc(1, sizeof *i);
	int x, y, astride = ((w + 15) / 16) * 2;
	int xstride = bpp == 1 ? astride : bpp == 4 ? ((w * 4 + 15) / 16) * 2 : (w + 1) & ~1;

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

			if (bpp == 1)
				v = (M[xor + y * xstride + x / 8] >> (7 - x % 8)) & 1;
			else if (bpp == 4) {
				/* the 16 VGA colours: the static ones at each end of the palette */
				v = (M[xor + y * xstride + x / 2] >> (x & 1 ? 0 : 4)) & 15;
				v = v < 8 ? v : 240 + v;
			} else
				v = M[xor + y * xstride + x];
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

/* ---- what SHELL and Program Manager use ---- */

/* GetIconID(hRes, resType): of a loaded icon or cursor directory, the image for this display */
static u32
c_GetIconID(a)
	u32 *a;
{
	struct gblock *b = g_block(a[0]);
	int cursor = (a[1] & 0xffff) == RT_CURSOR, n, k, score, bs = -1, id = 0;
	u32 dir;

	if (!b || b->gb_discarded || b->gb_size < 6)
		return 0;
	dir = b->gb_base;
	n = GW(dir + 4);
	for (k = 0; k < n && 6 + 14 * (k + 1) <= (int)b->gb_size; k++) {
		u32 e = dir + 6 + 14 * k;
		int w = cursor ? GW(e) : M[e], bpp = GW(e + 6);

		if (!cursor && bpp == 0)
			bpp = M[e + 2] == 16 ? 4 : M[e + 2] == 2 ? 1 : 8;
		score = (w == 32 ? 100 : w == 0 ? 50 : 10) + (bpp <= 8 ? bpp : 0);
		if (score > bs) {
			bs = score;
			id = GW(e + 12);
		}
	}
	return id;
}

/* LoadIconHandler(hRes, fNew): a loaded RT_ICON resource (a DIB when fNew) made an icon in place */
static u32
c_LoadIconHandler(a)
	u32 *a;
{
	struct gblock *b = g_block(a[0]);
	u8 *copy;
	u32 n, t;
	u16 h;

	if (!b || b->gb_discarded || ico_get(a[0]))
		return 0;
	/* the DIB is read from a copy: the block becomes the icon */
	n = b->gb_size;
	copy = (u8 *)malloc(n);
	memcpy(copy, M + b->gb_base, n);
	t = g_alloc(GMEM_MOVEABLE, n, 0);
	if (!t) {
		free(copy);
		return 0;
	}
	memcpy(M + sel_base(t), copy, n);
	free(copy);
	h = fromdib_in(sel_base(t), n, 0, 0, 0, (u16)a[0]);
	g_free(t);
	return h;
}

/* DumpIcon(lpInfo, lpLen, lpXorBits, lpAndBits): where the parts of a locked icon are */
static u32
c_DumpIcon(a)
	u32 *a;
{
	u32 info = lin(FPSEL(a[0]), FPOFF(a[0])), p;
	int w, h, xb, ab, sx, sa;

	if (!info)
		return 0;
	w = GW(info + 4);
	h = GW(info + 6);
	xb = GW(info + 8);
	ab = ((w + 15) / 16) * 2;
	sx = xb * h;
	sa = ab * h;
	if ((p = lin(FPSEL(a[3]), FPOFF(a[3]))) != 0)
		PL(p, a[0] + 12);
	if ((p = lin(FPSEL(a[2]), FPOFF(a[2]))) != 0)
		PL(p, a[0] + 12 + sa);
	if ((p = lin(FPSEL(a[1]), FPOFF(a[1]))) != 0)
		PW(p, 12 + sa + sx);
	return FP(sx, sx);
}

/* CopyIcon(hInst, hIcon) */
static u32
c_CopyIcon(a)
	u32 *a;
{
	struct ico *i = ico_get(a[1]), *n;

	if (!i)
		return 0;
	n = (struct ico *)calloc(1, sizeof *n);
	*n = *i;
	n->hinst = 0;
	n->id = 0;
	n->pix = (u8 *)malloc(i->w * i->h);
	n->mask = (u8 *)malloc(i->w * i->h);
	memcpy(n->pix, i->pix, i->w * i->h);
	memcpy(n->mask, i->mask, i->w * i->h);
	memset((char *)&n->c, 0, sizeof n->c);
	if (n->cursor)
		mkdev(n);
	return newico(n);
}

struct impl cu_impl[] = {
	{ "USER", "GetIconID", c_GetIconID },
	{ "USER", "LoadIconHandler", c_LoadIconHandler },
	{ "USER", "DumpIcon", c_DumpIcon },
	{ "USER", "CopyIcon", c_CopyIcon },
	{ "USER", "CopyCursor", c_CopyIcon },
	{ 0, 0, 0 }
};

/* a block freed (GlobalFree): an icon taken from it goes */
void
ico_forget(h)
	u32 h;
{
	int ix = SELIX(h & 0xffff), k;
	struct ico *i;

	if (!bysel || ix <= 0 || ix >= LDTSIZE || !(i = bysel[ix]))
		return;
	bysel[ix] = 0;
	for (k = 1; k < NICO; k++)
		if (icos[k] == i)
			icos[k] = 0;
	free(i->pix);
	free(i->mask);
	if (i->c.and) {
		free(i->c.and);
		free(i->c.xor);
	}
	free((char *)i);
}
