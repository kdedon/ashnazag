/*
 * Host tests of the SONIC core against the model in simsonic.c:
 * init and CAM load, receive ring wrap, receive buffer exhaustion and
 * recovery, oversized frames, transmit completion and list restart,
 * a lost TXP and a transmit error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "simsonic.h"

static struct sn_softc sc;
static int bad;

#define NIN	1024
static unsigned char in[NIN][1536];
static int inlen[NIN], nin, ntxdone;

static const unsigned char me[6] = { 0x08, 0x00, 0x07, 0x12, 0x34, 0x56 };
static const unsigned char other[6] = { 0x08, 0x00, 0x07, 0x99, 0x99, 0x99 };
static const unsigned char bcast[6] = { 255, 255, 255, 255, 255, 255 };
static const unsigned char atmc[6] = { 0x09, 0x00, 0x07, 0xff, 0xff, 0xff };

void
sn_input(struct sn_softc *s, unsigned char *p, int len, unsigned int st)
{
	(void)s; (void)st;
	if (nin < NIN) {
		memcpy(in[nin], p, len);
		inlen[nin] = len;
	}
	nin++;
}

void
sn_txdone(struct sn_softc *s)
{
	(void)s;
	ntxdone++;
}

static void
check(int ok, const char *msg)
{
	printf("%s %s\n", ok ? "OK  " : "FAIL", msg);
	if (!ok)
		bad = 1;
}

/* frame: dst, our source, type 0x0800, payload tagged with n */
static int
mkframe(unsigned char *f, const unsigned char *dst, int n, int len)
{
	int i;

	memcpy(f, dst, 6);
	memcpy(f + 6, other, 6);
	f[12] = 0x08; f[13] = 0x00;
	for (i = 14; i < len; i++)
		f[i] = (unsigned char)(n + i);
	f[14] = n >> 8; f[15] = n;
	return len;
}

static int
frame_ok(int k, int n, int len)
{
	unsigned char f[1536];

	mkframe(f, me, n, len);
	return k < NIN && inlen[k] == len && memcmp(in[k], f, len) == 0;
}

static void
reset_model(void)
{
	memset(&sim, 0, sizeof sim);
	nin = ntxdone = 0;
}

static void
t_init(void)
{
	int ok;

	reset_model();
	check(sn_attach(&sc, simmem) == 0, "attach: pool carved");
	check((sim_pa((unsigned char *)sc.tda) & 4095) == 0,
	    "descriptor block 4 KB aligned in chip space");
	check(sim_pa(sc.rxbuf + SN_NRX * SN_BUFSZ) <= sim_pa(simmem) + SIMMEMSZ,
	    "buffers inside the pool");
	memcpy(sc.cam[0], me, 6);
	check(sn_init(&sc) == 0, "init");
	check(sim.reg[SN_DCR] == (DCR_EXBUS | DCR_BMS | DCR_DW | DCR_RFT1 | DCR_TFT0),
	    "DCR = EXBUS|BMS|DW|RFT1|TFT0 (0x803a)");
	check(sim.rxen && sim.have_rba && !sim.rst, "receiver on, first RBA read");
	check(sim.reg[SN_IMR] == SN_IMRVAL, "IMR set");
	ok = sim.reg[SN_CE] == 1 && memcmp(sim.cam[0], me, 6) == 0;
	check(ok, "CAM entry 0 = station address, CE = 0x0001");
	check(sim.reg[SN_RWP] == (sim_pa((unsigned char *)&sc.rra[SN_NRX - 1]) & 0xffff),
	    "RWP one slot short of the ring (guard)");
}

