/*
 * adbkbd.c -- ADB keyboard: talk-R0 decoding, the raw key consumer
 * (A/UX key-layer shape), and the console path: US layout to ASCII with
 * auto-repeat, fed to adb_ttyin one byte at a time.
 *
 * Register 0: two key transitions, bit 7 = up, 0xFF = none; 0x7F7F is
 * the power key down, 0xFFFF the power key up.  Register 2 byte 1 bits
 * 0-2 are the LEDs, active low.  Caps Lock latches in the keyboard: it
 * reports down while locked and up when released.
 */
#ifdef ADB_HOST
#include <stdio.h>
#else
#include "sys/types.h"
#endif
#include "adb.h"

#ifdef ADB_FBCONS
extern void fbcons_input();
void (*adb_ttyin)() = fbcons_input;
#else
void (*adb_ttyin)() = 0;
#endif

#define RPT_WAIT	30	/* ticks before repeating (HZ/2) */
#define RPT_GAP		3	/* ticks between repeats (HZ/20) */

/* modifier key codes (left, then right-hand codes of handler 3) */
#define K_CTL		0x36
#define K_CMD		0x37
#define K_SHIFT		0x38
#define K_CAPS		0x39
#define K_OPT		0x3A
#define K_RSHIFT	0x7B
#define K_ROPT		0x7C
#define K_RCTL		0x7D
#define K_POWER		0x7F

#define M_SHIFT		0x01
#define M_CTL		0x02
#define M_OPT		0x04
#define M_CMD		0x08
#define M_CAPS		0x10

/* US layout by raw ADB key code: unshifted, shifted; 0 = no character */
static char kmap[128][2] = {
	{ 'a', 'A' }, { 's', 'S' }, { 'd', 'D' }, { 'f', 'F' },	/* 00 */
	{ 'h', 'H' }, { 'g', 'G' }, { 'z', 'Z' }, { 'x', 'X' },
	{ 'c', 'C' }, { 'v', 'V' }, { 0, 0 },     { 'b', 'B' },	/* 08 */
	{ 'q', 'Q' }, { 'w', 'W' }, { 'e', 'E' }, { 'r', 'R' },
	{ 'y', 'Y' }, { 't', 'T' }, { '1', '!' }, { '2', '@' },	/* 10 */
	{ '3', '#' }, { '4', '$' }, { '6', '^' }, { '5', '%' },
	{ '=', '+' }, { '9', '(' }, { '7', '&' }, { '-', '_' },	/* 18 */
	{ '8', '*' }, { '0', ')' }, { ']', '}' }, { 'o', 'O' },
	{ 'u', 'U' }, { '[', '{' }, { 'i', 'I' }, { 'p', 'P' },	/* 20 */
	{ '\r', '\r' }, { 'l', 'L' }, { 'j', 'J' }, { '\'', '"' },
	{ 'k', 'K' }, { ';', ':' }, { '\\', '|' }, { ',', '<' },	/* 28 */
	{ '/', '?' }, { 'n', 'N' }, { 'm', 'M' }, { '.', '>' },
	{ '\t', '\t' }, { ' ', ' ' }, { '`', '~' }, { '\b', '\b' },	/* 30 */
	{ '\r', '\r' }, { 033, 033 }, { 0, 0 }, { 0, 0 },
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },			/* 38 */
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
	{ 0, 0 }, { '.', '.' }, { 0, 0 }, { '*', '*' },		/* 40 */
	{ 0, 0 }, { '+', '+' }, { 0, 0 }, { 033, 033 },
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { '/', '/' },			/* 48 */
	{ '\r', '\r' }, { 0, 0 }, { '-', '-' }, { 0, 0 },
	{ 0, 0 }, { '=', '=' }, { '0', '0' }, { '1', '1' },		/* 50 */
	{ '2', '2' }, { '3', '3' }, { '4', '4' }, { '5', '5' },
	{ '6', '6' }, { '7', '7' }, { 0, 0 }, { '8', '8' },		/* 58 */
	{ '9', '9' }, { 0, 0 }, { 0, 0 }, { 0, 0 },
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },			/* 60 */
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },			/* 68 */
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },			/* 70 */
	{ 0, 0 }, { 0177, 0177 }, { 0, 0 }, { 0, 0 },
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },			/* 78 */
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
};

static void (*adb_keyfn)() = 0;
static int kb_mods = 0;
static int kb_rcode = -1;		/* repeating key, -1 none */
static int kb_rticks = 0;
static int kb_leds = 0, kb_ledwant = -1, kb_ledbusy = 0;
static struct adbreq kb_ledreq;
unsigned char adb_keydown[16] = { 0 };	/* key-down bitmap by raw code */
short adb_key_r0 = 0;

void (*
adb_keyhook(fn))()
void (*fn)();
{
	void (*o)() = adb_keyfn;

	adb_keyfn = fn;
	kb_rcode = -1;
	return o;
}

static void
emit(s)
register char *s;
{
	if (adb_ttyin == 0)
		return;
	while (*s)
		(*adb_ttyin)(*s++ & 0xFF);
}

