/*
 * textw.c -- GDI's text widths against the glyphs it draws, on the
 * in-memory screen: for each font, GetTextExtent is the sum of
 * GetCharWidth, each character's ink stays within its width, and a
 * string draws as its characters do one after another at those widths.
 *
 *   textw [Windows SYSTEM directory]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "scr.h"

struct x86 *cpu;
u32 pmvec[256];

static u16 hdc, mem;
static int bg, ink, fails;
static u8 *save;

static u32
call(name, a)
	char *name;
	u32 *a;
{
	struct impl *im;

	for (im = g_impl; im->im_name; im++)
		if (strcmp(im->im_name, name) == 0)
			return im->im_fn(a);
	fprintf(stderr, "textw: no %s\n", name);
	exit(2);
}

/* s drawn at (x, 8), the screen cleared first unless keep */
static void
draw(s, n, x, keep)
	char *s;
	int n, x, keep;
{
	u32 a[5];

	if (!keep)
		memset(screen.pix, bg, screen.rowb * screen.h);
	memcpy(gptr(FP(mem, 0)), s, n);
	a[0] = hdc, a[1] = x, a[2] = 8, a[3] = FP(mem, 0), a[4] = n;
	call("TextOut", a);
}

/* the leftmost and rightmost columns of ink, -1 if none */
static void
span(l, r)
	int *l, *r;
{
	int x, y;

	*l = *r = -1;
	for (y = 0; y < 64; y++)
		for (x = 0; x < screen.w; x++)
			if (screen.pix[y * screen.rowb + x] == ink) {
				if (*l < 0 || x < *l)
					*l = x;
				if (x > *r)
					*r = x;
			}
}

static void
font(face, height, weight)
	char *face;
	int height, weight;
{
	static char text[] = "This will end your Windows session.";
	u32 a[14];
	u16 hf;
	int c, l, r, cw[256], sum, ext, x, bad = 0;

	memset(a, 0, sizeof a);
	strcpy(gptr(FP(mem, 512)), face);
	a[0] = height, a[4] = weight, a[13] = FP(mem, 512);
	hf = call("CreateFont", a);
	a[0] = hdc, a[1] = hf;
	call("SelectObject", a);
	a[0] = hdc, a[1] = 0x20, a[2] = 0xff, a[3] = FP(mem, 1024);
	call("GetCharWidth", a);
	for (c = 0x20; c <= 0xff; c++)
		cw[c] = (short)GW(lin(mem, 1024 + 2 * (c - 0x20)));
	/* each character's ink within its width */
	for (c = 0x21; c < 0x7f; c++) {
		char ch = c;

		draw(&ch, 1, 100, 0);
		span(&l, &r);
		if (l >= 0 && (l < 100 || r >= 100 + cw[c])) {
			if (!bad++)
				printf("[FAIL] textw %s %d %d: '%c' ink %d-%d, width %d\n", face, height, weight,
				    c, l - 100, r - 100, cw[c]);
		}
	}
	/* the string's extent, and the string as its characters */
	memcpy(gptr(FP(mem, 0)), text, sizeof text - 1);
	a[0] = hdc, a[1] = FP(mem, 0), a[2] = sizeof text - 1;
	ext = (u16)call("GetTextExtent", a);
	for (sum = 0, c = 0; text[c]; c++)
		sum += cw[(u8)text[c]];
	if (ext != sum && !bad++)
		printf("[FAIL] textw %s %d %d: extent %d, widths %d\n", face, height, weight, ext, sum);
	draw(text, sizeof text - 1, 8, 0);
	memcpy(save, screen.pix, screen.rowb * 64);
	memset(screen.pix, bg, screen.rowb * screen.h);
	for (x = 8, c = 0; text[c]; x += cw[(u8)text[c++]])
		draw(text + c, 1, x, 1);
	if (memcmp(save, screen.pix, screen.rowb * 64) != 0 && !bad++)
		printf("[FAIL] textw %s %d %d: the string drawn apart from its characters\n", face, height, weight);
	fails += bad != 0;
	if (!bad)
		printf("[ok] textw %s %d %d: extent %d\n", face, height, weight, ext);
	a[0] = hf;
	call("DeleteObject", a);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	u32 a[4];

	mem_init((u32)8 << 20);
	scr_open(640, 480, 0);
	w16_dpi = 96;
	gdi_init();
	if (argc > 1)
		font_init(argv[1]);
	memset(a, 0, sizeof a);
	hdc = call("CreateDC", a);
	a[0] = hdc, a[1] = 1;		/* TRANSPARENT */
	call("SetBkMode", a);
	mem = g_alloc(GMEM_FIXED | GMEM_ZEROINIT, (u32)4096, 0);
	bg = pal_index(RGB(255, 255, 255));
	ink = pal_index(RGB(0, 0, 0));
	save = (u8 *)malloc(screen.rowb * 64);
	font("System", 0, 400);
	font("System", 0, 700);
	font("MS Sans Serif", -11, 400);
	font("MS Sans Serif", -11, 700);
	font("MS Serif", -13, 700);
	font("Courier", -13, 700);
	return fails != 0;
}
