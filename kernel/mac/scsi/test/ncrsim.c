/*
 * ncrsim.c -- host test of the 53C96 state machine in ncr96.c.
 *
 * ncr96.c is compiled with -DNCR_HOST, so its chip, DMA port and VIA
 * accesses land here.  A simulated 53C96 and SCSI disks at IDs 0-6 run
 * against it in simulated time; each scenario runs in its own process.
 *
 *   ncrsim            run every scenario
 *   ncrsim -v NAME    run one, printing the driver's messages
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>
#include "ncrhost.h"
#include "ncr96reg.h"
#undef printf

#define NBLK	32
#define BSZ	512
#define GLUE_US	20		/* stalled port cycle -> bus error */
#define TICK_US	16667

/* the driver */
extern void	ncr96init(), ncr96queue(), ncr96intr();
extern int	ncr_intrmode, ncr_pdmamode, ncr_disc, ncr_debug;
extern long	ncr_nintr, ncr_nlost, ncr_nfault, ncr_nnosel, ncr_ndisc, ncr_nresel, ncr_nerr,
		ncr_nnodreq, ncr_nchunk;

long	lbolt;
char	*panicstr;
static long	now;			/* microseconds */
static int	verbose, nerr_sim, nreset;

static void
advance(us)
long us;
{
	now += us;
	lbolt = now / TICK_US;
}

void	delayus(us) long us; { advance(us); }
void	delay(t) long t; { advance(t * TICK_US); }
int	splscsi() { return 0; }
void	splrestore(s) int s; { }

int
sim_printf(const char *fmt, ...)
{
	va_list ap;

	if (strstr(fmt, "resetting the bus"))
		nreset++;
	if (verbose) {
		va_start(ap, fmt);
		printf("    | ");
		vprintf(fmt, ap);
		va_end(ap);
	}
	return 0;
}

static void
simfail(msg)
char *msg;
{
	nerr_sim++;
	printf("    SIM: %s\n", msg);
}

/* ---- callouts ---- */

static struct { void (*fn)(); caddr_t arg; long when; } co[8];

int
timeout(fn, arg, t)
void (*fn)();
caddr_t arg;
long t;
{
	int i;

	for (i = 0; i < 8; i++)
		if (!co[i].fn) {
			co[i].fn = fn;
			co[i].arg = arg;
			co[i].when = lbolt + t;
			return i + 1;
		}
	simfail("callout table full");
	return 0;
}

static void
callouts()
{
	void (*fn)();
	int i;

	for (i = 0; i < 8; i++)
		if (co[i].fn && co[i].when <= lbolt) {
			fn = co[i].fn;
			co[i].fn = 0;
			(*fn)(co[i].arg);
		}
}

/* ---- targets ---- */

#define	E_NONE	0
#define	E_DISC	1
#define	E_DONE	2

struct stgt {
	int	present;
	uchar	disk[NBLK * BSZ];
	/* knobs */
	int	disc_before;		/* disconnect after the command */
	int	disc_at;		/* disconnect at this data offset */
	int	nosdp;			/* ... without SAVE DATA POINTER */
	long	resel_us;
	int	stall_at;		/* stop REQ at this data offset ... */
	long	stall_us;		/* ... for this long */
	int	sdtr;			/* send an SDTR before the data */
	int	extra;			/* data-in bytes beyond the request */
	int	lag;			/* data out: leave this many bytes in the FIFO */
	/* nexus */
	int	active, connected, discpriv, atn, phase;
	uchar	*buf, pat[1024];
	int	len, pos, savedpos, in;
	int	status_done;
	uchar	msgs[8];
	int	nmsg, endflag;
	long	resel_at, stall_until;
	int	rejects, done;
};
static struct stgt T[8];

static void
qmsg(t, b)
struct stgt *t;
int b;
{
	t->msgs[t->nmsg++] = b;
}

