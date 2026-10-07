/*
 * Host test: the DaynaPORT core and the 5380 command path against a
 * model of the 5380, the SCSI bus and its targets (a disk and a
 * DaynaPORT that behaves like the emulations, or like the ROM).
 *
 *   cc -DFS_HOST -I.. -I../../scsi -o tdp tdp.c ../dpchip.c ../../scsi/scsi.c ../../scsi/falcon.c && ./tdp
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "dp.h"
#include "ncr5380.h"

static int fails, checks;
#define CHECK(c, m)	do { checks++; if (!(c)) { fails++; \
			printf("FAIL %s (line %d)\n", m, __LINE__); } } while (0)

long sim_ticks;
static long usec;		/* simulated time */
static long naccess;		/* register accesses */

void
sim_us(int n)
{
	usec += n;
	sim_ticks = usec / 16667;
}

void
sim_pause()
{
	sim_us(16667);
}

/* ---------------------------------------------------------- targets */

#define NONE	0
#define DISK	1
#define DAYNA	2

#define QMAX	32

static struct tgt {
	int type;
	/* DaynaPORT */
	int enabled, rom, multi, wedge_after, wedged, stall_at;
	long enabled_at;		/* usec of the last ENABLE */
	unsigned char mac[6];
	unsigned char q[QMAX][1600];
	int qlen[QMAX], qh, qt;
	unsigned char out[QMAX][1600];
	int outlen[QMAX], nout;
	unsigned char mc[8][6];
	int nmc, nenable, ndisable, nmode, nreads, nsense, rxcount;
	int sensekey, strict09, lastctl;
} tg[8];

/* the connection */
static struct {
	int id;			/* selected target, -1 bus free */
	int phase, req, bsy;
	unsigned char buf[4000];
	int len, idx;		/* bytes of this phase */
	int dir;		/* 1: target to initiator */
	unsigned char cdb[6];
	int ncdb;
	int status, stall;	/* stall: no more REQ */
	int doutop;		/* command awaiting data out */
	unsigned char data[4000];
	int datan;
	int identify, atnbad;
} b;

static int icr, mr, tcr, odr, aip, nochip, resets, tcrbad, busbusy;

static void
setphase(int ph, int dir, int len)
{
	b.phase = ph;
	b.dir = dir;
	b.len = len;
	b.idx = 0;
	b.req = 1;
}

static void
endcmd(int status)
{
	b.status = status;
	b.buf[0] = status;
	setphase(PH_STAT, 1, 1);
}

static void
reply(unsigned char *p, int n, int alloc)
{
	if (n > alloc)
		n = alloc;
	memcpy(b.buf, p, n);
	if (n == 0) {
		endcmd(0);
		return;
	}
	setphase(PH_DIN, 1, n);
}

static void
check(struct tgt *t, int key)
{
	t->sensekey = key;
	endcmd(2);
}

