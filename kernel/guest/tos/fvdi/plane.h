/*
 * plane.h -- drawing into separate bitplanes: pixel bit p lives in
 * plane p, at base + p * pstep.  Each plane is a packed 1-bit bitmap
 * of stride bytes a row, so every operation is the 1-bit one, once a
 * plane, with that plane's bit of fg and bg.  Pixel values, modes and
 * logic operations as in drawops.h; coordinates already clipped.
 */
#ifndef _PLANE_H
#define _PLANE_H

struct plbm {
	unsigned char *base;
	long stride, pstep, planes;
};

/* what the pointer covers: 3 bytes a row a plane */
struct plsave {
	unsigned char *at;
	long rows, bytes;
	unsigned char b[8 * 16 * 3];
};

void pl_fill(struct plbm *b, long x, long y, long w, long h,
    unsigned short const *pat, long fg, long bg, long mode);
void pl_expand(struct plbm *b, long x, long y, long w, long h,
    void const *src, long sstride, long sx, long fg, long bg, long mode, long flags);
void pl_blit(struct plbm *s, long sx, long sy, struct plbm *d,
    long x, long y, long w, long h, long op);
void pl_line(struct plbm *b, long x1, long y1, long x2, long y2,
    long style, long fg, long bg, long mode);
long pl_get(struct plbm *b, long x, long y);
void pl_put(struct plbm *b, long x, long y, long v);
/*
 * The pointer's visible w x h part at x, y: md holds a mask and a data
 * word a row from its first visible row, left pixel in bit 15 after a
 * shift by sh (what is cut off on the left).
 */
void pl_mshow(struct plbm *b, struct plsave *s, long x, long y, long w, long h,
    long sh, unsigned short const *md, long fg, long bg);
void pl_mhide(struct plbm *b, struct plsave *s);

#endif