/* The target decides its next phase. */
static void
tadvance(t)
struct stgt *t;
{
	if (t->atn) {
		t->phase = P_MSGOUT;
		return;
	}
	if (t->nmsg) {
		t->phase = P_MSGIN;
		return;
	}
	if (t->sdtr) {
		t->sdtr = 0;
		qmsg(t, M_EXTENDED); qmsg(t, 3); qmsg(t, 1); qmsg(t, 25); qmsg(t, 8);
		t->phase = P_MSGIN;
		return;
	}
	if (t->disc_before && t->discpriv) {
		t->disc_before = 0;
		qmsg(t, M_DISCONNECT);
		t->endflag = E_DISC;
		t->phase = P_MSGIN;
		return;
	}
	if (t->pos < t->len) {
		if (t->discpriv && t->disc_at > 0 && t->pos == t->disc_at) {
			t->disc_at = 0;
			if (t->nosdp)
				t->pos = t->savedpos;
			else {
				qmsg(t, M_SAVEDP);
				t->savedpos = t->pos;
			}
			qmsg(t, M_DISCONNECT);
			t->endflag = E_DISC;
			t->phase = P_MSGIN;
			return;
		}
		t->phase = t->in ? P_DATAIN : P_DATAOUT;
		return;
	}
	if (!t->status_done) {
		t->phase = P_STATUS;
		return;
	}
	simfail("target has nothing to do");
	t->phase = P_MSGIN;
}

/* After a data byte: leave the phase at a disconnect point or the end. */
static void
tbyte(t)
struct stgt *t;
{
	if (t->pos == t->len || t->pos == t->disc_at)
		tadvance(t);
}

static void
tplan(t, cdb)
struct stgt *t;
uchar *cdb;
{
	long lba;
	int n, i;

	t->pos = t->savedpos = 0;
	t->status_done = 0;
	t->nmsg = 0;
	t->endflag = E_NONE;
	t->atn = 0;
	t->stall_until = 0;
	switch (cdb[0]) {
	case 0x28:
	case 0x2A:
		lba = (long)cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5];
		n = cdb[7] << 8 | cdb[8];
		if (lba + n > NBLK)
			simfail("LBA out of range");
		t->buf = t->disk + lba * BSZ;
		t->len = n * BSZ;
		t->in = cdb[0] == 0x28;
		break;
	case 0x3C:			/* READ BUFFER: pattern, cdb[7..8] bytes */
		n = (cdb[7] << 8 | cdb[8]) + t->extra;
		for (i = 0; i < n; i++)
			t->pat[i] = i * 3 + 1;
		t->buf = t->pat;
		t->len = n;
		t->in = 1;
		break;
	default:
		t->buf = t->pat;
		t->len = 0;
		t->in = 1;
	}
	tadvance(t);
}

/* ---- chip ---- */

static struct {
	uchar	fifo[16];
	int	nf;
	uint	tcload, tc;
	int	intr, sint, step, tczero;
	int	dma, dmain;		/* chip moving bytes; direction */
	int	port;			/* DMA port open (until the next command) */
	int	selid, ensel, lock, conn;
	long	selto_at;
	uchar	via_ifr, via_ier, via_pcr;
	/* injections */
	int	lose;			/* interrupt edges to drop */
	int	race;			/* next selection loses to a reselection */
	int	early;			/* read TC interrupt before the FIFO drains */
	int	fault_chunk, fault_word, chunkno;
} c;

static void
post(bits)
int bits;
{
	c.intr |= bits;
	if (!c.sint) {
		c.sint = 1;
		if (c.lose > 0)
			c.lose--;
		else
			c.via_ifr |= V2_SCSIIRQ;
	}
}

static void
push(b)
int b;
{
	if (c.nf < 16)
		c.fifo[c.nf++] = b;
	else
		simfail("FIFO overflow");
}

static int
pop()
{
	int b;

	if (c.nf == 0) {
		simfail("FIFO underflow");
		return 0;
	}
	b = c.fifo[0];
	memmove(c.fifo, c.fifo + 1, --c.nf);
	return b;
}

static void
reselect(id)
int id;
{
	struct stgt *t = &T[id];

	c.conn = id;
	t->connected = 1;
	t->pos = t->savedpos;
	t->resel_at = 0;
	c.nf = 0;
	push(0x80 | 1 << id);
	push(M_IDENTIFY);
	c.lock = 1;
	c.ensel = 0;
	c.dma = 0;
	t->phase = P_MSGIN;
	post(I_RESEL | I_FC);
}

