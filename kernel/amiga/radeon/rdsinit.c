/*
 * Start a cold R100/R200-class Radeon from the init tables in its
 * legacy (COMBIOS) x86 ROM, without running ROM code.  Table layout,
 * opcodes and order follow the Linux radeon driver's combios code:
 *
 * Copyright 2004 ATI Technologies Inc., Markham, Ontario
 * Copyright 2007-8 Advanced Micro Devices, Inc.
 * Copyright 2008 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Dave Airlie, Alex Deucher
 *
 * The caller supplies register access (rdi_rl, rdi_wl, rdi_rpll,
 * rdi_wpll, values in host order) and rdi_delay in microseconds.
 * The ROM is a copy in memory, little-endian, every read bounds-checked.
 */
#include "rdsinit.h"

extern unsigned long rdi_rl(), rdi_rpll();
extern void rdi_wl(), rdi_wpll(), rdi_delay();

#define	MM_INDEX	0x0000
#define	MM_DATA		0x0004
#define	 MM_APER	0x80000000UL
#define	MEM_CNTL	0x0140
#define	 HALF_MODE	(1 << 3)
#define	MC_STATUS	0x0150		/* MEM_STR_CNTL on write */
#define	 MC_IDLE	(1 << 2)
#define	 PWRUP_DONE	0x03
#define	MEM_SDRAM_MODE	0x0158
#define	CONFIG_MEMSIZE	0x00f8

/* PLL */
#define	CLK_PWRMGT_CNTL	0x14
#define	 MC_BUSY	(1L << 16)
#define	 DLL_READY	(1L << 19)
#define	 CG_NO1_DEBUG_0	(1L << 24)

static unsigned char *rom;
static unsigned long romlen;

static unsigned
rb(o)
unsigned long o;
{
	return o < romlen ? rom[o] : 0;
}

static unsigned
rw(o)
unsigned long o;
{
	return rb(o) | rb(o + 1) << 8;
}

static unsigned long
rq(o)
unsigned long o;
{
	return (unsigned long)rw(o) | (unsigned long)rw(o + 2) << 16;
}

static void
mask(r, and, or)
int r;
unsigned long and, or;
{
	rdi_wl(r, (rdi_rl(r) & and) | or);
}

static void
pllmask(r, and, or)
int r;
unsigned long and, or;
{
	rdi_wpll(r, (rdi_rpll(r) & and) | or);
}

static int
check(v, len)
unsigned char *v;
unsigned long len;
{
	unsigned long pcir;

	rom = v;
	romlen = len;
	if (rb(0) != 0x55 || rb(1) != 0xaa)
		return RDI_NOROM;
	pcir = rw(0x18);
	if (rb(pcir) != 'P' || rb(pcir + 1) != 'C' || rb(pcir + 2) != 'I' || rb(pcir + 3) != 'R')
		return RDI_NOROM;
	/* code type: 0 x86, 1 Open Firmware */
	if (rb(pcir + 0x14) != 0)
		return RDI_NOTX86;
	if (rw(0x48) == 0)
		return RDI_NOTABLES;
	pcir = rw(0x48) + 4;
	if ((rb(pcir) == 'A' && rb(pcir + 1) == 'T' && rb(pcir + 2) == 'O' && rb(pcir + 3) == 'M') ||
	    (rb(pcir) == 'M' && rb(pcir + 1) == 'O' && rb(pcir + 2) == 'T' && rb(pcir + 3) == 'A'))
		return RDI_ATOM;
	return 0;
}

int
rdi_check(v, len)
unsigned char *v;
unsigned long len;
{
	return check(v, len);
}

/* a table pointer in the header; 0 past the header's own size */
static unsigned
table(at)
unsigned at;
{
	unsigned h = rw(0x48);

	return at < rb(h + 6) ? rw(h + at) : 0;
}

/* tables found through the misc info table: 3 ASIC_INIT_3, 5 ASIC_INIT_4, 7 detected memory */
static unsigned
misc(at)
unsigned at;
{
	unsigned m = table(0x5e);

	return m && rb(m) > 0 ? rw(m + at) : 0;
}

static void
mmio(o)
unsigned long o;
{
	unsigned id, n;
	int r;
	unsigned long a, b;

	while (o && o < romlen && (id = rw(o)) != 0) {
		r = id & 0x1fff;
		o += 2;
		switch (id >> 13) {
		case 0:		/* write */
		case 1:
			rdi_wl(r, rq(o));
			o += 4;
			break;
		case 2:		/* and, or */
		case 3:
			a = rq(o);
			b = rq(o + 4);
			o += 8;
			mask(r, a, b);
			break;
		case 4:
			rdi_delay((long)rw(o));
			o += 2;
			break;
		case 5:		/* wait, up to n reads */
			n = rw(o);
			o += 2;
			if (r == 8)
				while (n-- && (rdi_rpll(CLK_PWRMGT_CNTL) & MC_BUSY))
					;
			else if (r == 9)
				while (n-- && !(rdi_rl(MC_STATUS) & MC_IDLE))
					;
			break;
		}
	}
}

