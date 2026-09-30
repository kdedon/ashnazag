/*
 * A behavioural model of the DP83932 in 32-bit mode, big enough to run
 * the driver core on the host: CR commands, the RRA/RDA receive path
 * with EOBC, RBE and RDE, the TDA list with EOL restart, and CAM load.
 * Descriptor fields are host-order longwords (the driver writes them
 * the same way), so no byte swapping is modelled.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "simsonic.h"

#define SIMBASE	0x00200100u	/* chip address of simmem[0]; not 4 KB aligned */

unsigned char simmem[SIMMEMSZ];
struct sim sim;

sn_u32
sim_pa(unsigned char *p)
{
	if (p < simmem || p > simmem + SIMMEMSZ) {
		fprintf(stderr, "sim_pa: pointer outside the pool\n");
		abort();
	}
	return SIMBASE + (sn_u32)(p - simmem);
}

unsigned char *
sim_va(sn_u32 a)
{
	if (a < SIMBASE || a >= SIMBASE + SIMMEMSZ) {
		sim.errors++;
		fprintf(stderr, "sim: chip access outside the pool: 0x%x\n", a);
		abort();
	}
	return simmem + (a - SIMBASE);
}

static sn_u32 *
L(sn_u32 a)
{
	if (a & 3) {
		sim.errors++;
		fprintf(stderr, "sim: unaligned descriptor 0x%x\n", a);
	}
	return (sn_u32 *)sim_va(a);
}

#define R(n)	(sim.reg[n])
#define FULL(u, lo)	((sn_u32)R(u) << 16 | (lo))

static void
fetch_rba(void)
{
	sn_u32 *e;

	if (R(SN_RRP) == R(SN_RWP)) {
		sim.rbe_stall = 1;
		R(SN_ISR) |= INT_RBE;
		return;
	}
	e = L(FULL(SN_URRA, R(SN_RRP)));
	sim.crba = (e[1] & 0xffff) << 16 | (e[0] & 0xffff);
	sim.rbwc = (e[3] & 0xffff) << 16 | (e[2] & 0xffff);
	sim.rbaseq++;
	sim.pktseq = 0;
	R(SN_RRP) += sizeof(struct sn_rra);
	if (R(SN_RRP) == R(SN_REA))
		R(SN_RRP) = R(SN_RSA);
	sim.rbe_stall = 0;
	sim.have_rba = 1;
}

static void
cam_load(void)
{
	sn_u32 a = FULL(SN_URRA, R(SN_CDP));
	sn_u32 *c;
	int e;

	while (R(SN_CDC) > 0) {
		c = L(a);
		e = c[0] & 0xf;
		sim.cam[e][0] = c[1]; sim.cam[e][1] = c[1] >> 8;
		sim.cam[e][2] = c[2]; sim.cam[e][3] = c[2] >> 8;
		sim.cam[e][4] = c[3]; sim.cam[e][5] = c[3] >> 8;
		a += sizeof(struct sn_cda);
		R(SN_CDC)--;
	}
	R(SN_CE) = *L(a) & 0xffff;
	R(SN_CDP) = a & 0xffff;
	R(SN_ISR) |= INT_LCD;
	sim.camloads++;
}

unsigned short
sim_rd(int r)
{
	if (r == SN_CR)
		return (sim.rst ? CR_RST : 0) | (sim.rxen ? CR_RXEN : 0) |
		    (sim.txp ? CR_TXP : 0);
	return R(r);
}

void
sim_wr(int r, int v)
{
	v &= 0xffff;
	switch (r) {
	case SN_CR:
		if (v & CR_RST) {
			sim.rst = 1;
			sim.rxen = sim.txp = 0;
			return;
		}
		if (v == 0 && sim.rst) {
			sim.rst = 0;
			sim.at_eol = sim.rde_stall = sim.rbe_stall = 0;
			sim.have_rba = 0;
			return;
		}
		if (sim.rst)
			return;
		if (v & CR_RXDIS)
			sim.rxen = 0;
		if (v & CR_HTX)
			sim.txp = 0;
		if (v & CR_LCAM)
			cam_load();
		if (v & CR_RRRA)
			fetch_rba();
		if (v & CR_RXEN)
			sim.rxen = 1;
		if (v & CR_TXP) {
			if (sim.lose_txp) {
				sim.lose_txp--;
				return;
			}
			sim.txp = 1;
		}
		return;
	case SN_ISR:
		R(SN_ISR) &= ~v;
		if ((v & INT_RBE) && sim.rbe_stall && R(SN_RRP) != R(SN_RWP))
			fetch_rba();
		return;
	case SN_CTDA:
		sim.at_eol = 0;
		break;
	case SN_CRDA:
		sim.rde_stall = 0;
		break;
	}
	R(r) = v;
}