static int
stalled(t)
struct stgt *t;
{
	if (t->stall_at < 0 || t->pos != t->stall_at)
		return 0;
	if (!t->stall_until)
		t->stall_until = now + t->stall_us;
	if (now < t->stall_until)
		return 1;
	t->stall_at = -1;
	return 0;
}

/* Let the bus make progress up to now. */
static void
run()
{
	struct stgt *t;
	int i;

	if (c.selto_at && now >= c.selto_at) {
		c.selto_at = 0;
		c.step = 0;
		post(I_DIS);
	}
	if (c.dma && c.conn >= 0) {
		t = &T[c.conn];
		if (c.dmain) {
			while (c.tc > 0 && c.nf < 16 && t->phase == P_DATAIN && !stalled(t)) {
				push(t->buf[t->pos++]);
				c.tc--;
				tbyte(t);
			}
			if (c.tc == 0)
				c.tczero = 1;
			if (t->phase != P_DATAIN || (c.tc == 0 && (c.nf == 0 || c.early))) {
				c.dma = 0;
				post(I_BS);
			}
		} else {
			while (c.nf > (c.tc ? t->lag : 0) && t->phase == P_DATAOUT && !stalled(t)) {
				t->buf[t->pos++] = pop();
				tbyte(t);
			}
			if (t->phase != P_DATAOUT || (c.tc == 0 && c.nf == 0)) {
				c.tczero = c.tc == 0;
				c.dma = 0;
				post(I_BS);
			}
		}
	}
	if (c.conn < 0 && !c.selto_at && c.ensel && !c.sint)
		for (i = 0; i < 8; i++)
			if (T[i].active && !T[i].connected && T[i].resel_at && now >= T[i].resel_at) {
				reselect(i);
				break;
			}
}

static void
cmd(v)
int v;
{
	struct stgt *t;
	int i, id;

	t = c.conn >= 0 ? &T[c.conn] : 0;
	c.port = 0;
	if (v & C_DMA) {
		c.tc = c.tcload ? c.tcload : 0x10000;
		c.tczero = 0;
		if (v != (C_DMA | C_TRANS) || !t || (t->phase != P_DATAIN && t->phase != P_DATAOUT)) {
			simfail("DMA command outside a data phase");
			return;
		}
		c.dma = c.port = 1;
		c.dmain = t->phase == P_DATAIN;
		run();
		return;
	}
	switch (v) {
	case C_NOP:
		break;
	case C_FLUSH:
		c.nf = 0;
		break;
	case C_RSTCHIP:
		c.nf = c.intr = c.sint = c.dma = c.ensel = c.tczero = c.lock = 0;
		break;
	case C_RSTSCSI:
		nreset++;
		for (i = 0; i < 8; i++)
			T[i].active = T[i].connected = 0;
		c.conn = -1;
		c.dma = c.ensel = 0;
		c.selto_at = 0;
		break;
	case C_ENSEL:
		c.ensel = 1;
		break;
	case C_SETATN:
		if (t)
			t->atn = 1;
		break;
	case C_SELATN:
		if (c.sint || c.conn >= 0) {
			simfail("select while busy");
			return;
		}
		if (c.race) {
			for (i = 0; i < 8; i++)
				if (T[i].active && !T[i].connected) {
					c.race = 0;
					reselect(i);
					return;
				}
		}
		id = c.selid;
		if (!T[id].present) {
			c.selto_at = now + 250000;
			return;
		}
		if (c.nf < 7 || (c.fifo[0] & ~M_DISCPRIV) != M_IDENTIFY) {
			simfail("bad selection FIFO");
			return;
		}
		t = &T[id];
		t->discpriv = (c.fifo[0] & M_DISCPRIV) != 0;
		t->active = t->connected = 1;
		t->done = 0;
		c.conn = id;
		tplan(t, c.fifo + 1);
		c.nf = 0;
		c.step = 4;
		post(I_BS | I_FC);
		break;
	case C_TRANS:
		if (!t) {
			simfail("transfer while disconnected");
			return;
		}
		switch (t->phase) {
		case P_MSGOUT:
			t->atn = 0;
			while (c.nf) {
				i = pop();
				if (i == M_ABORT) {
					t->active = t->connected = 0;
					c.conn = -1;
					post(I_DIS);
					return;
				}
				if (i == M_REJECT)
					t->rejects++;
			}
			tadvance(t);
			post(I_BS);
			break;
		case P_MSGIN:
			push(t->msgs[0]);
			memmove(t->msgs, t->msgs + 1, --t->nmsg);
			post(I_FC);
			break;
		case P_DATAIN:
			push(t->buf[t->pos++]);
			tbyte(t);
			post(I_BS);
			break;
		case P_DATAOUT:
			while (c.nf && t->phase == P_DATAOUT) {
				t->buf[t->pos++] = pop();
				tbyte(t);
			}
			post(I_BS);
			break;
		default:
			simfail("transfer in an unexpected phase");
		}
		break;
	case C_ICCS:
		if (!t || t->phase != P_STATUS) {
			simfail("ICCS outside status phase");
			return;
		}
		push(0);
		push(M_CMDCOMPLETE);
		t->status_done = 1;
		t->endflag = E_DONE;
		t->phase = P_MSGIN;
		post(I_FC);
		break;
	case C_MSGOK:
		if (!t) {
			simfail("MSGOK while disconnected");
			return;
		}
		if (t->nmsg == 0 && t->endflag != E_NONE && !t->atn) {
			t->connected = 0;
			c.conn = -1;
			if (t->endflag == E_DISC)
				t->resel_at = now + t->resel_us;
			else {
				t->active = 0;
				t->done = 1;
			}
			t->endflag = E_NONE;
			post(I_DIS);
			return;
		}
		tadvance(t);
		post(I_BS);
		break;
	default:
		simfail("unknown command");
	}
}