/* Command received: decide the next phase, as the emulations do. */
static void
exec(void)
{
	struct tgt *t = &tg[b.id];
	unsigned char *c = b.cdb, r[3100];
	int len = c[3] << 8 | c[4], i, n, k, total, more;

	if (c[0] == 0x12) {
		memset(r, ' ', 36);
		r[0] = t->type == DAYNA ? 3 : 0;
		r[1] = 0;
		r[2] = 2;
		r[4] = 31;
		if (t->type == DAYNA) {
			memcpy(r + 8, "Dayna", 5);
			memcpy(r + 16, "SCSI/Link", 9);
			memcpy(r + 32, "2.0f", 4);
		} else {
			memcpy(r + 8, "BLUESCSI", 8);
			memcpy(r + 16, "HARDDRIVE", 9);
		}
		reply(r, 36, c[4]);
		return;
	}
	if (c[0] == 0x03) {
		memset(r, 0, 18);
		r[0] = 0x70;
		r[2] = t->sensekey;
		t->sensekey = 0;
		t->nsense++;
		reply(r, t->rom ? 9 : 18, c[4]);
		return;
	}
	if (t->type != DAYNA) {
		check(t, 5);
		return;
	}
	/* the ROM refuses data commands while disabled and while settling */
	if (t->rom && c[0] != 0x0e && (!t->enabled || usec - t->enabled_at < 500000)) {
		check(t, 2);
		return;
	}
	switch (c[0]) {
	case 0x0e:
		if (c[5] & 0x80) {
			t->enabled = 1;
			t->enabled_at = usec;
			t->nenable++;
			t->qh = t->qt = 0;
			t->wedged = 0;
		} else {
			t->enabled = 0;
			t->ndisable++;
		}
		endcmd(0);
		return;
	case 0x09:
		if (t->strict09 && len != 18) {
			check(t, 5);
			return;
		}
		memset(r, 0, 22);
		memcpy(r, t->mac, 6);
		reply(r, t->rom ? 22 : 18, len);
		return;
	case 0x0c:
		t->nmode++;
		endcmd(0);
		return;
	case 0x0a:
	case 0x0d:
		b.doutop = c[0];
		b.datan = 0;
		setphase(PH_DOUT, 0, len);
		return;
	case 0x08:
		t->nreads++;
		t->lastctl = c[5];
		if (len == 1) {
			check(t, 5);
			return;
		}
		if (t->wedged) {
			r[0] = 0x0f;
			r[1] = 0xa0;
			r[2] = r[3] = r[4] = r[5] = 0xff;
			reply(r, 6, len);
			return;
		}
		if (t->qh == t->qt) {
			memset(r, 0, 6);
			reply(r, 6, len);
			return;
		}
		for (total = 0; t->qh != t->qt; ) {
			n = t->qlen[t->qh];
			if (total + 6 + n > len)
				break;
			more = t->multi && (c[5] & 0x40) && (t->qh + 1) % QMAX != t->qt &&
			    total + 6 + n + 6 + t->qlen[(t->qh + 1) % QMAX] <= len;
			r[total] = n >> 8;
			r[total + 1] = n;
			r[total + 2] = r[total + 3] = r[total + 4] = 0;
			r[total + 5] = more ? 0x10 : 0;
			memcpy(r + total + 6, t->q[t->qh], n);
			total += 6 + n;
			t->qh = (t->qh + 1) % QMAX;
			t->rxcount++;
			if (t->wedge_after && t->rxcount == t->wedge_after)
				t->wedged = 1;
			if (!more)
				break;
		}
		reply(r, total, len);
		return;
	}
	k = 0;
	(void)k;
	(void)i;
	check(t, 5);
}

/* Data-out finished. */
static void
dataout(void)
{
	struct tgt *t = &tg[b.id];

	if (b.doutop == 0x0a && t->nout < QMAX) {
		memcpy(t->out[t->nout], b.data, b.datan);
		t->outlen[t->nout++] = b.datan;
	} else if (b.doutop == 0x0d && t->nmc < 8)
		memcpy(t->mc[t->nmc++], b.data, 6);
	endcmd(0);
}

/* A byte of the current phase has been acknowledged. */
static void
advance(void)
{
	if (++b.idx < b.len) {
		b.req = 1;
		return;
	}
	switch (b.phase) {
	case PH_MOUT:
		setphase(PH_CMD, 0, 6);
		b.ncdb = 0;
		break;
	case PH_CMD:
		exec();
		break;
	case PH_DIN:
		endcmd(0);
		break;
	case PH_DOUT:
		dataout();
		break;
	case PH_STAT:
		b.buf[0] = 0;
		setphase(PH_MIN, 1, 1);
		break;
	case PH_MIN:
		b.id = -1;	/* bus free */
		b.bsy = b.req = 0;
		break;
	}
}

static void
ack(void)
{
	int v = odr;

	if ((tcr & 7) != (b.phase >> 2))
		tcrbad++;
	if (b.dir == 0) {
		if (b.phase == PH_MOUT) {
			if (icr & ICR_ATN)
				b.atnbad++;
			b.identify = v;
		} else if (b.phase == PH_CMD)
			b.cdb[b.ncdb++] = v;
		else if (b.phase == PH_DOUT)
			b.data[b.datan++] = v;
	}
	b.req = 0;
	if (b.stall && --b.stall == 0)
		b.stall = -1;
}

static void
busreset(void)
{
	b.id = -1;
	b.bsy = b.req = 0;
	b.stall = 0;
	resets++;
}

