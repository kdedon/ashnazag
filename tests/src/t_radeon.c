/*
 * t_radeon -- the Radeon ROM init-table interpreter against a ROM image
 * built here and a model of the card's registers: header checks, then
 * the register writes, delays and polls each table produces, in order.
 */
#include <stdio.h>
#include <string.h>
#include "rdsinit.h"
#include "t.h"

#define	ROMLEN	0x10000

static unsigned char rom[ROMLEN];

/* the card */
static unsigned long reg[0x4000], pll[64], vram[256];
static int stuck, busy, dll;

/* what the interpreter did: W reg write, P PLL write, D delay, r/p poll */
struct op {
	int k, r;
	unsigned long v;
};
static struct op got[30000];
static int ngot, npoll;

static void
log_op(int k, int r, unsigned long v)
{
	if (k == 'r' || k == 'p')
		npoll++;
	if (ngot < sizeof got / sizeof got[0]) {
		got[ngot].k = k;
		got[ngot].r = r;
		got[ngot].v = v;
	}
	ngot++;
}

unsigned long
rdi_rl(int r)
{
	if (r == 0x0004)
		return vram[(reg[0] >> 20) & 0xff];
	if (r == 0x0150) {
		log_op('r', r, 0);
		return stuck ? 0 : 0x07;
	}
	return reg[r / 4];
}

void
rdi_wl(int r, unsigned long v)
{
	if (r == 0x0004) {
		vram[(reg[0] >> 20) & 0xff] = v;
		return;
	}
	if (r != 0)
		log_op('W', r, v);
	reg[r / 4] = v;
}

unsigned long
rdi_rpll(int r)
{
	unsigned long v = pll[r];

	if (r == 0x14) {
		log_op('p', r, 0);
		if (busy) {
			busy--;
			v |= 1L << 16;
		} else if (dll)
			dll--;
		else
			v |= 1L << 19;
	}
	return v;
}

void
rdi_wpll(int r, unsigned long v)
{
	log_op('P', r, v);
	pll[r] = v;
}

void
rdi_delay(long us)
{
	log_op('D', 0, (unsigned long)us);
}

static void
w16(int o, unsigned v)
{
	rom[o] = v;
	rom[o + 1] = v >> 8;
}

static void
w32(int o, unsigned long v)
{
	w16(o, (unsigned)(v & 0xffff));
	w16(o + 2, (unsigned)(v >> 16));
}

/* tables at fixed places; detmem: the detected-memory table's size */
static void
mkrom(unsigned detmem)
{
	int h = 0x200;

	memset(rom, 0, sizeof rom);
	rom[0] = 0x55;
	rom[1] = 0xaa;
	rom[2] = 0x80;
	w16(0x18, 0x1c0);
	memcpy(rom + 0x1c0, "PCIR", 4);
	w16(0x48, h);
	rom[h] = 1;
	rom[h + 6] = 0x70;
	w16(h + 0x0c, 0x300);
	w16(h + 0x46, 0x400);
	w16(h + 0x48, 0x501);
	w16(h + 0x4e, 0x600);
	w16(h + 0x52, 0x700);
	w16(h + 0x5e, 0x800);

	/* ASIC_INIT_1: write, and/or, delay, wait for the memory controller */
	w16(0x300, 0x00e4); w32(0x302, 0x11223344);
	w16(0x306, 0x4140); w32(0x308, 0xffff00ff); w32(0x30c, 0x00002500);
	w16(0x310, 0x8000); w16(0x312, 100);
	w16(0x314, 0xa009); w16(0x316, 50);
	/* PLL_INIT: write, byte 1 and/or, the five waits */
	rom[0x400] = 0x0a; w32(0x401, 0xaabbccdd);
	rom[0x405] = 0x52; rom[0x406] = 1; rom[0x407] = 0x0f; rom[0x408] = 0x30;
	rom[0x409] = 0x81; rom[0x40a] = 0x82; rom[0x40b] = 0x83;
	rom[0x40c] = 0x84; rom[0x40d] = 0xc5;
	/* MEM_CONFIG revision 0: 32 MB, mapping 0x25; RAM_RESET after it */
	rom[0x501] = 0x20; rom[0x502] = 0x25;
	rom[0x506] = 0x31; w16(0x507, 0x0132);
	rom[0x509] = 0x0f; rom[0x50a] = 0xff;
	/* ASIC_INIT_2, DYN_CLK_1 */
	w16(0x600, 0x2180); w32(0x602, 2);
	rom[0x700] = 0x0d; w32(0x701, 0x12345678);
	/* misc info: ASIC_INIT_3, ASIC_INIT_4, detected memory */
	rom[0x800] = 1;
	w16(0x803, 0x900);
	w16(0x805, 0xa00);
	w16(0x807, 0xb00);
	w16(0x900, 0x0184); w32(0x902, 3);
	w16(0xa00, 0x0188); w32(0xa02, 4);
	w32(0xb01, 0x00002900);
	w16(0xb05, detmem);
}