int
sim_rd(r)
int r;
{
	int v;

	advance(1);
	run();
	switch (r) {
	case TCL:
		return c.tc & 0xFF;
	case TCM:
		return c.tc >> 8 & 0xFF;
	case FIFO:
		return pop();
	case STAT:
		v = (c.sint ? S_INT : 0) | (c.tczero ? S_TC : 0);
		if (c.conn >= 0)
			v |= T[c.conn].phase;
		return v;
	case INTR:
		v = c.intr;
		c.intr = c.sint = c.lock = 0;
		return v;
	case STEP:
		return c.step;
	case FFLAG:
		return c.nf;
	}
	return 0;
}

void
sim_wr(r, v)
int r, v;
{
	advance(1);
	run();
	v &= 0xFF;
	switch (r) {
	case TCL:
		c.tcload = (c.tcload & 0xFF00) | v;
		break;
	case TCM:
		c.tcload = (c.tcload & 0xFF) | v << 8;
		break;
	case FIFO:
		if (!c.lock)
			push(v);
		break;
	case CMD:
		cmd(v);
		break;
	case SELID:
		c.selid = v & 7;
		break;
	}
}

int
sim_via_rd(r)
int r;
{
	return r == VIA_PCR ? c.via_pcr : r == VIA_IFR ? c.via_ifr : c.via_ier;
}

void
sim_via_wr(r, v)
int r, v;
{
	if (r == VIA_PCR)
		c.via_pcr = v;
	else if (r == VIA_IFR)
		c.via_ifr &= ~v;
	else if (v & 0x80)
		c.via_ier |= v & 0x7F;
	else
		c.via_ier &= ~v;
}

/* Mode 1 port accesses must never stall. */
int
sim_prd()
{
	uchar b[2];
	ushort w;

	advance(1);
	run();
	if (!c.port || !c.dmain || c.nf < 2) {
		simfail("mode-1 port read would stall");
		return 0;
	}
	b[0] = pop();
	b[1] = pop();
	memcpy(&w, b, 2);
	return w;
}

void
sim_pwr(w)
int w;
{
	uchar b[2];
	ushort x = w;

	advance(1);
	run();
	if (!c.dma || c.dmain || c.tc < 2 || c.nf > 14) {
		simfail("mode-1 port write would stall");
		return;
	}
	memcpy(b, &x, 2);
	push(b[0]);
	push(b[1]);
	c.tc -= 2;
	run();
}

/* ---- ncr_blind, as ncr96pdma.s does it ---- */