static void
pll(o)
unsigned long o;
{
	unsigned c, r, n, sh;
	unsigned long v;

	while (o && o < romlen && (c = rb(o)) != 0) {
		r = c & 0x3f;
		o++;
		switch (c >> 6) {
		case 0:
			rdi_wpll(r, rq(o));
			o += 4;
			break;
		case 1:		/* byte number, and, or */
			sh = rb(o) * 8;
			pllmask(r, (unsigned long)rb(o + 1) << sh | ~(0xffUL << sh),
			    (unsigned long)rb(o + 2) << sh);
			o += 3;
			break;
		default:
			n = 1000;
			switch (r) {
			case 1:
				rdi_delay(150L);
				break;
			case 2:
				rdi_delay(5000L);
				break;
			case 3:
				while (n-- && (rdi_rpll(CLK_PWRMGT_CNTL) & MC_BUSY))
					;
				break;
			case 4:
				while (n-- && !(rdi_rpll(CLK_PWRMGT_CNTL) & DLL_READY))
					;
				break;
			case 5:
				v = rdi_rpll(CLK_PWRMGT_CNTL);
				if (v & CG_NO1_DEBUG_0) {
					rdi_wpll(CLK_PWRMGT_CNTL, v & ~CG_NO1_DEBUG_0);
					rdi_delay(10000L);
				}
				break;
			}
		}
	}
}

/* SDRAM mode register writes; 0x0f waits for the memory to power up */
static void
ramreset(o)
unsigned long o;
{
	unsigned c;
	int n;

	while (o && o < romlen && (c = rb(o)) != 0xff) {
		o++;
		if (c == 0x0f) {
			for (n = 20000; n-- && (rdi_rl(MC_STATUS) & PWRUP_DONE) != PWRUP_DONE; )
				;
		} else {
			mask(MEM_SDRAM_MODE, 0xffff0000UL, (unsigned long)rw(o));
			o += 2;
			mask(MEM_SDRAM_MODE, 0x6fffffffUL, (unsigned long)c << 24);
		}
	}
}

/* each MB written and read back through the indexed aperture */
static unsigned long
detect(ram, map)
unsigned ram, map;
{
	unsigned long c = rdi_rl(MEM_CNTL), size;

	if (c & HALF_MODE)
		ram /= 2;
	size = ram;
	rdi_wl(MEM_CNTL, (c & ~0xff00UL) | (unsigned long)(map & 0xff) << 8);
	(void)rdi_rl(MEM_CNTL);
	while (ram--) {
		rdi_wl(MM_INDEX, (unsigned long)ram << 20 | MM_APER);
		rdi_wl(MM_DATA, 0xdeadbeefUL);
		rdi_wl(MM_INDEX, (unsigned long)ram << 20 | MM_APER);
		if (rdi_rl(MM_DATA) != 0xdeadbeefUL)
			return 0;
	}
	return size;
}

static void
ramsize(r100)
int r100;
{
	unsigned long o, size = 0;

	if ((o = misc(7)) != 0 && rb(o) < 3) {
		size = rw(o + 5);
		if (r100)
			rdi_wl(MEM_CNTL, rq(o + 1));
	}
	if (size == 0 && (o = table(0x48)) != 0) {
		if (rb(o - 1) >= 1)
			size = rb(o) * 2;
		else if (!r100)
			size = rb(o);
		else
			for (; o < romlen && rb(o); o += 2) {
				size = rb(o);
				if (rb(o + 1) != 0x25)
					size *= 2;
				if ((size = detect((unsigned)size, rb(o + 1))) != 0)
					break;
			}
	}
	rdi_wl(CONFIG_MEMSIZE, size << 20);
}

/*
 * Run the ROM's init tables in the order the card's own start-up code
 * does.  r100: an R100/RV100/RV200, whose memory is sized differently.
 */
int
rdi_init(v, len, r100)
unsigned char *v;
unsigned long len;
int r100;
{
	unsigned long o;
	int e;

	if ((e = check(v, len)) != 0)
		return e;
	mmio((unsigned long)table(0x0c));		/* ASIC_INIT_1 */
	pll((unsigned long)table(0x46));		/* PLL_INIT */
	mmio((unsigned long)table(0x4e));		/* ASIC_INIT_2 */
	mmio((unsigned long)misc(5));			/* ASIC_INIT_4 */
	/* RAM_RESET: after the memory config table's zero-ended list and two bytes */
	if ((o = table(0x48)) != 0) {
		while (o < romlen && rb(o))
			o++;
		ramreset(o + 3);
	}
	mmio((unsigned long)misc(3));			/* ASIC_INIT_3 */
	ramsize(r100);
	pll((unsigned long)table(0x52));		/* DYN_CLK_1 */
	return 0;
}
