/*
 * fontfile.c -- Windows' own bitmap fonts: the .FON files in the user's
 * SYSTEM directory (NE files whose RT_FONT resources are .FNT fonts,
 * versions 2 and 3), made into our struct bfont beside the built-in
 * ones.  Of a face's sizes for several displays, those for 96 dpi (VGA)
 * are kept.  Vector fonts (Modern, Roman, Script) are left out.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "w16.h"
#include "font.h"

struct bfont *fontlist;		/* the loaded fonts, then the built-in ones; f_face 0 ends */
int nfontlist;

static struct bfont *loaded;
static int nloaded, maxloaded;

#define	W(p)	((p)[0] | (p)[1] << 8)
#define	L(p)	(W(p) | (u32)W((p) + 2) << 16)

/* one .FNT image of n bytes into a bfont; 0 done */
static int
fnt(p, n, f)
	u8 *p;
	u32 n;
	struct bfont *f;
{
	int ver, first, last, h, nc, i, c, w, bpr, row, col, hdr, esize;
	u32 off, total, face;
	u8 *bits;

	if (n < 118)
		return -1;
	ver = W(p);
	if ((ver != 0x200 && ver != 0x300) || (W(p + 66) & 1))	/* dfType bit 0: vector */
		return -1;
	h = W(p + 88);
	first = p[95];
	last = p[96];
	if (h <= 0 || h > 64 || last < first)
		return -1;
	nc = last - first + 1;
	hdr = ver == 0x200 ? 118 : 148;
	esize = ver == 0x200 ? 4 : 6;
	if (hdr + (u32)(nc + 1) * esize > n)
		return -1;
	memset((char *)f, 0, sizeof *f);
	face = L(p + 105);
	if (face == 0 || face >= n)
		return -1;
	{
		int len = strlen((char *)p + face) > 32 ? 32 : strlen((char *)p + face);

		f->f_face = (char *)malloc(len + 1);
		memcpy(f->f_face, (char *)p + face, len);
		f->f_face[len] = 0;
	}
	f->f_points = W(p + 68);
	f->f_res = W(p + 70);
	f->f_height = h;
	f->f_ascent = W(p + 74);
	f->f_descent = h - f->f_ascent;
	f->f_leading = W(p + 76);
	f->f_extlead = W(p + 78);
	f->f_italic = p[80];
	f->f_weight = W(p + 83);
	f->f_charset = p[85];
	f->f_pitch = !(p[90] & 1);	/* dfPitchAndFamily bit 0: variable */
	f->f_family = p[90] & 0xf0;
	f->f_avgw = W(p + 91);
	f->f_maxw = W(p + 93);
	f->f_first = first;
	f->f_last = last;
	f->f_default = first + p[97];
	f->f_break = first + p[98];
	f->f_w = (u8 *)malloc(nc);
	f->f_off = (unsigned short *)malloc(nc * sizeof(unsigned short));
	/* the glyphs: columns of 8 pixels, h bytes each, to rows of (w+7)/8 bytes */
	for (total = 0, i = 0; i < nc; i++) {
		w = W(p + hdr + i * esize);
		total += ((w + 7) / 8) * h;
	}
	if (total > 0xffff)
		return -1;
	bits = f->f_bits = (u8 *)calloc(1, total + 1);
	for (total = 0, i = 0; i < nc; i++) {
		w = W(p + hdr + i * esize);
		off = ver == 0x200 ? W(p + hdr + i * esize + 2) : L(p + hdr + i * esize + 2);
		bpr = (w + 7) / 8;
		if (w > 255 || off + (u32)bpr * h > n)
			w = 0, bpr = 0;
		f->f_w[i] = w;
		f->f_off[i] = total;
		for (col = 0; col < bpr; col++)
			for (row = 0; row < h; row++) {
				c = p[off + col * h + row];
				bits[total + row * bpr + col] = c;
			}
		total += bpr * h;
	}
	return 0;
}

