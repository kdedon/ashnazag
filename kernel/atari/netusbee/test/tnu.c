/*
 * Host test: the chip core against a model of the cartridge-port
 * decoding and the DP8390 as its datasheet describes it.
 *
 *   cc -DNU_HOST -I.. -o tnu tnu.c ../nuchip.c && ./tnu
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "nu.h"

static int fails, checks;
int sim_clkdue;
#define CHECK(c, m)	do { checks++; if (!(c)) { fails++; \
			printf("FAIL %s (line %d)\n", m, __LINE__); } } while (0)

/* ---------------------------------------------------------- the model */

enum { NOCARD, ROMCART, CARD } mode;
static struct {
	int cr, isr, pstart, pstop, bnry, tpsr, tbcr, rsar, rbcr, rcr, tcr;
	int dcr, imr, curr, tsr, cntr[3], crda, dmacmd;
	unsigned char par[6], mar[8], mem[0x8000];
	int txpend;
	long nwrites, badaddr;
} c;
static unsigned char wire[8][1600];
static int wirelen[8], nwire;

static void
chipreset()
{
	c.cr = CR_NODMA | CR_STP;
	c.isr = ISR_RST;
	c.dmacmd = 0;
	c.txpend = 0;
}

static int
memrd(int a)
{
	if (a < 0x20)
		return c.mem[a >> 1];	/* PROM: each byte twice */
	if (a >= 0x4000 && a < 0x8000)
		return c.mem[a];
	return 0xff;
}

static void
memwr(int a, int v)
{
	if (a >= 0x4000 && a < 0x8000)
		c.mem[a] = v;
}

static int
rd(int r)
{
	int v, pg = c.cr >> 6;

	if (r == NU_DATA) {
		if (c.dmacmd != CR_RREAD || c.rbcr <= 0)
			return 0;
		v = memrd(c.crda++);
		if (--c.rbcr == 0)
			c.isr |= ISR_RDC;
		return v;
	}
	if (r == NU_RESET) {
		chipreset();
		return 0;
	}
	if (r == NU_CR)
		return c.cr;
	if (pg == 1) {
		if (r >= NU_PAR0 && r < NU_PAR0 + 6)
			return c.par[r - NU_PAR0];
		if (r == NU_CURR)
			return c.curr;
		return c.mar[r - NU_MAR0];
	}
	switch (r) {
	case NU_BNRY: return c.bnry;
	case NU_TSR: return c.tsr;
	case NU_ISR: return c.isr;
	case NU_ID0: return 'P';
	case NU_ID1: return 'p';
	case NU_CNTR0: case NU_CNTR1: case NU_CNTR2:
		v = c.cntr[r - NU_CNTR0];
		c.cntr[r - NU_CNTR0] = 0;
		return v;
	}
	return 0;
}

static void
wr(int r, int v)
{
	int pg = c.cr >> 6;

	c.nwrites++;
	if (r == NU_DATA) {
		if (c.dmacmd == CR_RWRITE && c.rbcr > 0) {
			memwr(c.crda++, v);
			if (--c.rbcr == 0)
				c.isr |= ISR_RDC;
		}
		return;
	}
	if (r == NU_RESET) {
		chipreset();
		return;
	}
	if (r == NU_CR) {
		c.cr = (v & ~CR_TXP) | (c.cr & CR_TXP);
		if (v & CR_STP) {
			c.cr &= ~(CR_STA | CR_TXP);
			c.isr |= ISR_RST;
		} else if (v & CR_STA) {
			c.cr &= ~CR_STP;
		}
		if (v & (CR_RREAD | CR_RWRITE)) {
			c.dmacmd = v & (CR_RREAD | CR_RWRITE);
			c.crda = c.rsar;
		}
		if ((v & CR_TXP) && (c.cr & CR_STA)) {
			c.cr |= CR_TXP;
			c.txpend = 1;
		}
		return;
	}
	if (pg == 1) {
		if (r >= NU_PAR0 && r < NU_PAR0 + 6)
			c.par[r - NU_PAR0] = v;
		else if (r == NU_CURR)
			c.curr = v;
		else if (r >= NU_MAR0)
			c.mar[r - NU_MAR0] = v;
		return;
	}
	switch (r) {
	case NU_PSTART: c.pstart = v; break;
	case NU_PSTOP: c.pstop = v; break;
	case NU_BNRY: c.bnry = v; break;
	case NU_TPSR: c.tpsr = v; break;
	case NU_TBCR0: c.tbcr = (c.tbcr & 0xff00) | v; break;
	case NU_TBCR1: c.tbcr = (c.tbcr & 0xff) | v << 8; break;
	case NU_ISR: c.isr &= ~v; break;
	case NU_RSAR0: c.rsar = (c.rsar & 0xff00) | v; break;
	case NU_RSAR1: c.rsar = (c.rsar & 0xff) | v << 8; break;
	case NU_RBCR0: c.rbcr = (c.rbcr & 0xff00) | v; break;
	case NU_RBCR1: c.rbcr = (c.rbcr & 0xff) | v << 8; break;
	case NU_RCR: c.rcr = v; break;
	case NU_TCR: c.tcr = v; break;
	case NU_DCR: c.dcr = v; break;
	case NU_IMR: c.imr = v; break;
	}
}