static int
dreq(w)
int w;
{
	run();
	if (w)
		return c.dma && !c.dmain && c.tc >= 2 && c.nf <= 14;
	return c.port && c.dmain && c.nf >= 2;
}

static int
dreqwait(w)
int w;
{
	long n;

	for (n = 0; n < 1000; n++) {
		if (dreq(w))
			return 1;
		if (c.sint)
			return 0;
		advance(1);
	}
	return 0;
}

int
ncr_blind(buf, w)
uchar *buf;
int w;
{
	int i, n;

	c.chunkno++;
	if (!dreqwait(w))
		return -1;
	for (i = 0; i < 128; i++) {
		if (i == 1 && !dreqwait(w))
			return -1;
		if (c.chunkno == c.fault_chunk && i == c.fault_word) {
			c.fault_chunk = 0;
			advance(GLUE_US);
			return -2;
		}
		for (n = 0; !dreq(w); n++) {
			if (n >= GLUE_US)
				return -2;	/* the glue ends the cycle: bus error */
			advance(1);
		}
		if (w) {
			push(buf[2 * i]);
			push(buf[2 * i + 1]);
			c.tc -= 2;
		} else {
			buf[2 * i] = pop();
			buf[2 * i + 1] = pop();
		}
		run();
	}
	return 256;
}

/* ---- harness ---- */

struct req {
	struct sdcom	sc;
	uchar		buf[NBLK * BSZ];
	int		done, okay, seq;
};
static int	seq;

static void
reqdone(cp)
struct sdcom *cp;
{
	struct req *r = (struct req *)cp;

	r->done = 1;
	r->okay = cp->okay;
	r->seq = ++seq;
}

static struct req *
mkreq(unit, op, lba, n)
int unit, op, lba, n;
{
	struct req *r;
	int i;

	r = calloc(1, sizeof *r);
	r->sc.unit = unit;
	r->sc.card = 0;
	r->sc.reading = op != 0x2A;
	r->sc.cdb[0] = op;
	r->sc.cdb[2] = lba >> 24;
	r->sc.cdb[3] = lba >> 16;
	r->sc.cdb[4] = lba >> 8;
	r->sc.cdb[5] = lba;
	if (op == 0x3C) {
		r->sc.cdb[7] = n >> 8;
		r->sc.cdb[8] = n;
		r->sc.nbyte = n;
	} else {
		r->sc.cdb[7] = n >> 8;
		r->sc.cdb[8] = n;
		r->sc.nbyte = n * BSZ;
	}
	r->sc.addr = (caddr_t)r->buf;
	r->sc.intr = reqdone;
	if (op == 0x2A)
		for (i = 0; i < n * BSZ; i++)
			r->buf[i] = i * 11 + lba + 5;
	return r;
}

static void
setup()
{
	int i, j;

	memset(&c, 0, sizeof c);
	c.conn = -1;
	ncr_debug = getenv("NCRDEBUG") != 0;
	for (i = 0; i < 8; i++) {
		T[i].present = i < 3;
		T[i].stall_at = -1;
		T[i].resel_us = 2000;
		for (j = 0; j < NBLK * BSZ; j++)
			T[i].disk[j] = j * 7 + i * 13 + (j >> 9);
	}
}

/* Run interrupts and callouts until the requests are done. */
static int
pump(rs, n, maxus)
struct req **rs;
int n;
long maxus;
{
	long end = now + maxus;
	int i;

	for (;;) {
		for (i = 0; i < n && rs[i]->done; i++)
			;
		if (i == n)
			return 1;
		if (now >= end)
			return 0;
		if ((c.via_ifr & c.via_ier & V2_SCSIIRQ)) {
			c.via_ifr &= ~V2_SCSIIRQ;	/* p2int acknowledges */
			ncr96intr();
			continue;
		}
		callouts();
		advance(20);
		run();
	}
}

static int	bad;

static void
check(ok, what)
int ok;
char *what;
{
	if (!ok) {
		bad++;
		printf("    FAIL: %s\n", what);
	}
}

static void
checkread(r, id, lba)
struct req *r;
int id, lba;
{
	check(r->done && r->okay, "read completed");
	check(memcmp(r->buf, T[id].disk + lba * BSZ, r->sc.nbyte) == 0, "read data");
}

