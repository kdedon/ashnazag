/*
 * fbtest.c -- host test of fbcons.c/fbprobe.c: renders a test page into
 * memory at every depth, decodes the pixels back to black/white, and
 * compares every cell with the font bitmap a plain text-grid model
 * predicts.  Also checks guard bytes (row padding, before and after
 * the buffer), the DSR answer, and the mode probe on fake A/UX low
 * memory.  Writes one PGM per mode into the output directory.
 *
 *   cc -DFB_HOST -I.. -o fbtest fbtest.c ../fbcons.c ../fbprobe.c ../fbfont.c
 *   ./fbtest outdir
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fbcons.h"

#define GUARD	0xA5
#define MAXR	64
#define MAXC	160

static int bad;
static char answer[64];
static int nanswer;

/* ---- stubs for the kernel side */

void
fbcons_input(c)
int c;
{
	if (nanswer < (int)sizeof answer - 1)
		answer[nanswer++] = c;
}

void mac_puts(s) char *s; { fputs(s, stdout); }
void mac_puthex(v) unsigned long v; { printf("0x%08lx", v); }

/* fake physical memory for the low-memory probe: 1 MB at address 0 */
static unsigned char ram[0x100000];
static unsigned long sense_reg = ~6UL;		/* sense code 6: 640x480 */
static unsigned char *vram;			/* video memory at 0xF9000000 */
#define VRAMSZ	0x100000
static unsigned char *rom;			/* ROM image at 0x40800000 */
static unsigned long romsz;
#define ROMBASE	0x40800000UL

static unsigned char *
hostaddr(a)
unsigned long a;
{
	if (a < sizeof ram)
		return ram + a;
	if (a >= 0xF9000000 && a < 0xF9000000 + VRAMSZ)
		return vram + (a - 0xF9000000);
	if (rom && a >= ROMBASE && a < ROMBASE + romsz)
		return rom + (a - ROMBASE);
	if (a >= 0x40000000 && a < 0x50000000) {	/* no ROM image: a bus error */
		printf("FAIL probe read 0x%08lx outside the ROM image\n", a);
		bad = 1;
		return ram;
	}
	return (unsigned char *)a;
}