/* The port: ROM4 reads, ROM3 reads that carry a write on A1-A8. */
unsigned char
sim_bus(unsigned long a)
{
	unsigned long off;

	if (mode == NOCARD)
		return 0xff;
	if (mode == ROMCART)
		return (unsigned char)(0xab ^ (a >> 9));
	if (a >= NU_RDBASE && a < NU_RDBASE + 0x10000) {
		off = a - NU_RDBASE;
		if (off & 0x1ff || off >> 9 >= 0x20) {
			c.badaddr++;
			return 0;
		}
		return rd(off >> 9);
	}
	if (a >= NU_WRBASE && a < NU_WRBASE + 0x10000) {
		off = a - NU_WRBASE;
		if (off & 1 || off >> 9 >= 0x20) {
			c.badaddr++;
			return 0;
		}
		wr(off >> 9, (off >> 1) & 0xff);
		return 0;
	}
	c.badaddr++;
	return 0;
}

/* The transmitter finishes the frame on the wire. */
static void
sim_txdone()
{
	if (!c.txpend)
		return;
	if (!(c.tcr & TCR_LOOP) && nwire < 8) {
		memcpy(wire[nwire], &c.mem[c.tpsr << 8], c.tbcr);
		wirelen[nwire++] = c.tbcr;
	}
	c.txpend = 0;
	c.cr &= ~CR_TXP;
	c.tsr = TSR_PTX;
	c.isr |= ISR_PTX;
}

/* A frame arrives: stored at CURR unless it would reach BNRY. */
static int
sim_rx(unsigned char *f, int len)
{
	int cnt = 4 + len + 4, pages = (cnt + 255) >> 8, ring, room, a, i;

	if (!(c.cr & CR_STA) || (c.tcr & TCR_LOOP) || (c.rcr & RCR_MON))
		return 0;
	ring = c.pstop - c.pstart;
	room = (c.bnry - c.curr + ring) % ring;
	if (pages > room) {
		c.isr |= ISR_OVW | ISR_RST;
		c.cntr[2]++;
		return 0;
	}
	a = c.curr << 8;
	c.mem[a] = RSR_PRX;
	c.mem[a + 1] = c.pstart + (c.curr - c.pstart + pages) % ring;
	c.mem[a + 2] = cnt;
	c.mem[a + 3] = cnt >> 8;
	for (i = 0; i < len + 4; i++) {
		a = (c.curr << 8) + 4 + i;
		if (a >= c.pstop << 8)
			a -= ring << 8;
		c.mem[a] = i < len ? f[i] : 0xcc;
	}
	c.curr = c.mem[c.curr << 8 | 1];
	c.isr |= ISR_PRX;
	return 1;
}

/* ------------------------------------------------------ the receiver */

static unsigned char got[512][1600];
static int gotlen[512], ngot, ntxdone;

void
nu_input(struct nu_softc *sc, unsigned char *f, int len)
{
	if (ngot < 512) {
		memcpy(got[ngot], f, len);
		gotlen[ngot] = len;
	}
	ngot++;
}

void
nu_txdone(struct nu_softc *sc)
{
	ntxdone++;
}