static void
t_cam(void)
{
	unsigned char f[128];

	sc.camvalid = 0x0003;
	memcpy(sc.cam[1], atmc, 6);
	check(sn_filter(&sc) == 0 && sim.rxen, "CAM reload on the running chip");
	check(sim.reg[SN_CE] == 3 && memcmp(sim.cam[1], atmc, 6) == 0,
	    "CAM entry 1 = 09:00:07:ff:ff:ff, CE = 0x0003");
	check(sim_rx(f, mkframe(f, atmc, 1, 60)) == SIM_OK, "enabled multicast accepted");
	f[5] = 0xfe;
	check(sim_rx(f, 60) == SIM_FILTERED, "other multicast filtered");
	check(sim_rx(f, mkframe(f, bcast, 2, 60)) == SIM_OK, "broadcast accepted");
	check(sim_rx(f, mkframe(f, other, 3, 60)) == SIM_FILTERED, "foreign unicast filtered");
	sn_intr(&sc);
	check(nin == 2, "two frames delivered");
}

static void
t_rxwrap(void)
{
	unsigned char f[1536];
	int i, ok = 1, len;

	nin = 0;
	for (i = 0; i < 5 * SN_NRX + 3; i++) {
		len = 60 + (i * 97) % (SN_MAXFRAME - 59);
		if (sim_rx(f, mkframe(f, me, i, len)) != SIM_OK)
			ok = 0;
		if (i % 3 == 2)
			sn_intr(&sc);
	}
	sn_intr(&sc);
	check(ok, "163 frames accepted, serviced every third");
	for (i = 0; i < 5 * SN_NRX + 3; i++)
		if (!frame_ok(i, i, 60 + (i * 97) % (SN_MAXFRAME - 59)))
			ok = 0;
	check(ok && nin == 5 * SN_NRX + 3, "all delivered in order, ring wrapped 5 times");
	check(sim_rx(f, mkframe(f, me, 7, SN_MAXFRAME)) == SIM_OK && sn_intr(&sc) &&
	    frame_ok(nin - 1, 7, SN_MAXFRAME), "1514-byte frame");
	check(sim.errors == 0, "chip never wrote a host-owned descriptor");
}

static void
t_exhaust(void)
{
	unsigned char f[1536];
	int i, got = 0, r, ok = 1;

	nin = 0;
	for (i = 0; i < 2 * SN_NRX; i++) {
		r = sim_rx(f, mkframe(f, me, i, 200));
		if (r == SIM_OK)
			got++;
		else if (r != SIM_NORBA)
			ok = 0;
	}
	check(ok && got == SN_NRX - 1, "without service: 31 RBAs filled, rest missed");
	check((sim.reg[SN_ISR] & INT_RBE) && sim.rbe_stall, "RBE raised, chip stalled");
	sn_intr(&sc);
	check(nin == SN_NRX - 1, "all 31 held frames delivered after service");
	for (i = 0; i < nin; i++)
		if (!frame_ok(i, i, 200))
			ok = 0;
	check(ok, "held frames intact");
	check(!sim.rbe_stall && !(sim.reg[SN_ISR] & INT_RBE), "RBE cleared, chip resumed");
	r = sim_rx(f, mkframe(f, me, 500, 100));
	sn_intr(&sc);
	check(r == SIM_OK && frame_ok(nin - 1, 500, 100), "reception continues");
	check(sc.st.rbe >= 1, "rbe counted");
	for (i = 0; i < 3 * SN_NRX; i++) {
		sim_rx(f, mkframe(f, me, i, 64));
		sn_intr(&sc);
	}
	check(sim.errors == 0 && sc.st.rxbad == 0, "ring consistent afterwards");
}

static void
t_toobig(void)
{
	unsigned char f[1600];
	int n0 = nin, r;

	memset(f, 0, sizeof f);
	memcpy(f, me, 6);
	r = sim_rx(f, 1560);
	sn_intr(&sc);
	check(r == SIM_TOOBIG && sc.st.rbae == 1, "oversized frame dropped (RBA exceeded)");
	r = sim_rx(f, mkframe(f, me, 9, 90));
	sn_intr(&sc);
	check(r == SIM_OK && nin == n0 + 1 && frame_ok(nin - 1, 9, 90),
	    "next frame fine");
}