static void
add(f)
	struct bfont *f;
{
	if (nloaded == maxloaded) {
		maxloaded = maxloaded ? 2 * maxloaded : 32;
		loaded = (struct bfont *)realloc(loaded, (maxloaded + 1) * sizeof *loaded);
	}
	loaded[nloaded++] = *f;
}

/* a .FON (or a bare .FNT) file's fonts; the number added */
int
font_loadfile(host)
	char *host;
{
	FILE *fp;
	u8 *d, *ne, *rt, *t;
	long n;
	int ne0, shift, type, count, i, added = 0;
	struct bfont f;

	if ((fp = fopen(host, "rb")) == 0)
		return 0;
	fseek(fp, 0L, 2);
	n = ftell(fp);
	fseek(fp, 0L, 0);
	if (n < 64 || n > 4L << 20 || (d = (u8 *)malloc(n)) == 0) {
		fclose(fp);
		return 0;
	}
	if (fread(d, 1, n, fp) != n) {
		fclose(fp);
		free(d);
		return 0;
	}
	fclose(fp);
	if (d[0] != 'M' || d[1] != 'Z') {
		if (fnt(d, (u32)n, &f) == 0)
			add(&f), added++;
		free(d);
		return added;
	}
	ne0 = W(d + 0x3c);
	if (ne0 + 64 > n || d[ne0] != 'N' || d[ne0 + 1] != 'E') {
		free(d);
		return 0;
	}
	ne = d + ne0;
	rt = ne + W(ne + 0x24);
	if (rt + 2 > d + n) {
		free(d);
		return 0;
	}
	shift = W(rt);
	for (t = rt + 2; t + 8 <= d + n && (type = W(t)) != 0; t += 8 + 12 * count) {
		count = W(t + 2);
		if (type != (0x8000 | 8))
			continue;
		for (i = 0; i < count; i++) {
			u8 *e = t + 8 + 12 * i;
			u32 off = (u32)W(e) << shift, len = (u32)W(e + 2) << shift;

			if (off + len > (u32)n)
				len = n - off;
			if (fnt(d + off, len, &f) == 0)
				add(&f), added++;
		}
	}
	free(d);
	return added;
}

/* does a face have a size for this resolution */
static int
hasres(face, res)
	char *face;
	int res;
{
	int i;

	for (i = 0; i < nloaded; i++)
		if (loaded[i].f_res == res && strcmp(loaded[i].f_face, face) == 0)
			return 1;
	return 0;
}

/* every .FON in the directory (a host path), then the list GDI chooses from */
void
font_init(sysdir)
	char *sysdir;
{
	DIR *dp;
	struct dirent *e;
	char p[1200];
	int i, j, n, nb;

	if (sysdir && (dp = opendir(sysdir)) != 0) {
		while ((e = readdir(dp)) != 0) {
			n = strlen(e->d_name);
			if (n < 5 || n > 200 || (strcmp(e->d_name + n - 4, ".fon") != 0 && strcmp(e->d_name + n - 4, ".FON") != 0))
				continue;
			sprintf(p, "%s/%s", sysdir, e->d_name);
			font_loadfile(p);
		}
		closedir(dp);
	}
	/* the sizes for other displays go where VGA's exist */
	for (i = j = 0; i < nloaded; i++)
		if (loaded[i].f_res == 96 || !hasres(loaded[i].f_face, 96))
			loaded[j++] = loaded[i];
	nloaded = j;
	for (nb = 0; bfonts[nb].f_face; nb++)
		;
	nfontlist = nloaded + nb;
	fontlist = (struct bfont *)calloc(nfontlist + 1, sizeof *fontlist);
	for (i = 0; i < nloaded; i++)
		fontlist[i] = loaded[i];
	for (i = 0; i < nb; i++)
		fontlist[nloaded + i] = bfonts[i];
	if (w16_debug)
		w16_log("startwin: %d fonts of Windows, %d built in\n", nloaded, nb);
}

/* any of Windows' own */
int
font_haveface(face)
	char *face;
{
	int i;

	for (i = 0; i < nloaded; i++)
		if (w16_stricmp(loaded[i].f_face, face) == 0)
			return 1;
	return 0;
}