static void
reset(void)
{
	memset(reg, 0, sizeof reg);
	memset(pll, 0, sizeof pll);
	memset(vram, 0, sizeof vram);
	reg[0x140 / 4] = 0x12345670;
	reg[0x158 / 4] = 0xabcd1234;
	pll[0x12] = 0x0000ffff;
	pll[0x14] = 1L << 24 | 5;
	busy = 2;
	dll = 2;
	stuck = 0;
	ngot = npoll = 0;
}

static struct op head[] = {
	{ 'W', 0x0e4, 0x11223344 }, { 'W', 0x140, 0x12342570 }, { 'D', 0, 100 }, { 'r', 0x150, 0 },
	{ 'P', 0x0a, 0xaabbccdd }, { 'P', 0x12, 0x00003fff }, { 'D', 0, 150 }, { 'D', 0, 5000 },
	{ 'p', 0x14, 0 }, { 'p', 0x14, 0 }, { 'p', 0x14, 0 },			/* MC busy twice */
	{ 'p', 0x14, 0 }, { 'p', 0x14, 0 },					/* DLL ready second time */
	{ 'p', 0x14, 0 }, { 'P', 0x14, 0x00080005 }, { 'D', 0, 10000 },	/* clock gating bit cleared */
	{ 'W', 0x180, 2 },							/* ASIC_INIT_2 */
	{ 'W', 0x188, 4 },							/* ASIC_INIT_4 */
	{ 'W', 0x158, 0xabcd0132 }, { 'W', 0x158, 0x3bcd0132 }, { 'r', 0x150, 0 },	/* RAM_RESET */
	{ 'W', 0x184, 3 },							/* ASIC_INIT_3 */
};
#define	NHEAD	(sizeof head / sizeof head[0])

static int
same(char *name, struct op *want, int n, int from)
{
	int i;

	for (i = 0; i < n; i++)
		if (from + i >= ngot || got[from + i].k != want[i].k || got[from + i].r != want[i].r ||
		    got[from + i].v != want[i].v) {
			if (from + i < ngot)
				t_fail(name, "op %d: got %c %x %lx, want %c %x %lx", from + i,
				    got[from + i].k, got[from + i].r, got[from + i].v, want[i].k, want[i].r, want[i].v);
			else
				t_fail(name, "op %d: missing, want %c %x %lx", from + i, want[i].k, want[i].r, want[i].v);
			return 0;
		}
	return 1;
}

static void
run(char *name, unsigned detmem, int r100, struct op *tail, int ntail)
{
	int e;

	mkrom(detmem);
	reset();
	e = rdi_init(rom, (unsigned long)ROMLEN, r100);
	if (e != 0) {
		t_fail(name, "rdi_init %d", e);
		return;
	}
	if (!same(name, head, NHEAD, 0) || !same(name, tail, ntail, NHEAD))
		return;
	t_check(name, ngot == NHEAD + ntail, "%d ops, want %d", ngot, (int)(NHEAD + ntail));
}