static int
send(int n, int len)
{
	unsigned char *b;

	if (!sn_txroom(&sc))
		return 0;
	b = sn_txbuf(&sc);
	mkframe(b, other, n, len);
	sn_txstart(&sc, len);
	return 1;
}

static int
wire_ok(int k, int n, int len)
{
	unsigned char f[1536];
	int l = len < 60 ? 60 : len;

	memset(f, 0, sizeof f);
	mkframe(f, other, n, len);
	return sim.wirelen[k] == l && memcmp(sim.wire[k], f, l) == 0;
}

static void
t_tx(void)
{
	int i, n = 0, ok = 1, sent;

	sim.nwire = 0;
	for (i = 0; send(n, 42 + n); i++)
		n++;
	check(n == SN_NTX - 1, "7 frames queued, then the ring is full");
	for (i = 0; i < 3; i++)
		sim_txstep();
	sn_intr(&sc);
	check(sc.txcnt == 4 && ntxdone == 1, "3 completions reclaimed");
	/* append while the chip is mid-list, then drain completely */
	while (send(n, 42 + n))
		n++;
	while (sim_txstep())
		;
	sn_intr(&sc);
	check(sc.txcnt == 0 && sim.nwire == n, "all frames out, ring empty");
	/* chip idle at end-of-list: a new frame restarts it */
	for (i = 0; i < 25; i++) {
		send(n, 100 + i);
		n++;
		while (sim_txstep())
			;
		sn_intr(&sc);
	}
	for (i = 0; i < n; i++)
		if (!wire_ok(i, i, i < 10 ? 42 + i : (i < n - 25 ? 42 + i : 100 + i - (n - 25))))
			ok = 0;
	check(ok && sim.nwire == n, "every frame exactly once, in order, short ones padded to 60");
	check(sc.st.opackets == (unsigned long)n && sc.st.collisions == (unsigned long)n,
	    "opackets and collision tally");

	/* a TXP write lost to the EOL race: interrupt path or watchdog kicks */
	sent = sim.nwire;
	sim.lose_txp = 1;
	send(n++, 80);
	while (sim_txstep())
		;
	check(sim.nwire == sent, "lost TXP: frame stuck");
	sn_watch(&sc);
	sn_watch(&sc);
	while (sim_txstep())
		;
	sn_intr(&sc);
	check(sim.nwire == sent + 1 && sc.txcnt == 0 && sc.st.txkick >= 1,
	    "watchdog kick sends it");

	/* a transmit error stops the chip; reclaim restarts the rest */
	sim.tx_fail = 1;
	send(n++, 70);
	send(n++, 71);
	send(n++, 72);
	sim_txstep();		/* fails, transmitter stops */
	sn_intr(&sc);
	while (sim_txstep())
		;
	sn_intr(&sc);
	check(sc.txcnt == 0 && sc.st.txfu == 1 && sc.st.oerrors == 1 &&
	    sim.nwire == sent + 4, "FIFO underrun counted, remaining frames sent");
	check(sim.errors == 0, "no malformed descriptors");
}

static void
t_reinit(void)
{
	unsigned char f[200];

	sim.rxen = 0;		/* the chip dropped its receiver */
	check(sn_watch(&sc) == 1 && sc.running && sim.rxen, "watchdog resets a stopped receiver");
	nin = 0;
	sim_rx(f, mkframe(f, me, 3, 150));
	sn_intr(&sc);
	check(nin == 1 && frame_ok(0, 3, 150), "receive works after reset");
	check(send(1, 60) && sim_txstep() && sn_intr(&sc) && sc.txcnt == 0,
	    "transmit works after reset");
}

int
main(void)
{
	t_init();
	t_cam();
	t_rxwrap();
	t_exhaust();
	t_toobig();
	t_tx();
	t_reinit();
	printf(bad ? "FAILED\n" : "all tests passed\n");
	return bad;
}