unsigned long
fbh_peekl(a)
unsigned long a;
{
	unsigned char *p;

	if (a == 0xF980001C)
		return sense_reg;
	p = hostaddr(a);
	return (unsigned long)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

unsigned long
fbh_map(a)
unsigned long a;
{
	return (unsigned long)hostaddr(a);
}

unsigned long
fbh_peekw(a)
unsigned long a;
{
	unsigned char *p = hostaddr(a);

	return p[0] << 8 | p[1];
}

static void
pokel(a, v)
unsigned long a, v;
{
	ram[a] = v >> 24; ram[a + 1] = v >> 16; ram[a + 2] = v >> 8; ram[a + 3] = v;
}

static void
pokew(a, v)
unsigned long a, v;
{
	ram[a] = v >> 8; ram[a + 1] = v;
}

/* ---- the text-grid model */

static unsigned short grid[MAXR][MAXC];	/* char | 0x100 inverse | 0x200 bold */
static int R, C;

static void
g_put(r, c, s, attr)
int r, c, attr;
char *s;
{
	while (*s && c < C)
		grid[r][c++] = (*s++ & 0xFF) | attr;
}

static void
g_clear(r)
int r;
{
	int c;

	for (c = 0; c < C; c++)
		grid[r][c] = ' ';
}

/* rows top..bot up by n (down if n < 0) */
static void
g_scroll(top, bot, n)
int top, bot, n;
{
	int r;

	if (n > 0)
		for (r = top; r <= bot; r++)
			if (r + n <= bot)
				memcpy(grid[r], grid[r + n], sizeof grid[r]);
			else
				g_clear(r);
	else
		for (r = bot; r >= top; r--)
			if (r + n >= top)
				memcpy(grid[r], grid[r + n], sizeof grid[r]);
			else
				g_clear(r);
}

static void
out(s)
char *s;
{
	fbcons_write((unsigned char *)s, (int)strlen(s));
}

/* the page, sent to the renderer and applied to the model */
static void
page()
{
	char b[512];
	int i, r, c;

	for (r = 0; r < R; r++)
		g_clear(r);
	out("\033[2J\033[H");
	for (i = 0; i < R + 2; i++) {
		sprintf(b, "L%02d", i);
		out(b);
		if (i < R + 1)
			out("\r\n");
	}
	for (r = 0; r < R; r++) {
		sprintf(b, "L%02d", r + 2);
		g_put(r, 0, b, 0);
	}

	out("\033[H\033[2KTitle");
	g_clear(0); g_put(0, 0, "Title", 0);

	out("\033[2;1H\033[K");
	for (i = 0; i < C && 0x20 + i < 0x7F; i++)
		b[i] = 0x20 + i;
	b[i] = 0;
	out(b);
	g_clear(1); g_put(1, 0, b, 0);
	c = i;
	out("\033[3;1H\033[K");
	for (i = 0; 0x20 + c + i < 0x7F; i++)
		b[i] = 0x20 + c + i;
	for (r = 0xC0; r < 0xD0; r++)
		b[i++] = r;
	b[i] = 0;
	out(b);
	g_clear(2); g_put(2, 0, b, 0);

	out("\033[4;1H\033[2K\033[7mINV\033[m \033[1mB\033[0m.");
	g_clear(3); g_put(3, 0, "INV", 0x100); g_put(3, 3, " ", 0);
	g_put(3, 4, "B", 0x200); g_put(3, 5, ".", 0);

	out("\033[5;1H\033[2Kabc\bX\tT");
	g_clear(4); g_put(4, 0, "abX", 0); g_put(4, 8, "T", 0);

	out("\033[6;1H\033[2KXXXXXXXX\033[4D\033[K");
	g_clear(5); g_put(5, 0, "XXXX", 0);

	out("\033[7;1H\033[2K\033(0lqk\033(Bx");
	g_clear(6); g_put(6, 0, "\015\022\014x", 0);

	out("\033[8;1H\033[2K0123456789\033[8;3H\033[2P\033[8;2H\033[1@");
	g_clear(7); g_put(7, 0, "0 1456789", 0);

	out("\033[12;1H\033[2M");
	g_scroll(11, R - 1, 2);
	out("\033[10;1H\033[1L");
	g_scroll(9, R - 1, -1);

	out("\033[20;22r\033[22;1H\n\033[r");
	g_scroll(19, 21, 1);

	out("\033[1;1H\033M");
	g_scroll(0, R - 1, -1);
	out("RI");
	g_put(0, 0, "RI", 0);

	sprintf(b, "\033[%d;%dHABC", R, C - 1);
	out(b);
	g_put(R - 1, C - 2, "AB", 0);
	g_scroll(0, R - 1, 1);
	g_put(R - 1, 0, "C", 0);

	nanswer = 0;
	out("\033[6n");
	answer[nanswer] = 0;
	sprintf(b, "\033[%d;2R", R);
	if (strcmp(answer, b) != 0) {
		printf("FAIL DSR answer \"\\033%s\", want \"\\033%s\"\n", answer + 1, b + 1);
		bad = 1;
	}
}

/* ---- decode and compare */

static unsigned long
pixel(m, x, y)
struct fbmode *m;
int x, y;
{
	unsigned char *p = (unsigned char *)m->fm_base + (unsigned long)y * m->fm_row;
	unsigned long d = m->fm_depth, bit = (unsigned long)x * d;

	if (d < 8)
		return (p[bit / 8] >> (8 - d - bit % 8)) & ((1 << d) - 1);
	p += bit / 8;
	if (d == 8)
		return p[0];
	if (d == 16)
		return p[0] << 8 | p[1];
	return (unsigned long)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static int
check(name, m, outdir)
char *name, *outdir;
struct fbmode *m;
{
	unsigned long black, white, v;
	int x, y, want, got, errs = 0, cx, cy, e, gb;
	unsigned char *img, *base = (unsigned char *)m->fm_base;
	char path[512];
	FILE *f;

	if (m->fm_depth <= 8) {
		black = (1UL << m->fm_depth) - 1;
		white = 0;
	} else {
		black = 0;
		white = m->fm_depth == 16 ? 0x7FFF : 0xFFFFFF;
	}
	img = malloc(m->fm_width * m->fm_height);
	for (y = 0; y < (int)m->fm_height; y++)
		for (x = 0; x < (int)m->fm_width; x++) {
			v = pixel(m, x, y);
			got = v == black ? 1 : v == white ? 0 : -1;
			img[y * m->fm_width + x] = got == 1 ? 0 : got == 0 ? 255 : 128;
			cx = x / FB_CW; cy = y / FB_CH;
			if (cx >= C || cy >= R)
				want = 0;			/* margins */
			else {
				e = grid[cy][cx];
				gb = fb_font[e & 0xFF][y % FB_CH];
				if (e & 0x200)
					gb |= gb >> 1;
				want = (gb >> (7 - x % FB_CW)) & 1;
				if (e & 0x100)
					want ^= 1;
				if (cy == R - 1 && cx == 1)	/* cursor */
					want ^= 1;
			}
			if (got != want) {
				if (errs < 5)
					printf("  %s: pixel (%d,%d) cell (%d,%d) got %d want %d (0x%lx)\n",
					    name, x, y, cx, cy, got, want, v);
				errs++;
			}
		}
	/* guard bytes: before the buffer and the padding of every line */
	for (x = 1; x <= 64; x++)
		if (base[-x] != GUARD)
			errs++, printf("  %s: byte before the buffer changed\n", name);
	for (y = 0; y < (int)m->fm_height; y++)
		for (x = (m->fm_width * m->fm_depth + 7) / 8; x < (int)m->fm_row; x++)
			if (base[y * m->fm_row + x] != GUARD) {
				if (errs < 5)
					printf("  %s: padding byte %d of line %d changed\n", name, x, y);
				errs++;
			}
	for (x = 0; x < 64; x++)
		if (base[m->fm_height * m->fm_row + x] != GUARD)
			errs++, printf("  %s: byte after the buffer changed\n", name);
	sprintf(path, "%s/fbtest-%s.pgm", outdir, name);
	if ((f = fopen(path, "wb")) != NULL) {
		fprintf(f, "P5\n%lu %lu\n255\n", m->fm_width, m->fm_height);
		fwrite(img, 1, m->fm_width * m->fm_height, f);
		fclose(f);
	}
	free(img);
	printf("%s %-26s %lux%lu %2lu bpp row %4lu  %dx%d cells  -> %s\n",
	    errs ? "FAIL" : "OK  ", name, m->fm_width, m->fm_height, m->fm_depth,
	    m->fm_row, C, R, path);
	return errs != 0;
}

static int
mode(name, w, h, d, row, outdir)
char *name, *outdir;
unsigned long w, h, d, row;
{
	struct fbmode m;
	unsigned char *buf;

	buf = malloc(row * h + 128);
	memset(buf, GUARD, row * h + 128);
	m.fm_base = (unsigned long)(buf + 64);
	m.fm_row = row;
	m.fm_depth = d;
	m.fm_width = w;
	m.fm_height = h;
	if (!fbcons_attach(&m)) {
		printf("FAIL %s: attach refused\n", name);
		return 1;
	}
	C = w / FB_CW;
	R = h / FB_CH;
	page();
	fbcons_kputc('\n');		/* the kernel parser: LF implies CR */
	fbcons_kputc('k');
	g_scroll(0, R - 1, 1);
	g_put(R - 1, 0, "k", 0);
	/* cursor now at (R-1, 1) again */
	bad |= check(name, &m, outdir);
	free(buf);
	return 0;
}

/* ---- probe on fake A/UX low memory */

extern struct fbprobe { int fp_how; } fbprobe;

#define P_ROM	6
#define P_ONE	7

static void
probe(name, want, depth, w, h, base, row, info)
char *name;
int want, info;
unsigned long depth, w, h, base, row;
{
	int ok;

	fbcons.fc_on = 0;
	fbprobe.fp_how = 0;
	ok = fbcons_auxinit((unsigned long)info);
	ok = ok && fbprobe.fp_how == want && fbcons.fc_m.fm_depth == depth &&
	    fbcons.fc_m.fm_width == w && fbcons.fc_m.fm_height == h &&
	    (base == 0 || fbcons.fc_m.fm_base == (unsigned long)(vram + (base - 0xF9000000))) &&
	    (row == 0 || fbcons.fc_m.fm_row == row);
	printf("%s probe: %-40s -> method %d, %lux%lu %lu bpp row %lu\n", ok ? "OK  " : "FAIL",
	    name, fbprobe.fp_how, fbcons.fc_m.fm_width, fbcons.fc_m.fm_height,
	    fbcons.fc_m.fm_depth, fbcons.fc_m.fm_row);
	fbcons_report();
	bad |= !ok;
}

static void
refused(name, info)
char *name;
int info;
{
	int on;

	fbcons.fc_on = 0;
	on = fbcons_auxinit((unsigned long)info);
	printf("%s probe: %-40s -> %s\n", on ? "FAIL" : "OK  ", name, "refused");
	fbcons_report();
	bad |= on;
}

/*
 * A/UX Startup leaves ScrnBase/ScreenRow of the Monitors depth and the
 * screen in the lowest mode.  With the Quadra 700/900 ROM (same DAFB
 * video; the Q800's own ROM is not in the tree) the declaration data
 * must give that mode.
 */
static void
probes(romfile)
char *romfile;
{
	unsigned long lm = 0, info = 0x3C00;
	unsigned long gd = 0x20000, gdh = 0x20100, pm = 0x20200, pmh = 0x20300;
	FILE *f;

	vram = malloc(VRAMSZ);
	memset(ram, 0, sizeof ram);
	pokel(info, 0x50696773);
	pokew(info + 0xB0, 33);
	pokel(lm + 0x108, 0x800000);			/* MemTop 8 MB */
	pokel(lm + 0x2AE, 0);				/* no ROM to read */

	/* no ROM: 1 bpp at ScrnBase, size from the sense code */
	pokel(lm + 0x824, 0xF9000000);
	pokew(lm + 0x106, 0x8000 | 1024);		/* flag bit as in a PixMap */
	probe("no ROM: 1 bpp, sense 6", P_ONE, 1UL, 640UL, 480UL,
	    0xF9000000UL, 1024UL, (int)info);
	sense_reg = ~7UL;				/* extended sense */
	pokel(lm + 0xC24, 1024 * 480);
	probe("no ROM: 1 bpp, no sense, ScreenBytes", P_ONE, 1UL, 1152UL, 480UL,
	    0xF9000000UL, 1024UL, (int)info);
	sense_reg = ~6UL;

	if (romfile == NULL || (f = fopen(romfile, "rb")) == NULL) {
		printf("SKIP probe: declaration ROM cases (no ROM image given)\n");
		goto refusals;
	}
	rom = malloc(0x400000);
	romsz = fread(rom, 1, 0x400000, f);
	fclose(f);
	pokel(lm + 0x2AE, ROMBASE);

	/* Monitors 8 bpp from the main PixMap, 640x480, row 1024 at every depth */
	pokel(lm + 0x8A4, gdh);
	pokel(gdh, gd);
	pokel(gd + 22, pmh);
	pokel(pmh, pm);
	pokel(pm, 0xF9000000);
	pokew(pm + 4, 0x8000 | 1024);
	pokel(pm + 6, 0);
	pokel(pm + 10, 480 << 16 | 640);
	pokew(pm + 32, 8);
	probe("ROM: PixMap 8 bpp 640x480 row 1024", P_ROM, 1UL, 640UL, 480UL,
	    0xF9000000UL, 1024UL, (int)info);

	/* base offset moves with the depth (row 832 modes) */
	pokel(pm, 0x12345678);				/* heap overwritten */
	pokel(lm + 0x824, 0xF9009C40);
	pokew(lm + 0x106, 832);
	pokel(lm + 0x898, 0xF9009C40);
	pokew(lm + 0x8AC, 832);
	pokew(lm + 0xD60, 8);
	probe("ROM: ChunkyDepth 8, row 832, base +9C40", P_ROM, 1UL, 640UL, 480UL,
	    0xF9009C08UL, 832UL, (int)info);

	/* row bytes change with the depth (640x870 portrait: 1024 at 8 bpp, 512 below) */
	pokel(lm + 0x824, 0xF9000000);
	pokew(lm + 0x106, 1024);
	pokel(lm + 0x898, 0xF9000000);
	pokew(lm + 0x8AC, 1024);
	sense_reg = ~1UL;
	probe("ROM: 8 bpp, sense 1 (640x870)", P_ROM, 1UL, 640UL, 870UL,
	    0xF9000000UL, 512UL, (int)info);

	/* Monitors already 1 bpp: the same mode */
	pokew(lm + 0xD60, 1);
	sense_reg = ~6UL;
	probe("ROM: 1 bpp already, sense 6", P_ROM, 1UL, 640UL, 480UL,
	    0xF9000000UL, 1024UL, (int)info);

	/* nothing in the ROM has these row bytes: the 1 bpp assumption */
	pokew(lm + 0x106, 100);
	pokew(lm + 0x8AC, 100);
	probe("ROM: no matching mode", P_ONE, 1UL, 640UL, 480UL,
	    0xF9000000UL, 100UL, (int)info);
	pokew(lm + 0x106, 1024);

	/* ROMBase not a ROM: not read past its header */
	pokel(lm + 0x2AE, 0x00123456);
	probe("ROMBase in RAM", P_ONE, 1UL, 640UL, 480UL, 0xF9000000UL, 1024UL, (int)info);
	pokel(lm + 0x2AE, ROMBASE);

refusals:
	pokel(lm + 0x824, 0x00123456);			/* not video memory */
	refused("ScrnBase in RAM", (int)info);
	pokel(lm + 0x824, 0xF9000000);
	pokel(info, 0);
	refused("no 'Pigs' block", (int)info);
}

/* ---- jump scroll: one block against the same bytes one at a time */

static void
jump(outdir)
char *outdir;
{
	static char txt[16384];
	struct fbmode m;
	unsigned char *a, *b;
	unsigned long sz;
	int i, n, diff;

	n = 0;
	n += sprintf(txt + n, "\033[2J\033[Hfirst\r\n");
	for (i = 0; i < 70; i++) {
		n += sprintf(txt + n, "line %d", i);
		if (i % 9 == 4)			/* a wrapping line */
			n += sprintf(txt + n, " %0120d", i);
		if (i % 13 == 7)
			n += sprintf(txt + n, "\v");
		n += sprintf(txt + n, "\r\n");
		if (i == 40)			/* scroll region, then more lines */
			n += sprintf(txt + n, "\033[5;12r\033[12;1H");
	}
	n += sprintf(txt + n, "\033[r\033[30;1Hend\r\n\n\n");
	m.fm_row = 1024; m.fm_depth = 1; m.fm_width = 640; m.fm_height = 480;
	sz = m.fm_row * m.fm_height;
	a = calloc(1, sz);
	b = calloc(1, sz);
	m.fm_base = (unsigned long)a;
	fbcons_attach(&m);
	fbcons_write((unsigned char *)txt, n);
	m.fm_base = (unsigned long)b;
	fbcons_attach(&m);
	for (i = 0; i < n; i++)
		fbcons_putc(txt[i] & 0xFF);
	diff = memcmp(a, b, sz) != 0;
	printf("%s jump scroll: %d-byte block = the same bytes one at a time\n",
	    diff ? "FAIL" : "OK  ", n);
	bad |= diff;
	(void)outdir;
	free(a);
	free(b);
}

int
main(argc, argv)
int argc;
char **argv;
{
	char *o = argc > 1 ? argv[1] : ".";

	mode("1bpp-640x480", 640UL, 480UL, 1UL, 80UL, o);
	mode("1bpp-512x384-row128", 512UL, 384UL, 1UL, 128UL, o);
	mode("2bpp-640x480", 640UL, 480UL, 2UL, 160UL, o);
	mode("4bpp-640x480", 640UL, 480UL, 4UL, 320UL, o);
	mode("8bpp-640x480-row832", 640UL, 480UL, 8UL, 832UL, o);
	mode("8bpp-1152x870", 1152UL, 870UL, 8UL, 1152UL, o);
	mode("16bpp-640x480", 640UL, 480UL, 16UL, 1280UL, o);
	mode("32bpp-640x480-row2600", 636UL, 480UL, 32UL, 2600UL, o);
	jump(o);
	probes(argc > 2 ? argv[2] : NULL);
	printf("%s\n", bad ? "FAIL" : "all OK");
	return bad;
}
