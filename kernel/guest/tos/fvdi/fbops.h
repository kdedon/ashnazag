/*
 * fbops.h -- the frame buffer driver's drawing backend.  Pixel values,
 * not VDI colours; modes 1 replace, 2 transparent, 3 xor, 4 reverse
 * transparent; ops 0-15 the VDI logic operations.  Coordinates are
 * already clipped.  Each host may supply its own (a ROM or card
 * driver's blits and fills); the software one draws with the CPU.
 */
#ifndef _FBOPS_H
#define _FBOPS_H

typedef unsigned char Pix;

struct fbops {
	void (*fill)(Workstation *wk, long x, long y, long w, long h,
	    unsigned short const *pat, long fg, long bg, long mode);
	/* 1-bit source, bit sx of the row at src, onto 8-bit pixels at d */
	void (*expand)(unsigned short const *src, long swrap, long sx,
	    Pix *d, long dwrap, long w, long h, long fg, long bg, long mode);
	void (*blit)(Pix *s, long swrap, Pix *d, long dwrap, long w, long h, long op);
	void (*line)(Workstation *wk, long x1, long y1, long x2, long y2,
	    long pattern, long fg, long bg, long mode);
};

extern struct fbops const *fb;
extern Workstation *fb_wk;
long fb_pen(long colour);
Pix fb_op(long o, Pix s, Pix d);
long CDECL c_mouse_draw(Workstation *wk, long x, long y, Mouse *mouse);
void linea_init(Workstation *wk);
long la_alias(void *a);

#endif