static void
checkwrite(r, id, lba)
struct req *r;
int id, lba;
{
	check(r->done && r->okay, "write completed");
	check(memcmp(r->buf, T[id].disk + lba * BSZ, r->sc.nbyte) == 0, "written data");
}

/* Interrupt mode with the watchdog already run once. */
static void
warmup()
{
	ncr96init(0);
	advance(1100000);
	callouts();
}

static void
rw(unit, lba, n)
int unit, lba, n;
{
	struct req *r, *w;

	w = mkreq(unit, 0x2A, lba, n);
	ncr96queue(&w->sc);
	check(pump(&w, 1, 5000000), "write finishes");
	checkwrite(w, unit, lba);
	r = mkreq(unit, 0x28, lba, n);
	ncr96queue(&r->sc);
	check(pump(&r, 1, 5000000), "read finishes");
	checkread(r, unit, lba);
}

static void t_intr() { warmup(); rw(0, 3, 8); check(ncr_nchunk == 32, "32 blind chunks"); }
static void t_polled() { ncr_intrmode = 0; warmup(); rw(1, 0, 4); }
static void t_mode1() { ncr_pdmamode = 1; warmup(); rw(0, 1, 5); check(ncr_nchunk == 0, "no blind chunks"); }
static void t_mode0() { ncr_pdmamode = 0; warmup(); rw(2, 7, 1); }
static void t_nodisc() { ncr_disc = 0; T[0].disc_before = 1; warmup(); rw(0, 2, 2); check(ncr_ndisc == 0, "no disconnect without privilege"); }
static void t_early() { c.early = 1; warmup(); rw(0, 0, 3); }

static void
t_discbefore()
{
	T[0].disc_before = 1;
	warmup();
	rw(0, 4, 4);
	check(ncr_ndisc == 1 && ncr_nresel == 1, "one disconnect/reselect");
}

static void
t_discmid()
{
	warmup();
	T[0].disc_at = 1024;
	rw(0, 0, 4);
	T[0].disc_at = 1024;
	rw(0, 8, 4);
	check(ncr_ndisc == 2 && ncr_nresel == 2, "disconnect mid-data, both directions");
	check(ncr_nfault == 0, "chunk-aligned disconnect needs no bus error");
}

static void
t_discchunk()
{
	struct req *r;

	warmup();
	T[1].disc_at = 700;		/* inside a blind chunk: the port stalls */
	T[1].lag = 12;			/* the write leaves bytes in the FIFO */
	rw(1, 0, 3);
	T[1].disc_at = 701;		/* odd: a byte stays in the FIFO */
	r = mkreq(1, 0x28, 0, 3);
	ncr96queue(&r->sc);
	check(pump(&r, 1, 5000000), "second read finishes");
	checkread(r, 1, 0);
	check(ncr_ndisc == 2 && ncr_nfault == 2, "disconnect inside a chunk recovered from a bus error");
}

static void
t_nosdp()
{
	warmup();
	T[0].disc_at = 1536;
	T[0].nosdp = 1;
	rw(0, 2, 4);
	check(ncr_ndisc == 1, "disconnect without SAVE DATA POINTER, pointers restored");
}

static void
t_twotargets()
{
	struct req *a, *b, *rs[2];

	warmup();
	T[0].disc_before = 1;
	T[0].resel_us = 20000;
	a = mkreq(0, 0x28, 0, 8);
	b = mkreq(1, 0x2A, 5, 8);
	ncr96queue(&a->sc);
	ncr96queue(&b->sc);
	rs[0] = a;
	rs[1] = b;
	check(pump(rs, 2, 5000000), "both finish");
	checkread(a, 0, 0);
	checkwrite(b, 1, 5);
	check(b->seq < a->seq, "target 1 ran while target 0 was disconnected");
}

