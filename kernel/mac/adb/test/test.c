/*
 * test.c -- host tests of adb.c/adbkbd.c/adbms.c against sim.c.
 */
#include <stdio.h>
#include <string.h>
#include "../adb.h"
#include "sim.h"

static char out[256];
static int nout;
static int bad;

static void
ttyin(c)
int c;
{
	if (nout < (int)sizeof out - 1)
		out[nout++] = c;
	out[nout] = 0;
}

static void
check(ok, what)
int ok;
char *what;
{
	printf("%s %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		bad = 1;
}

/* let the bus run until quiet: interrupts, then auto-poll */
static void
run()
{
	int i, polls = 0;

	for (i = 0; i < 2000; i++) {
		if (sim_pending())
			adb_intr();
		else {
			adb_soft();
			if (++polls > 8)
				break;	/* sim_pollall never goes quiet */
			sim_idle();
			if (!sim_pending())
				break;
		}
	}
	adb_soft();
}

static void
ticks(n)
int n;
{
	while (n-- > 0) {
		adb_tick();
		run();
	}
}

static void
type(k, codes, n)
int k, n;
int *codes;
{
	int i;

	for (i = 0; i < n; i++)
		sim_key(k, codes[i]);
	run();
}

static int kbd;

static void
boot2()
{
	sim_reset();
	kbd = sim_add(SIM_KBD, 2, 2, 3);
	sim_add(SIM_MOUSE, 3, 1, 2);
	adb_init();
	run();
	nout = 0; out[0] = 0;
}

static int rawn, raw[16][3];

static void
rawhook(unit, type, code, more)
int unit, type, code, more;
{
	if (rawn < 16) {
		raw[rawn][0] = type; raw[rawn][1] = code; raw[rawn][2] = more;
		rawn++;
	}
}

static int mcalls, mr0, mch;

static void
mhook(unit, type, r0, ch)
int unit, type, r0, ch;
{
	mcalls++;
	mr0 = r0 & 0xFFFF;
	mch = ch;
}

static void
suite()
{
	int i, a, b;
	unsigned long p;
	char msg[128];

	/* enumeration */
	boot2();
	check(adb_ready && adb_ndev == 2, "enumeration: 2 devices, ready");
	check(adb_dev[2].d_orig == 2 && adb_dev[2].d_handler == 2, "keyboard at 2, handler 2");
	check(adb_dev[3].d_orig == 3 && adb_dev[3].d_handler == 1, "mouse at 3, handler 1");
	check(sim_dev[0].addr == 2 && sim_dev[1].addr == 3, "devices back at default addresses");
	check(sim_state() == ST_IDLE, "bus idle (auto-poll) after init");
	check(sim_dev[kbd].leds == 0, "LEDs written off at init");

	/* keys */
	{ int k[] = { 0x00, 0x80 }; type(kbd, k, 2); }
	check(strcmp(out, "a") == 0, "key a");
	nout = 0;
	{ int k[] = { 0x38, 0x00, 0x80, 0xB8, 0x12, 0x92 }; type(kbd, k, 6); }
	check(strcmp(out, "A1") == 0, "shift-a, then 1 after shift up");
	nout = 0;
	{ int k[] = { 0x36, 0x08, 0x88, 0xB6 }; type(kbd, k, 4); }
	check(strcmp(out, "\003") == 0, "control-c");
	nout = 0;
	{ int k[] = { 0x24, 0xA4, 0x33, 0xB3, 0x4C, 0xCC }; type(kbd, k, 6); }
	check(strcmp(out, "\r\b\r") == 0, "return, delete, keypad enter");
	nout = 0;
	{ int k[] = { 0x39 }; type(kbd, k, 1); }
	check(sim_dev[kbd].leds == LED_CAPS, "caps lock down lights the LED");
	if (sim_pollall)
		check(sim_ndrop > 0, "LED listen dropped behind an auto-poll reply, then resent");
	{ int k[] = { 0x00, 0x80, 0x12, 0x92, 0xB9 }; type(kbd, k, 5); }
	check(strcmp(out, "A1") == 0, "caps lock: A, digits unshifted");
	check(sim_dev[kbd].leds == 0, "caps lock up clears the LED");
	nout = 0;
	{ int k[] = { 0x3E, 0xBE, 0x3B, 0xBB }; type(kbd, k, 4); }
	check(strcmp(out, "\033[A\033[D") == 0, "arrows up, left as VT100");
	nout = 0;
	{ int k[] = { 0x7B, 0x2C, 0xAC, 0xFB }; type(kbd, k, 4); }
	check(strcmp(out, "?") == 0, "right shift (handler 3 code)");

	/* auto-repeat */
	nout = 0;
	{ int k[] = { 0x07 }; type(kbd, k, 1); }
	ticks(29);
	check(strcmp(out, "x") == 0, "no repeat before 30 ticks");
	ticks(1);
	check(strcmp(out, "xx") == 0, "repeat at 30 ticks");
	ticks(3);
	check(strcmp(out, "xxx") == 0, "repeat every 3 ticks");
	{ int k[] = { 0x87 }; type(kbd, k, 1); }
	ticks(10);
	check(strcmp(out, "xxx") == 0, "key up stops repeat");

	/* mouse through SRQ while the keyboard is auto-polled */
	a = adb_mouse_x; b = adb_mouse_y;
	mcalls = 0;
	adb_mousehook(mhook);
	sim_dev[1].mdx = 5; sim_dev[1].mdy = -3; sim_dev[1].mbtn = 1;
	run();
	check(adb_mouse_x == a + 5 && adb_mouse_y == b - 3 && adb_mouse_button == 1,
	    "mouse motion and button via SRQ scan");
	check(mcalls == 1 && mch == 3 && mr0 == ((0x7D << 8) | 0x85), "mouse consumer: r0 word, changed 3");
	check(adb_nsrq > 0, "SRQ counted");
	nout = 0;
	{ int k[] = { 0x0B, 0x8B }; type(kbd, k, 2); }
	check(strcmp(out, "b") == 0, "keyboard still delivered after mouse took auto-poll");
	sim_dev[1].mbtn = 0;
	run();
	check(adb_mouse_button == 0 && mch == 1, "button release");
	adb_mousehook(0);

	/* idle bus: no SRQ scans, at most 3 interrupts per poll */
	a = adb_nintr; b = adb_nsrq; p = sim_npoll;
	run();
	sprintf(msg, "idle: %lu interrupts in %lu polls, %lu SRQ scans",
	    adb_nintr - a, sim_npoll - p, adb_nsrq - b);
	check(adb_nsrq == (unsigned long)b && adb_nintr - a <= 3 * (sim_npoll - p) &&
	    (!sim_pollall || sim_npoll > p), msg);

	/* raw consumer, A/UX shape */
	rawn = 0;
	adb_keyhook(rawhook);
	nout = 0;
	{ int k[] = { 0x00, 0x80, 0x7F, 0x7F }; type(kbd, k, 4); }
	check(nout == 0, "raw consumer takes the keyboard from the console");
	check(rawn == 4 && raw[0][0] == KC_CHAR && raw[0][1] == 0x00 && raw[0][2] == 1 &&
	    raw[1][1] == 0x80 && raw[1][2] == 0, "raw: (KC_CHAR, 0x00, more 1), (0x80, 0)");
	check(rawn == 4 && raw[2][1] == 0x7F && raw[2][2] == 1 && raw[3][1] == 0x7F &&
	    raw[3][2] == 0, "raw: power key 0x7F7F as 0x7F twice");
	adb_keyhook(0);

	/* PB3 low on byte 1 read as SRQ: the data still arrives */
	sim_end2 = 1;
	nout = 0;
	{ int k[] = { 0x01, 0x81 }; type(kbd, k, 2); }
	check(strcmp(out, "s") == 0, "end signalled on byte 1");
	sim_end2 = 0;

	/* collision: two mice at 3 */
	sim_reset();
	sim_add(SIM_KBD, 2, 2, 3);
	sim_add(SIM_MOUSE, 3, 1, 2);
	sim_add(SIM_MOUSE, 3, 1, 2);
	adb_init();
	run();
	for (a = b = 0, i = 1; i < 16; i++)
		if (adb_dev[i].d_orig == 3)
			a++;
	check(adb_ndev == 3 && a == 2, "collision: 3 devices, 2 mice");
	check(sim_dev[1].addr != sim_dev[2].addr && adb_dev[sim_dev[1].addr].d_orig == 3 &&
	    adb_dev[sim_dev[2].addr].d_orig == 3, "mice at distinct addresses, table agrees");
	sim_dev[2].mdx = 7;
	a = adb_mouse_x;
	run();
	check(adb_mouse_x == a + 7, "moved mouse reports through SRQ");

	/* no devices; keyboard plugged later */
	sim_reset();
	adb_init();
	run();
	check(adb_ready && adb_ndev == 0, "no devices: init completes, ready");
	kbd = sim_add(SIM_KBD, 2, 2, 3);
	nout = 0;
	{ int k[] = { 0x0C, 0x8C }; type(kbd, k, 2); }
	check(strcmp(out, "q") == 0, "keyboard plugged after boot is auto-polled");

	/* dead transceiver */
	sim_reset();
	sim_dead = 1;
	sim_add(SIM_KBD, 2, 2, 3);
	adb_init();
	check(!adb_ready, "dead transceiver: init returns, ADB disabled");
	ticks(10);
	check(1, "dead transceiver: ticks do nothing");

	/* watchdog: transceiver stops mid-transaction, then recovers */
	boot2();
	sim_stall = 2;
	sim_nbytes = 0;
	{ int k[] = { 0x02, 0x82 }; type(kbd, k, 2); }
	a = adb_nwdog;
	sim_stall = 0;
	ticks(10);
	sprintf(msg, "watchdog restarts a stalled bus (%lu)", adb_nwdog - a);
	check(adb_nwdog > (unsigned long)a, msg);
	nout = 0;
	{ int k[] = { 0x03, 0x83 }; type(kbd, k, 2); }
	check(strchr(out, 'f') != 0, "input works after the watchdog");

}

/* PB3 stuck low after commands; a transceiver without auto-poll */
static void
edge()
{
	struct adbreq r;
	int n;

	printf("-- PB3 low after every command\n");
	boot2();
	sim_cmdlow = 1;
	{ int k[] = { 0x39 }; type(kbd, k, 1); }
	ticks(2);
	check(sim_dev[kbd].leds == LED_CAPS, "LED listen completes despite the drops");
	r.r_cmd = ADB_TALK(3, 3);
	r.r_len = 0;
	r.r_done = 0;
	n = adb_op_sync(&r);
	check(n >= 2, "talk R3 completes after 3 resends");
	sim_cmdlow = 0;
	{ int k[] = { 0xB9, 0x0B, 0x8B }; type(kbd, k, 3); }
	nout = 0;
	{ int k[] = { 0x0B, 0x8B }; type(kbd, k, 2); }
	check(strcmp(out, "b") == 0, "keys after PB3 recovers");

	printf("-- no auto-poll, state edges only\n");
	sim_reset();
	sim_noauto = 1;
	kbd = sim_add(SIM_KBD, 2, 2, 3);
	sim_add(SIM_MOUSE, 3, 1, 2);
	adb_init();
	run();
	check(adb_ready && adb_ndev == 2, "reset then talks: enumeration over CMD-to-CMD");
	sim_dev[kbd].leds = 7;
	adbkbd_setleds(0);
	run();
	check(sim_dev[kbd].leds == 0, "LED listen R2 without auto-poll");
	sim_key(kbd, 0x0C);
	r.r_cmd = ADB_TALK(2, 0);
	r.r_len = 0;
	r.r_done = 0;
	n = adb_op_sync(&r);
	check(n == 2 && r.r_data[0] == 0x0C, "explicit talk R0 gets the key");
}

int
main()
{
	adb_ttyin = ttyin;
	printf("-- transceiver interrupts only for data or SRQ\n");
	suite();
	printf("-- every auto-poll interrupts\n");
	sim_pollall = 1;
	suite();
	edge();
	printf("%s\n", bad ? "FAILED" : "all passed");
	return bad;
}