unsigned char
sim_rd(int r)
{
	naccess++;
	if (nochip)
		return 0xff;
	switch (r) {
	case NCR_DATA:
		if (b.id >= 0 && b.dir == 1 && b.req)
			return b.buf[b.idx];
		return (icr & ICR_DATA) ? odr : 0;
	case NCR_ICR:
		return icr | (aip ? ICR_AIP : 0);
	case NCR_MR:
		return mr;
	case NCR_TCR:
		return tcr;
	case NCR_CSBR:
		if (busbusy)
			return CSB_BSY;
		if (b.id < 0)
			return (icr & ICR_BSY ? CSB_BSY : 0) | (icr & ICR_SEL ? CSB_SEL : 0);
		return CSB_BSY | (b.req && b.stall >= 0 ? CSB_REQ | b.phase : b.phase);
	default:
		return 0;
	}
}

void
sim_wr(int r, int v)
{
	int old;

	naccess++;
	if (nochip)
		return;
	v &= 0xff;
	switch (r) {
	case NCR_DATA:
		odr = v;
		break;
	case NCR_MR:
		mr = v;
		aip = (v & MR_ARB) && b.id < 0 && !busbusy;
		break;
	case NCR_TCR:
		tcr = v;
		break;
	case NCR_ICR:
		old = icr;
		icr = v & 0x9f;
		if (icr & ICR_RST) {
			busreset();
			break;
		}
		/* selection: SEL and our data bits, BSY released */
		if (b.id < 0 && (icr & ICR_SEL) && !(icr & ICR_BSY) &&
		    (icr & ICR_DATA)) {
			int id;

			for (id = 0; id < 7; id++)
				if ((odr & (1 << id)) && tg[id].type != NONE) {
					b.id = id;
					b.bsy = 1;
					b.req = 0;
					b.phase = (icr & ICR_ATN) ? PH_MOUT : PH_CMD;
					b.dir = 0;
					b.len = b.phase == PH_MOUT ? 1 : 6;
					b.idx = b.ncdb = 0;
					b.identify = -1;
					b.stall = tg[id].stall_at;
					break;
				}
			break;
		}
		if (b.id >= 0 && (old & ICR_SEL) && !(icr & ICR_SEL) && b.idx == 0 &&
		    !b.req && (b.phase == PH_MOUT || b.phase == PH_CMD))
			b.req = 1;
		if (b.id >= 0 && !(old & ICR_ACK) && (icr & ICR_ACK) && b.req &&
		    b.stall >= 0)
			ack();
		else if (b.id >= 0 && (old & ICR_ACK) && !(icr & ICR_ACK) &&
		    b.stall >= 0)
			advance();
		break;
	}
}

/* ------------------------------------------------------------ host */

static unsigned char got[16][1600];
static int gotlen[16], ngot;

void
dp_input(struct dp_softc *sc, unsigned char *f, int len)
{
	if (ngot < 16) {
		memcpy(got[ngot], f, len);
		gotlen[ngot++] = len;
	}
}

static void
queue(struct tgt *t, int len, int seed)
{
	unsigned char *p = t->q[t->qt];
	int i, n = len < 60 ? 60 : len;

	for (i = 0; i < n; i++)
		p[i] = i < len ? (unsigned char)(seed + i) : 0;
	memset(p + n, 0xcc, 4);		/* CRC */
	t->qlen[t->qt] = n + 4;
	t->qt = (t->qt + 1) % QMAX;
}

static void
reset_all(void)
{
	memset(tg, 0, sizeof(tg));
	memset(&b, 0, sizeof(b));
	b.id = -1;
	icr = mr = tcr = odr = aip = nochip = resets = tcrbad = busbusy = 0;
	ngot = 0;
}

static struct dp_softc sc;

static void
bringup(int rom)
{
	reset_all();
	tg[0].type = DISK;
	tg[4].type = DAYNA;
	tg[4].rom = rom;
	memcpy(tg[4].mac, "\x00\x80\x19\xc0\xff\xee", 6);
	memset(&sc, 0, sizeof(sc));
	CHECK(dp_probe(&sc) == 0 && sc.id == 4, "probe finds ID 4");
	CHECK(dp_init(&sc) == 0, "init");
	CHECK(memcmp(sc.ea, tg[4].mac, 6) == 0, "MAC");
	CHECK(tg[4].enabled && tg[4].nenable == 1, "enabled once");
}