/* the bytes one key produces with the current modifiers; 0 if none */
static int
keystr(code, buf)
int code;
char *buf;
{
	register int c;

	buf[0] = buf[1] = 0;
	switch (code) {
	case 0x3B: c = 'D'; goto arrow;
	case 0x3C: c = 'C'; goto arrow;
	case 0x3D: c = 'B'; goto arrow;
	case 0x3E: c = 'A';
	arrow:
		buf[0] = 033; buf[1] = '['; buf[2] = c; buf[3] = 0;
		return 1;
	}
	if (kb_mods & M_CMD)
		return 0;
	c = kmap[code][(kb_mods & M_SHIFT) ? 1 : 0];
	if (c == 0)
		return 0;
	if ((kb_mods & M_CAPS) && c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	if (kb_mods & M_CTL) {
		if (c >= '@' && c < 0177)
			c &= 037;
		else if (c == ' ' || c == '2')
			c = 0;
		else if (c == '6')
			c = 036;
		else if (c == '-')
			c = 037;
		else if (c == '/')
			c = 037;
	}
	buf[0] = c;
	if (c == 0) {		/* control-space: NUL can't go through a string */
		if (adb_ttyin)
			(*adb_ttyin)(0);
		return 0;
	}
	return 1;
}

static int
modbit(code)
int code;
{
	switch (code) {
	case K_SHIFT: case K_RSHIFT:	return M_SHIFT;
	case K_CTL: case K_RCTL:	return M_CTL;
	case K_OPT: case K_ROPT:	return M_OPT;
	case K_CMD:			return M_CMD;
	case K_CAPS:			return M_CAPS;
	}
	return 0;
}

/* one transition, console path */
static void
conskey(b)
int b;
{
	int code = b & 0x7F, up = b & 0x80, m;
	char buf[4];

	if ((m = modbit(code)) != 0) {
		if (up)
			kb_mods &= ~m;
		else
			kb_mods |= m;
		if (m == M_CAPS)
			adbkbd_setleds(up ? (kb_leds & ~LED_CAPS) : (kb_leds | LED_CAPS));
		return;
	}
	if (up) {
		if (code == kb_rcode)
			kb_rcode = -1;
		return;
	}
	kb_rcode = -1;
	if (keystr(code, buf)) {
		emit(buf);
		kb_rcode = code;
		kb_rticks = RPT_WAIT;
	}
}

/* the console path for one transition, for a consumer that routes keys */
void
adbkbd_cons(b)
int b;
{
	conskey(b);
}

static void
key(b, more)
int b, more;
{
	int code = b & 0x7F;

	if (b & 0x80)
		adb_keydown[code >> 3] &= ~(1 << (code & 7));
	else
		adb_keydown[code >> 3] |= 1 << (code & 7);
	if (adb_keyfn)
		(*adb_keyfn)(0, KC_CHAR, b, more);
	else if (code != K_POWER)
		conskey(b);
}

/* talk R0 reply from the keyboard at addr */
void
adbkbd_input(addr, d, n)
int addr;
unsigned char *d;
int n;
{
	if (n < 2)
		return;
	adb_key_r0 = (d[0] << 8) | d[1];
	if (d[0] == 0x7F && d[1] == 0x7F) {	/* power key: 0x7F twice */
		key(0x7F, 1);
		key(0x7F, 0);
		return;
	}
	if (d[0] != 0xFF)
		key(d[0], d[1] != 0xFF);
	if (d[1] != 0xFF)
		key(d[1], 0);
}

/* clock tick: auto-repeat on the console path */
void
adbkbd_tick()
{
	char buf[4];

	if (kb_rcode < 0 || --kb_rticks > 0)
		return;
	kb_rticks = RPT_GAP;
	if (keystr(kb_rcode, buf))
		emit(buf);
}

/* ---------------------------------------------------------- LEDs */

static void ledstep();

static void
leddone(r)
struct adbreq *r;
{
	if (ADB_ISTALK(r->r_cmd) && r->r_n >= 2) {
		/* have R2: write it back with the new LED bits */
		r->r_cmd = ADB_LISTEN(ADB_ADDR_KBD, 2);
		r->r_len = 2;
		r->r_data[1] = (r->r_data[1] & ~7) | (~kb_ledwant & 7);
		kb_leds = kb_ledwant;
		if (adb_queue(r) == 0)
			return;
	}
	kb_ledbusy = 0;
	if (kb_ledwant != kb_leds && ADB_ISLISTEN(r->r_cmd))
		ledstep();
}

static void
ledstep()
{
	kb_ledbusy = 1;
	kb_ledreq.r_cmd = ADB_TALK(ADB_ADDR_KBD, 2);
	kb_ledreq.r_len = 0;
	kb_ledreq.r_done = leddone;
	if (adb_queue(&kb_ledreq) < 0)
		kb_ledbusy = 0;
}

/* set the keyboard LEDs (LED_*); asynchronous, latest request wins */
void
adbkbd_setleds(mask)
int mask;
{
	kb_ledwant = mask & 7;
	if (!kb_ledbusy && adb_ready)
		ledstep();
}