int
main(void)
{
	static struct op det[] = { { 'W', 0x0f8, 64L << 20 }, { 'P', 0x0d, 0x12345678 } };
	static struct op det100[] = { { 'W', 0x140, 0x2900 }, { 'W', 0x0f8, 64L << 20 }, { 'P', 0x0d, 0x12345678 } };
	static struct op cfg[] = { { 'W', 0x0f8, 32L << 20 }, { 'P', 0x0d, 0x12345678 } };
	static struct op cfg2[] = { { 'W', 0x0f8, 64L << 20 }, { 'P', 0x0d, 0x12345678 } };
	static struct op probe[] = { { 'W', 0x140, 0x2900 }, { 'W', 0x140, 0x2500 }, { 'W', 0x0f8, 32L << 20 },
	    { 'P', 0x0d, 0x12345678 } };
	int i, e;

	t_init("radeon", 60);

	/* header checks: nothing touches the card on a refusal */
	mkrom(64);
	reset();
	rom[0] = 0;
	e = rdi_init(rom, (unsigned long)ROMLEN, 0);
	t_check("no_signature", e == RDI_NOROM && ngot == 0, "%d, %d ops", e, ngot);
	mkrom(64);
	rom[0x1c0] = 'X';
	t_check("no_pcir", rdi_check(rom, (unsigned long)ROMLEN) == RDI_NOROM, "%d", rdi_check(rom, (unsigned long)ROMLEN));
	mkrom(64);
	rom[0x1c0 + 0x14] = 1;
	t_check("open_firmware", rdi_check(rom, (unsigned long)ROMLEN) == RDI_NOTX86, "%d", rdi_check(rom, (unsigned long)ROMLEN));
	mkrom(64);
	w16(0x48, 0);
	t_check("no_header", rdi_check(rom, (unsigned long)ROMLEN) == RDI_NOTABLES, "%d", rdi_check(rom, (unsigned long)ROMLEN));
	mkrom(64);
	memcpy(rom + 0x204, "ATOM", 4);
	t_check("atom", rdi_check(rom, (unsigned long)ROMLEN) == RDI_ATOM, "%d", rdi_check(rom, (unsigned long)ROMLEN));
	mkrom(64);
	memcpy(rom + 0x204, "MOTA", 4);
	t_check("mota", rdi_check(rom, (unsigned long)ROMLEN) == RDI_ATOM, "%d", rdi_check(rom, (unsigned long)ROMLEN));
	mkrom(64);
	t_check("legacy", rdi_check(rom, (unsigned long)ROMLEN) == 0, "%d", rdi_check(rom, (unsigned long)ROMLEN));

	/* the whole sequence, memory size from each source */
	run("sequence_r200", 64, 0, det, 2);
	run("sequence_r100", 64, 1, det100, 3);
	run("memconfig_r200", 0, 0, cfg, 2);
	/* revision 1 memory config: size in 2 MB units */
	mkrom(0);
	rom[0x500] = 1;
	reset();
	rdi_init(rom, (unsigned long)ROMLEN, 0);
	if (same("memconfig_rev1", head, NHEAD, 0) && same("memconfig_rev1", cfg2, 2, NHEAD))
		t_pass("memconfig_rev1");

	/* R100 with no detected size: each MB of the 32 written and read back */
	run("memdetect_r100", 0, 1, probe, 4);
	for (i = 0; i < 32 && vram[i] == 0xdeadbeef; i++)
		;
	t_check("memdetect_r100_probed", i == 32, "%d MB probed", i);

	/* a card that never reports ready: each wait gives up after its count */
	mkrom(64);
	reset();
	stuck = 1;
	busy = 100000;
	rdi_init(rom, (unsigned long)ROMLEN, 0);
	t_check("timeouts", npoll == 50 + 1000 + 1000 + 1 + 20000,
	    "%d polls, want %d", npoll, 50 + 1000 + 1000 + 1 + 20000);

	/* a ROM cut short in its tables: bounded, no fault */
	mkrom(64);
	reset();
	e = rdi_init(rom, 0x302UL, 0);
	t_check("truncated", e == 0 && ngot < 20, "%d, %d ops", e, ngot);
	return t_done();
}