static void
mkframe(unsigned char *f, int len, int seed)
{
	int i;

	for (i = 0; i < len; i++)
		f[i] = (i * 7 + seed * 13) ^ (i >> 8);
}

static struct nu_softc sc;

int
main()
{
	static unsigned char f[1600], sent[64][1600];
	static int sentlen[64];
	unsigned char ea[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
	unsigned char mc1[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
	unsigned char mc2[6] = { 0x09, 0x00, 0x07, 0xff, 0xff, 0xff };
	int i, k, n, ok, lens[] = { 60, 61, 100, 255, 256, 500, 1000, 1514 };

	/* absent: floating bus, then a ROM cartridge */
	mode = NOCARD;
	CHECK(nu_probe(&sc) < 0, "no card: absent");
	mode = ROMCART;
	CHECK(nu_probe(&sc) < 0, "ROM cartridge: absent");

	/* present */
	mode = CARD;
	memset(&c, 0, sizeof(c));
	memcpy(c.mem, ea, 6);
	c.mem[6] = c.mem[7] = 0x57;
	chipreset();
	c.cr = 0x22;		/* left running by the ROM */
	CHECK(nu_probe(&sc) == 0, "card found");
	CHECK(sc.rtl == 1, "RTL8019 ID");
	CHECK(sc.memlo == 0x40 && sc.memhi == 0x80, "16 KB at 0x4000");
	CHECK(memcmp(sc.prom, ea, 6) == 0, "address from the doubled PROM");
	CHECK(c.dcr == (DCR_LS | DCR_FT1), "DCR byte-wide");

	memcpy(sc.ea, sc.prom, 6);
	sc.rcr = RCR_AB;
	CHECK(nu_init(&sc) == 0, "init");
	CHECK(memcmp(c.par, ea, 6) == 0, "PAR loaded");
	CHECK(c.pstart == 0x4c && c.pstop == 0x80, "ring 0x4c-0x80");
	CHECK(c.curr == 0x4c && c.bnry == 0x7f, "CURR/BNRY");
	CHECK(c.tcr == 0 && c.rcr == RCR_AB && (c.cr & CR_STA), "running, broadcast");
	CHECK(c.imr == 0, "no interrupts");

	/* transmit: two buffers, a short frame padded */
	nwire = 0;
	mkframe(f, 42, 1);
	memcpy(sc.txbuf, f, 42);
	CHECK(nu_txroom(&sc), "room 1");
	nu_txstart(&sc, 42);
	CHECK(c.txpend && c.tpsr == 0x40 && c.tbcr == 60, "first frame on the wire, padded");
	mkframe(f, 1514, 2);
	CHECK(nu_txroom(&sc), "room 2");
	memcpy(sc.txbuf, f, 1514);
	nu_txstart(&sc, 1514);
	CHECK(!nu_txroom(&sc), "both buffers busy");
	sim_txdone();
	CHECK(nu_txroom(&sc), "room after PTX");
	CHECK(c.txpend && c.tpsr == 0x46 && c.tbcr == 1514, "second frame started");
	sim_txdone();
	nu_poll(&sc);
	CHECK(nwire == 2 && wirelen[0] == 60 && wirelen[1] == 1514, "two frames sent");
	mkframe(f, 42, 1);
	CHECK(memcmp(wire[0], f, 42) == 0 && wire[0][42] == 0 && wire[0][59] == 0, "frame 1 bytes");
	mkframe(f, 1514, 2);
	CHECK(memcmp(wire[1], f, 1514) == 0, "frame 2 bytes");
	CHECK(sc.st.opackets == 2 && ntxdone >= 1, "tx accounted");
	CHECK(c.badaddr == 0, "every access decodes");

	/* receive: 300 frames over many ring wraps */
	ngot = 0;
	n = 0;
	ok = 1;
	for (i = 0; i < 300; i++) {
		int len = lens[i % 8];

		mkframe(sent[i % 64], len, i);
		sentlen[i % 64] = len;
		CHECK(sim_rx(sent[i % 64], len), "frame accepted");
		if (i % 3 == 2) {
			k = ngot;
			nu_poll(&sc);
			for (; k < ngot; k++, n++)
				if (gotlen[k] != sentlen[n % 64] ||
				    memcmp(got[k], sent[n % 64], gotlen[k]))
					ok = 0;
		}
	}
	CHECK(ngot == 300 && n == 300, "300 frames delivered");
	CHECK(ok, "300 frames intact, CRC stripped");
	CHECK(sc.st.rxbad == 0 && sc.st.ovw == 0, "no ring errors");

	/* overflow: fill the ring, recover, carry on */
	ngot = 0;
	for (i = 0, k = 0; i < 20; i++) {
		mkframe(f, 1514, i);
		k += sim_rx(f, 1514);
	}
	CHECK(c.isr & ISR_OVW, "ring overflowed");
	CHECK(k == 8, "8 full frames fit in 52 pages");
	nu_poll(&sc);
	CHECK(sc.st.ovw == 1 && ngot == 8, "overflow drained");
	CHECK(!(c.isr & ISR_OVW) && c.tcr == 0 && (c.cr & CR_STA), "running again");
	mkframe(f, 100, 99);
	CHECK(sim_rx(f, 100), "accepts after overflow");
	nu_poll(&sc);
	CHECK(ngot == 9 && gotlen[8] == 100 && memcmp(got[8], f, 100) == 0, "frame after overflow");

	/* a corrupt header resets the ring and nothing else */
	mkframe(f, 200, 5);
	sim_rx(f, 200);
	{
		int p = sc.rxnext;
		c.mem[(p << 8) + 1] = 0x10;	/* next page outside the ring */
	}
	nu_poll(&sc);
	CHECK(sc.st.rxbad == 1 && sc.rxnext == c.curr, "bad header: ring skipped");
	mkframe(f, 300, 6);
	sim_rx(f, 300);
	k = ngot;
	nu_poll(&sc);
	CHECK(ngot == k + 1 && memcmp(got[k], f, 300) == 0, "receives after a bad header");

	/* a zeroed header pointing at itself */
	mkframe(f, 200, 7);
	sim_rx(f, 200);
	{
		int p = sc.rxnext;

		c.mem[(p << 8) + 1] = p;
		c.mem[(p << 8) + 2] = c.mem[(p << 8) + 3] = 0;
	}
	k = sc.st.rxbad;
	nu_poll(&sc);
	CHECK(sc.st.rxbad == k + 1 && sc.rxnext == c.curr, "zero count: ring skipped");

	/* a due clock interrupt ends the burst after one frame */
	for (i = 0; i < 3; i++) {
		mkframe(f, 1514, i);
		sim_rx(f, 1514);
	}
	k = ngot;
	sim_clkdue = 1;
	nu_poll(&sc);
	CHECK(ngot == k + 1, "clock due: one frame");
	sim_clkdue = 0;
	nu_poll(&sc);
	CHECK(ngot == k + 3, "rest on the next poll");

	/* multicast hash: CRC top 6 bits 31 and 63 */
	nu_addmc(&sc, mc1);
	nu_addmc(&sc, mc2);
	CHECK(c.mar[3] == 0x80 && c.mar[7] == 0x80, "hash bits");
	CHECK(c.rcr == (RCR_AB | RCR_AM), "RCR multicast on");
	nu_addmc(&sc, mc1);
	nu_delmc(&sc, mc1);
	CHECK(c.mar[3] == 0x80, "refcounted");
	nu_delmc(&sc, mc1);
	nu_delmc(&sc, mc2);
	CHECK(c.mar[3] == 0 && c.mar[7] == 0 && c.rcr == RCR_AB, "multicast off");

	/* a transmit that never completes gets the chip re-initialised */
	memcpy(sc.txbuf, f, 100);
	nu_txstart(&sc, 100);
	for (i = 0; i < 130; i++)
		nu_poll(&sc);
	CHECK(sc.st.txtimeout == 1 && sc.st.reinit >= 1 && sc.txcur < 0, "tx timeout re-init");
	CHECK(c.cr & CR_STA, "running after re-init");

	/* a stopped chip is noticed */
	wr(NU_CR, CR_NODMA | CR_STP);
	k = sc.st.reinit;
	nu_poll(&sc);
	CHECK(sc.st.reinit == k + 1 && (c.cr & CR_STA), "stopped chip restarted");
	CHECK(c.badaddr == 0, "every access decodes");

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