static void
t_interleave()
{
	struct req *rs[3];

	warmup();
	T[0].disc_at = 512;
	T[1].disc_at = 1024;
	T[2].disc_before = 1;
	rs[0] = mkreq(0, 0x28, 0, 4);
	rs[1] = mkreq(1, 0x28, 4, 4);
	rs[2] = mkreq(2, 0x2A, 8, 4);
	ncr96queue(&rs[0]->sc);
	ncr96queue(&rs[1]->sc);
	ncr96queue(&rs[2]->sc);
	check(pump(rs, 3, 5000000), "three finish");
	checkread(rs[0], 0, 0);
	checkread(rs[1], 1, 4);
	checkwrite(rs[2], 2, 8);
	check(ncr_ndisc == 3 && ncr_nresel == 3, "three disconnects");
}

static void
t_race()
{
	struct req *rs[2];

	warmup();
	T[0].disc_before = 1;
	T[0].resel_us = 1000000;
	rs[0] = mkreq(0, 0x28, 0, 2);
	ncr96queue(&rs[0]->sc);
	pump(rs, 1, 5000);
	c.race = 1;			/* the next selection loses */
	rs[1] = mkreq(1, 0x28, 0, 2);
	ncr96queue(&rs[1]->sc);
	check(pump(rs, 2, 5000000), "both finish");
	checkread(rs[0], 0, 0);
	checkread(rs[1], 1, 0);
	check(ncr_nerr == 0, "no bus reset");
	check(c.race == 0, "our selection lost to the reselection");
}

static void
t_selto()
{
	struct req *r;

	warmup();
	r = mkreq(4, 0x28, 0, 1);
	ncr96queue(&r->sc);
	check(pump(&r, 1, 1000000), "finishes");
	check(r->done && !r->okay, "fails");
	check(ncr_nnosel == 1 && ncr_nerr == 0, "one selection timeout, no bus reset");
	rw(0, 0, 1);
}

static void
t_fault()
{
	warmup();
	c.fault_chunk = 2;
	c.fault_word = 37;
	rw(0, 0, 2);			/* write: chunks 1-4, fault in 2 */
	c.fault_chunk = c.chunkno + 3;
	c.fault_word = 90;
	{
		struct req *r = mkreq(0, 0x28, 0, 2);

		ncr96queue(&r->sc);
		check(pump(&r, 1, 5000000), "read finishes");
		checkread(r, 0, 0);
	}
	check(ncr_nfault == 2, "two bus errors, recovered");
}

static void
t_stall()
{
	warmup();
	T[2].stall_at = 300;
	T[2].stall_us = 3000;
	rw(2, 0, 2);
	T[2].stall_at = 300;
	T[2].stall_until = 0;
	{
		struct req *r = mkreq(2, 0x28, 0, 2);

		ncr96queue(&r->sc);
		check(pump(&r, 1, 5000000), "read finishes");
		checkread(r, 2, 0);
	}
	check(ncr_nfault == 2, "slow target: bus error, rest by FIFO count");
}

static void
t_lost()
{
	struct req *r;

	warmup();
	c.via_ifr = 0;			/* no stale edge latched */
	c.lose = 1;			/* the selection interrupt's edge */
	r = mkreq(0, 0x28, 0, 2);
	ncr96queue(&r->sc);
	check(pump(&r, 1, 3000000), "finishes");
	checkread(r, 0, 0);
	check(ncr_nlost == 1, "watchdog found the lost interrupt");
	c.via_ifr = 0;
	c.lose = 3;
	T[1].disc_before = 1;
	rw(1, 1, 2);
	check(ncr_nlost >= 2, "lost interrupts while disconnected");
}

static void
t_auto()
{
	struct req *r;

	ncr96init(0);
	r = mkreq(0, 0x28, 0, 2);
	T[0].disc_before = 1;
	ncr96queue(&r->sc);
	check(r->done, "cold: polled to completion inside sdqueue");
	checkread(r, 0, 0);
	advance(1100000);
	callouts();
	r = mkreq(0, 0x28, 1, 2);
	ncr96queue(&r->sc);
	check(!r->done, "warm: interrupt driven");
	check(pump(&r, 1, 5000000), "finishes");
	checkread(r, 0, 1);
	panicstr = "test";
	r = mkreq(1, 0x2A, 3, 2);
	ncr96queue(&r->sc);
	check(r->done, "panic: polled");
	checkwrite(r, 1, 3);
}

