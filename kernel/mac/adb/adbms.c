/*
 * adbms.c -- ADB mouse, talk register 0: byte 0 bit 7 = button up,
 * bits 6-0 = Y motion; byte 1 bit 7 = second button up, bits 6-0 =
 * X motion; both 7-bit signed.  The extended handler (4) may add bytes,
 * each with 3 more high bits of Y (6-4) and of X (2-0).
 * The state is kept in adb_mouse_*; a consumer installed with
 * adb_mousehook() gets each change (A/UX mouse-layer shape).
 */
#ifdef ADB_HOST
#include <stdio.h>
#else
#include "sys/types.h"
#endif
#include "adb.h"

unsigned char adb_mouse_button = 0;
unsigned char adb_mouse_nbtn = 0;	/* from register 1 of an extended mouse */
short adb_mouse_x = 0, adb_mouse_y = 0;
static void (*adb_mousefn)() = 0;

void (*
adb_mousehook(fn))()
void (*fn)();
{
	void (*o)() = adb_mousefn;

	adb_mousefn = fn;
	return o;
}

void
adbms_input(addr, d, n)
int addr;
unsigned char *d;
int n;
{
	int b, dx, dy, sx, sy, r, i, w = 7, ch;

	if (n < 2)
		return;
	b = (d[0] & 0x80 ? 0 : 1) | (d[1] & 0x80 ? 0 : 2);
	dy = d[0] & 0x7F;
	dx = d[1] & 0x7F;
	for (i = 2; i < n && adb_dev[addr].d_handler == 4 && w < 28; i++, w += 3) {
		dy |= (d[i] >> 4 & 7) << w;
		dx |= (d[i] & 7) << w;
	}
	if (dy & 1 << (w - 1))
		dy -= 1 << w;
	if (dx & 1 << (w - 1))
		dx -= 1 << w;
	ch = ((b ^ adb_mouse_button) & 1) | ((b ^ adb_mouse_button) & 2 ? 4 : 0);
	adb_mouse_button = b;
	adb_mouse_x += dx;
	adb_mouse_y += dy;
	/* the consumer takes 7-bit deltas: a longer move goes in steps */
	r = (b & 1 ? 0 : 0x8000) | (b & 2 ? 0 : 0x80);
	do {
		sx = dx > 63 ? 63 : dx < -64 ? -64 : dx;
		sy = dy > 63 ? 63 : dy < -64 ? -64 : dy;
		dx -= sx;
		dy -= sy;
		if (sx || sy)
			ch |= 2;
		if (ch && adb_mousefn)
			(*adb_mousefn)(0, MOUSE_CHANGE, (short)(r | (sy & 0x7F) << 8 | (sx & 0x7F)), ch);
		ch = 0;
	} while (dx || dy);
}
