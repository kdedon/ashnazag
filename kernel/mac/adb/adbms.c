/*
 * adbms.c -- ADB mouse, talk register 0: byte 0 bit 7 = button up,
 * bits 6-0 = Y motion; byte 1 bits 6-0 = X motion; both 7-bit signed.
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

static int
sext7(v)
int v;
{
	v &= 0x7F;
	return (v & 0x40) ? v - 0x80 : v;
}

void
adbms_input(addr, d, n)
int addr;
unsigned char *d;
int n;
{
	int b, dx, dy, ch = 0;

	if (n < 2)
		return;
	b = (d[0] & 0x80) ? 0 : 1;
	dy = sext7(d[0]);
	dx = sext7(d[1]);
	if (b != adb_mouse_button) {
		adb_mouse_button = b;
		ch |= 1;
	}
	if (dx || dy) {
		adb_mouse_x += dx;
		adb_mouse_y += dy;
		ch |= 2;
	}
	if (ch && adb_mousefn)
		(*adb_mousefn)(0, MOUSE_CHANGE, (short)((d[0] << 8) | d[1]), ch);
}