static void
t_odd()
{
	struct req *r, *rs[1];
	int i;

	warmup();
	r = mkreq(0, 0x3C, 0, 300);	/* 256 blind + 44 by PIO */
	ncr96queue(&r->sc);
	rs[0] = r;
	check(pump(rs, 1, 5000000), "finishes");
	for (i = 0; i < 300 && r->buf[i] == (uchar)(i * 3 + 1); i++)
		;
	check(r->okay && i == 300, "300-byte read");
	T[0].extra = 7;
	r = mkreq(0, 0x3C, 0, 21);	/* overrun: 7 bytes discarded */
	ncr96queue(&r->sc);
	rs[0] = r;
	check(pump(rs, 1, 5000000), "overrun finishes");
	for (i = 0; i < 21 && r->buf[i] == (uchar)(i * 3 + 1); i++)
		;
	check(r->okay && i == 21 && r->buf[21] == 0, "overrun drained");
}

static void
t_sdtr()
{
	warmup();
	T[0].sdtr = 1;
	rw(0, 0, 1);
	check(T[0].rejects == 1, "SDTR rejected");
}

static struct { char *name; void (*fn)(); char *what; } tests[] = {
	{ "intr",	t_intr,		"interrupt mode, blind chunks, write + read" },
	{ "polled",	t_polled,	"forced polled mode" },
	{ "mode1",	t_mode1,	"FIFO-count pseudo-DMA" },
	{ "mode0",	t_mode0,	"byte PIO" },
	{ "early",	t_early,	"TC interrupt before the FIFO drains" },
	{ "odd",	t_odd,		"remainder by PIO, read overrun" },
	{ "nodisc",	t_nodisc,	"IDENTIFY without disconnect privilege" },
	{ "discbefore",	t_discbefore,	"disconnect after the command, reselect" },
	{ "discmid",	t_discmid,	"SAVE DATA POINTER + DISCONNECT mid-data" },
	{ "discchunk",	t_discchunk,	"disconnect inside a blind chunk" },
	{ "nosdp",	t_nosdp,	"disconnect without SAVE DATA POINTER" },
	{ "twotargets",	t_twotargets,	"second target selected while the first is away" },
	{ "interleave",	t_interleave,	"three targets interleaved" },
	{ "race",	t_race,		"reselection wins over our selection" },
	{ "selto",	t_selto,	"selection timeout" },
	{ "fault",	t_fault,	"bus error mid-chunk, read and write" },
	{ "stall",	t_stall,	"target stall beyond the glue timeout" },
	{ "lost",	t_lost,		"lost interrupts, watchdog" },
	{ "auto",	t_auto,		"cold polled, warm interrupts, panic polled" },
	{ "sdtr",	t_sdtr,		"target SDTR rejected" },
};

static int
one(i)
int i;
{
	setup();
	(*tests[i].fn)();
	if (nerr_sim)
		bad++;
	if (verbose)
		printf("    counters: nintr %ld lost %ld fault %ld nodreq %ld chunk %ld disc %ld resel %ld nosel %ld err %ld\n",
		    ncr_nintr, ncr_nlost, ncr_nfault, ncr_nnodreq, ncr_nchunk, ncr_ndisc, ncr_nresel,
		    ncr_nnosel, ncr_nerr);
	check(ncr_nerr == 0, "no bus reset");
	return bad != 0;
}

int
main(argc, argv)
int argc;
char **argv;
{
	int i, st, nfail = 0, n = 0;
	pid_t p;

	if (argc > 1 && strcmp(argv[1], "-v") == 0) {
		verbose = 1;
		argc--, argv++;
	}
	for (i = 0; i < (int)(sizeof tests / sizeof tests[0]); i++) {
		if (argc > 1 && strcmp(argv[1], tests[i].name) != 0)
			continue;
		fflush(stdout);
		p = fork();
		if (p == 0)
			exit(one(i));
		waitpid(p, &st, 0);
		n++;
		if (!WIFEXITED(st) || WEXITSTATUS(st)) {
			nfail++;
			printf("FAIL %-11s %s\n", tests[i].name, tests[i].what);
		} else
			printf("OK   %-11s %s\n", tests[i].name, tests[i].what);
	}
	printf("%d scenarios, %d failed\n", n, nfail);
	return nfail != 0;
}