static void
settle(void)
{
	sim_us(600000);
	CHECK(dp_ready(&sc) == 1, "ready after settle");
}

int
main(void)
{
	int i, r;
	long t0, a0;

	/* no chip */
	reset_all();
	nochip = 1;
	memset(&sc, 0, sizeof(sc));
	CHECK(dp_probe(&sc) < 0 && sc.id == -1, "no 5380: absent");

	/* empty bus: quick, and nothing selected */
	reset_all();
	t0 = usec;
	CHECK(dp_probe(&sc) < 0, "empty bus: absent");
	CHECK(usec - t0 < 7 * 260000, "empty bus probe under 1.82 s");
	printf("empty-bus probe: %ld us simulated\n", usec - t0);

	/* a disk only */
	reset_all();
	tg[2].type = DISK;
	CHECK(dp_probe(&sc) < 0, "disk only: absent");

	/* emulation */
	bringup(0);
	CHECK(b.identify == 0x80, "IDENTIFY sent");
	CHECK(b.atnbad == 0, "ATN dropped before the message ACK");
	CHECK(memcmp(sc.rev, "2.0f", 4) == 0, "firmware revision");
	CHECK(dp_ready(&sc) == 0, "not ready inside the settle window");
	settle();
	CHECK(tg[4].nmode == 1, "broadcast mode sent once");
	CHECK(dp_ready(&sc) == 1 && tg[4].nmode == 1, "mode not repeated");

	/* transmit */
	for (i = 0; i < 42; i++)
		sc.txbuf[i] = i + 1;
	CHECK(dp_send(&sc, 42) == 0, "send short frame");
	CHECK(tg[4].nout == 1 && tg[4].outlen[0] == 60, "padded to 60");
	CHECK(tg[4].out[0][0] == 1 && tg[4].out[0][41] == 42 &&
	    tg[4].out[0][42] == 0 && tg[4].out[0][59] == 0, "frame bytes and padding");
	for (i = 0; i < 1514; i++)
		sc.txbuf[i] = i * 7;
	a0 = naccess;
	CHECK(dp_send(&sc, 1514) == 0, "send full frame");
	printf("full frame out: %ld register accesses\n", naccess - a0);
	CHECK(naccess - a0 < 1514 * 6 + 200, "data out: 6 accesses a byte");
	CHECK(tg[4].outlen[1] == 1514 && tg[4].out[1][1513] == (unsigned char)(1513 * 7),
	    "full frame intact");

	/* receive */
	a0 = naccess;
	t0 = usec;
	CHECK(dp_recv(&sc) == 0, "empty read");
	printf("empty poll: %ld register accesses\n", naccess - a0);
	queue(&tg[4], 42, 10);
	queue(&tg[4], 1514, 20);
	queue(&tg[4], 100, 30);
	CHECK(dp_recv(&sc) == 1, "one frame per single-packet read");
	a0 = naccess;
	CHECK(dp_recv(&sc) == 1, "one frame per single-packet read");
	printf("full frame in: %ld register accesses\n", naccess - a0);
	CHECK(naccess - a0 < 1518 * 5 + 200, "data in: 5 accesses a byte");
	CHECK(dp_recv(&sc) == 1, "one frame per single-packet read");
	CHECK(dp_recv(&sc) == 0, "drained");
	CHECK(sc.st.rxbad == 0, "empty reads are not bad frames");
	CHECK(ngot == 3 && gotlen[0] == 60 && gotlen[1] == 1514 && gotlen[2] == 100,
	    "lengths without CRC");
	CHECK(got[0][0] == 10 && got[0][41] == 51 && got[1][1513] == (unsigned char)(20 + 1513) &&
	    got[2][99] == 129, "frame contents");
	CHECK(tcrbad == 0, "TCR matched the phase on every ACK");

	/* multi-packet transfer */
	tg[4].multi = 1;
	ngot = 0;
	queue(&tg[4], 60, 1);
	queue(&tg[4], 70, 2);
	i = tg[4].nreads;
	CHECK(dp_recv(&sc) == 2 && ngot == 2 && gotlen[1] == 70, "two frames in one transfer");
	CHECK(tg[4].nreads == i + 1 && tg[4].lastctl == 0xc0, "one multi-packet READ");
	tg[4].multi = 0;
	ngot = 0;
	queue(&tg[4], 60, 1);
	queue(&tg[4], 70, 2);
	CHECK(dp_recv(&sc) == 1 && dp_recv(&sc) == 1 && ngot == 2,
	    "an adapter that ignores the multi-packet bit still works");
	tg[4].multi = 1;

	/* multicast */
	CHECK(dp_addmc(&sc, (unsigned char *)"\x01\x00\x5e\x00\x00\x01") == 0, "addmc");
	CHECK(dp_addmc(&sc, (unsigned char *)"\x01\x00\x5e\x00\x00\x01") == 0, "addmc again");
	CHECK(dp_ready(&sc) == 1 && tg[4].nmc == 1 &&
	    memcmp(tg[4].mc[0], "\x01\x00\x5e\x00\x00\x01", 6) == 0, "multicast sent once");

	/* wedge: re-enable, then frames again */
	tg[4].wedge_after = tg[4].rxcount + 1;
	ngot = 0;
	queue(&tg[4], 60, 5);
	CHECK(dp_recv(&sc) == 1, "frame before the wedge");
	queue(&tg[4], 60, 6);
	CHECK(dp_recv(&sc) == 0 && sc.reinit && sc.st.wedged == 1, "wedge seen");
	CHECK(dp_ready(&sc) == 0 && tg[4].ndisable == 1 && tg[4].nenable == 2,
	    "disable and enable");
	settle();
	CHECK(tg[4].nmc == 2, "multicast resent after re-enable");
	queue(&tg[4], 80, 7);
	CHECK(dp_recv(&sc) == 1 && gotlen[1] == 80, "frames after the wedge");

	/* target stalls in the middle of a data-in phase */
	queue(&tg[4], 300, 8);
	tg[4].stall_at = 100;
	r = dp_recv(&sc);
	CHECK(r < 0 && sc.st.busreset == 1 && resets == 1 && sc.reinit, "stall: bus reset");
	tg[4].stall_at = 0;
	CHECK(dp_ready(&sc) == 0, "re-enable after the reset");
	settle();
	queue(&tg[4], 90, 9);
	CHECK(dp_recv(&sc) == 1, "recovered after the stall");

	/* three failed commands in a row re-enable */
	tg[4].enabled = 0;
	tg[4].rom = 1;
	for (i = 0; i < 3; i++)
		(void)dp_recv(&sc);
	CHECK(sc.reinit, "three failures: re-enable");
	tg[4].rom = 0;

	/* bus held busy: no hang */
	busbusy = 1;
	t0 = usec;
	CHECK(dp_send(&sc, 60) < 0, "busy bus: send fails");
	CHECK(usec - t0 < 100000, "busy bus: bounded wait");
	busbusy = 0;

	/* a failed re-enable is retried, once the settle window has passed */
	sim_us(600000);
	sc.reinit = 1;
	sc.errs = 0;
	busbusy = 1;
	i = tg[4].nenable;
	CHECK(dp_ready(&sc) == 0 && sc.reinit && tg[4].nenable == i,
	    "failed re-enable stays due");
	busbusy = 0;
	CHECK(dp_ready(&sc) == 0 && tg[4].nenable == i, "no retry inside the settle window");
	sim_us(600000);
	CHECK(dp_ready(&sc) == 0 && tg[4].nenable == i + 1 && !sc.reinit,
	    "re-enable retried");
	settle();

	/* the ROM: refuses until enabled and settled; 22-byte stats */
	bringup(1);
	CHECK(tg[4].nsense > 0, "ROM: refusals fetched sense");
	CHECK(sc.st.reinit == 0 && !sc.reinit, "ROM: settle refusals do not re-enable");
	settle();
	queue(&tg[4], 64, 3);
	ngot = 0;
	CHECK(dp_recv(&sc) == 1 && gotlen[0] == 64, "ROM: receive");

	/* an emulation that refuses a 22-byte address read */
	bringup(0);
	dp_stop(&sc);
	tg[4].strict09 = 1;
	CHECK(dp_init(&sc) == 0 && memcmp(sc.ea, tg[4].mac, 6) == 0,
	    "address read falls back to 18 bytes");

	/* stop */
	dp_stop(&sc);
	CHECK(!tg[4].enabled && !sc.running, "stop disables");

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
