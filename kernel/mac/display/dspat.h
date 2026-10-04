/*
 * dspat.h -- the test pattern and palette dstest draws, so that a check
 * can predict every pixel of a screen dump.
 *
 * Pixel (x, y) is colour index ((x >> 4) + 3 * (y >> 4) + seed) mod n,
 * n = 2^depth (256 for 16 and 32 bpp).  Palette entry i, 8-bit: red
 * (37 i + seed) & 255, green (91 i + 40) & 255, blue (255 - 7 i) & 255.
 * With the colour table rotated by k, hardware entry i holds palette
 * entry (i + k) mod n.  16 bpp is x-5-5-5, 32 bpp x-8-8-8 of the same.
 */
#ifndef DSPAT_H
#define DSPAT_H

#include "dsio.h"

static unsigned long
dspat_n(depth)
unsigned long depth;
{
	return depth <= 8 ? 1UL << depth : 256;
}

static unsigned long
dspat_index(x, y, seed, depth)
unsigned long x, y, seed, depth;
{
	return ((x >> 4) + 3 * (y >> 4) + seed) % dspat_n(depth);
}

static void
dspat_rgb(i, seed, rgb)
unsigned long i, seed;
unsigned char *rgb;
{
	rgb[0] = (37 * i + seed) & 255;
	rgb[1] = (91 * i + 40) & 255;
	rgb[2] = (255 - 7 * i) & 255;
}

/* the table rotated by k, as 16-bit components */
static void
dspat_cmap(seed, k, n, r, g, b)
unsigned long seed, k, n;
unsigned short *r, *g, *b;
{
	unsigned char c[3];
	unsigned long i;

	for (i = 0; i < n; i++) {
		dspat_rgb((i + k) % n, seed, c);
		r[i] = c[0] * 0x101;
		g[i] = c[1] * 0x101;
		b[i] = c[2] * 0x101;
	}
}

/* draw the pattern into a mapping of the mode in fi */
static void
dspat_draw(fb, fi, seed)
unsigned char *fb;
struct fbinfo *fi;
unsigned long seed;
{
	unsigned long x, y, d = fi->fi_depth, v, i;
	unsigned char *row, c[3];

	for (y = 0; y < fi->fi_height; y++) {
		row = fb + fi->fi_offset + y * fi->fi_rowbytes;
		for (x = 0; x < fi->fi_width; x++) {
			i = dspat_index(x, y, seed, d);
			if (d == 16 || d == 32) {
				dspat_rgb(i, seed, c);
				if (d == 16) {
					v = (c[0] >> 3) << 10 | (c[1] >> 3) << 5 | c[2] >> 3;
					row[2 * x] = v >> 8;
					row[2 * x + 1] = v;
				} else {
					row[4 * x] = 0;
					row[4 * x + 1] = c[0];
					row[4 * x + 2] = c[1];
					row[4 * x + 3] = c[2];
				}
			} else if (d == 8)
				row[x] = i;
			else {
				v = 8 - d - (x * d & 7);
				row[x * d >> 3] = (row[x * d >> 3] & ~(((1 << d) - 1) << v)) | i << v;
			}
		}
	}
}

/* pixels of the mapping that differ from the pattern */
static unsigned long
dspat_check(fb, fi, seed)
unsigned char *fb;
struct fbinfo *fi;
unsigned long seed;
{
	unsigned long x, y, d = fi->fi_depth, i, v, bad = 0;
	unsigned char *row, c[3];

	for (y = 0; y < fi->fi_height; y++) {
		row = fb + fi->fi_offset + y * fi->fi_rowbytes;
		for (x = 0; x < fi->fi_width; x++) {
			i = dspat_index(x, y, seed, d);
			if (d == 16) {
				dspat_rgb(i, seed, c);
				v = (c[0] >> 3) << 10 | (c[1] >> 3) << 5 | c[2] >> 3;
				bad += (((unsigned long)row[2 * x] << 8 | row[2 * x + 1]) & 0x7FFF) != v;
			} else if (d == 32) {
				dspat_rgb(i, seed, c);
				bad += row[4 * x + 1] != c[0] || row[4 * x + 2] != c[1] ||
				    row[4 * x + 3] != c[2];
			} else if (d == 8)
				bad += row[x] != i;
			else
				bad += (row[x * d >> 3] >> (8 - d - (x * d & 7)) & ((1 << d) - 1)) != i;
		}
	}
	return bad;
}

#endif