/* Transmit one frame if the transmitter runs; 1 if a frame went out. */
int
sim_txstep(void)
{
	sn_u32 *t, a, link;
	int size;

	if (!sim.txp || sim.rst)
		return 0;
	if (sim.at_eol) {
		/* restarted after end-of-list: re-read the old link */
		link = L(FULL(SN_UTDA, R(SN_CTDA)))[7];
		if (link & LINK_EOL) {
			sim.txp = 0;
			return 0;
		}
		R(SN_CTDA) = link & 0xfffe;
		sim.at_eol = 0;
	}
	t = L(FULL(SN_UTDA, R(SN_CTDA)));
	size = t[2] & 0xffff;
	if ((t[3] & 0xffff) != 1 || (t[6] & 0xffff) != size || size < 60 ||
	    size > 1514) {
		sim.errors++;
		fprintf(stderr, "sim: bad tda: frags %u size %d/%u\n",
		    t[3], size, t[6]);
	}
	a = (t[5] & 0xffff) << 16 | (t[4] & 0xffff);
	if (sim.nwire < SIMWIRE) {
		memcpy(sim.wire[sim.nwire], sim_va(a), size);
		sim.wirelen[sim.nwire] = size;
		sim.nwire++;
	}
	if (sim.tx_fail) {
		/* a fatal transmit error: status, TXER, transmitter stops */
		sim.tx_fail = 0;
		t[0] = TCR_FU;
		R(SN_ISR) |= INT_TXER;
		link = t[7];
		if (link & LINK_EOL)
			sim.at_eol = 1;
		else
			R(SN_CTDA) = link & 0xfffe;
		sim.txp = 0;
		return 1;
	}
	t[0] = TCR_PTX | (1 << 11);	/* one collision, for the tally */
	R(SN_ISR) |= INT_PTX;
	link = t[7];
	if (link & LINK_EOL) {
		sim.at_eol = 1;
		sim.txp = 0;
	} else
		R(SN_CTDA) = link & 0xfffe;
	return 1;
}

static int
cam_match(const unsigned char *d)
{
	int e;

	for (e = 0; e < 16; e++)
		if ((R(SN_CE) & (1 << e)) && memcmp(sim.cam[e], d, 6) == 0)
			return 1;
	return 0;
}

/* A frame arrives from the wire (len without CRC). */
int
sim_rx(const unsigned char *f, int len)
{
	static const unsigned char bc[6] = { 255, 255, 255, 255, 255, 255 };
	sn_u32 *d, link;
	int bytes, words, st;

	if (!sim.rxen || sim.rst)
		return SIM_OFF;
	st = RCR_PRX;
	if (memcmp(f, bc, 6) == 0) {
		if (!(R(SN_RCR) & RCR_BRD))
			return SIM_FILTERED;
		st |= RCR_BC;
	} else if (f[0] & 1) {
		if (!cam_match(f) && !(R(SN_RCR) & (RCR_AMC | RCR_PRO)))
			return SIM_FILTERED;
		st |= RCR_MC;
	} else if (!cam_match(f) && !(R(SN_RCR) & RCR_PRO))
		return SIM_FILTERED;
	if (sim.rbe_stall || !sim.have_rba) {
		sim.missed++;
		return SIM_NORBA;
	}
	if (sim.rde_stall) {
		link = L(FULL(SN_URDA, sim.llfa))[5];
		if (link & LINK_EOL) {
			R(SN_ISR) |= INT_RDE;
			sim.missed++;
			return SIM_NORDA;
		}
		R(SN_CRDA) = link & 0xfffe;
		sim.rde_stall = 0;
	}
	bytes = len + 4;
	words = ((bytes + 3) & ~3) / 2;
	if (words > (int)sim.rbwc) {
		R(SN_ISR) |= INT_RBAE;
		fetch_rba();
		return SIM_TOOBIG;
	}
	d = L(FULL(SN_URDA, R(SN_CRDA)));
	if ((d[6] & 0xffff) == 0) {
		sim.errors++;
		fprintf(stderr, "sim: descriptor 0x%x still held by the host\n",
		    R(SN_CRDA));
	}
	memcpy(sim_va(sim.crba), f, len);
	memset(sim_va(sim.crba) + len, 0xcc, 4);	/* CRC */
	d[1] = bytes;
	d[2] = sim.crba & 0xffff;
	d[3] = sim.crba >> 16;
	d[4] = (sim.rbaseq & 0xff) << 8 | (sim.pktseq++ & 0xff);
	sim.crba += words * 2;
	sim.rbwc -= words;
	if (sim.rbwc < R(SN_EOBC)) {
		st |= RCR_LPKT;
		fetch_rba();
	}
	d[0] = st;
	d[6] = 0;
	link = d[5];
	if (link & LINK_EOL) {
		sim.rde_stall = 1;
		sim.llfa = R(SN_CRDA);
	} else
		R(SN_CRDA) = link & 0xfffe;
	R(SN_ISR) |= INT_PRX;
	sim.received++;
	return SIM_OK;
}
