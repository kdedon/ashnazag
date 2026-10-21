/*
 * The frame buffer driver for the host screen, 8 bits a pixel: the
 * kernel's driver table points at these.  The pointer is 16x16; the event
 * globals hold its images and masks a byte a pixel, premultiplied, and the
 * pixels it covers.
 *
 *	+0x48	image[4][256]
 *	+0x448	mask[4][256]
 *	+0x848	save[256]
 */
#include "hfswire.h"

#define F ((struct hf_fb *)HF_FB)
#define W(p, o) (*(short *)((char *)(p) + (o)))
#define L(p, o) (*(unsigned long *)((char *)(p) + (o)))

long
fb_probe()
{
	return 1;
}

long
fb_nop()
{
	return 0;
}

void
fb_info(ci)
	unsigned long *ci;
{
	int i;

	for (i = 0; i < 32; i++)
		ci[i] = F->info[i];
}

/* the window server draws from the address this returns */
long
fb_map()
{
	long a = (*F->map)();

	return a ? a + (F->info[16] & 0x1fff) : 0;
}

/* the screen byte at x, y, the bounds' top left being 0, 0 */
static unsigned char *
at(g, x, y)
	char *g;
	int x, y;
{
	return (unsigned char *)F->info[16] + (y - W(g, 52)) * F->info[3] + x - W(g, 48);
}

/* the pointer's rectangle, clipped to the screen: left, right, top, bottom */
void
fb_cursor()
{
	char *g = (char *)*F->evp;
	int x0 = W(g, 32), x1 = W(g, 34), y0 = W(g, 36), y1 = W(g, 38), x, y, m;
	unsigned char *p, *s = (unsigned char *)g + 0x848, *im, *mk;
	unsigned long o;

	x0 = x0 < W(g, 48) ? W(g, 48) : x0;
	x1 = x1 > W(g, 50) ? W(g, 50) : x1;
	y0 = y0 < W(g, 52) ? W(g, 52) : y0;
	y1 = y1 > W(g, 54) ? W(g, 54) : y1;
	x1 = x1 > W(g, 48) + (int)F->info[2] ? W(g, 48) + (int)F->info[2] : x1;
	y1 = y1 > W(g, 52) + (int)F->info[4] ? W(g, 52) + (int)F->info[4] : y1;
	L(g, 12) = x0 << 16 | (x1 & 0xffff);
	L(g, 16) = y0 << 16 | (y1 & 0xffff);
	o = L(g, 0) * 256 + (y0 - W(g, 36)) * 16 + x0 - W(g, 32);
	*F->drawn[0] = (unsigned long)at(g, x0, y0);
	for (y = y0; y < y1; y++, o += 16) {
		p = at(g, x0, y);
		im = (unsigned char *)g + 0x48 + o;
		mk = im + 0x400;
		for (x = x0; x < x1; x++, p++) {
			*s++ = *p;
			if ((m = *mk++) != 0)
				*p = m == 255 ? *im : *im + *p * (255 - m) / 255;
			im++;
		}
	}
	*F->drawn[1] = (unsigned long)at(g, x1, y1 - 1);
	*F->flags |= 1;
}

void
fb_uncursor()
{
	char *g = (char *)*F->evp;
	int x0 = W(g, 12), x1 = W(g, 14), y0 = W(g, 16), y1 = W(g, 18), x, y;
	unsigned char *p, *s = (unsigned char *)g + 0x848;

	*F->restored[0] = (unsigned long)at(g, x0, y0);
	for (y = y0; y < y1; y++)
		for (p = at(g, x0, y), x = x0; x < x1; x++)
			*p++ = *s++;
	*F->restored[1] = (unsigned long)at(g, x1, y1 - 1);
	*F->flags |= 2;
}
