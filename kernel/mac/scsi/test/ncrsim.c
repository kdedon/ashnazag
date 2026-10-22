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

#define NBLK	256
#define BSZ	512
#define GLUE_US	20		/* stalled port cycle -> bus error */
#define TICK_US	16667

/* the driver */
extern void	ncr96init(), ncr96queue(), ncr96intr();
extern int	ncr_intrmode, ncr_pdmamode, ncr_disc, ncr_debug;
extern long	ncr_nintr, ncr_nlost, ncr_nfault, ncr_nnosel, ncr_ndisc, ncr_nresel, ncr_nerr,
		ncr_nnodreq, ncr_nchunk;
extern long	ncr_xcmd, ncr_xbytes, ncr_xphase, ncr_xresume, ncr_xfall;
extern long	ncr_nnest, ncr_ipl2max;
extern int	ncr_xipl;

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
int	sim_sr = 0x2200;		/* SCSI interrupt level, as in the handler */
int	sim_t1hi() { return (13056 - (now * 1000 / 1277) % 13056) >> 8; }
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
	int	oob;			/* past the last block: CHECK CONDITION */
	int	chk;
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
		t->chk = 0;
		if (lba + n > NBLK && t->oob) {
			t->chk = 1;
			t->buf = t->pat;
			t->len = 0;
			t->in = 1;
			break;
		}
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
	int	nodreq;			/* DREQ never comes */
	int	nest_word;	/* ncr_xfer: take a nested interrupt at this word */
	int	xfer_sr;	/* SR seen inside the last ncr_xfer */
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
		push(t->chk ? 2 : 0);
		t->chk = 0;
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
	if (c.nodreq)
		return 0;
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

/*
 * ncr_xfer: whole transfer command.  A write's fault is reported two
 * words late, as the 040 does after buffered stores.
 */
int
ncr_xfer(pp, n, w)
uchar **pp;
uint n;
int w;
{
	uchar *buf = *pp;
	int i, k;

	c.chunkno++;
	c.xfer_sr = sim_sr;
	if (!dreqwait(w))
		return -1;
	for (i = 0; i < (int)n / 2; i++) {
		if (c.nest_word && i == c.nest_word) {
			c.nest_word = 0;
			ncr96intr();
		}
		if (c.chunkno == c.fault_chunk && i == c.fault_word) {
			c.fault_chunk = 0;
			advance(GLUE_US);
			goto fault;
		}
		for (k = 0; !dreq(w); k++) {
			if (k >= GLUE_US)
				goto fault;
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
	*pp = buf + n;
	return 0;
fault:
	*pp = buf + 2 * i + (w ? 4 : 0);
	return -2;
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

static void
xcount(cmds, phases, bytes)
long cmds, phases, bytes;
{
	check(ncr_xcmd == cmds, "transfer commands");
	check(ncr_xphase == phases, "data phases");
	check(ncr_xbytes == bytes, "bytes counted");
	check(ncr_nchunk == 0, "no blind chunks");
	if (verbose)
		printf("    x: cmd %ld phase %ld bytes %ld resume %ld fall %ld\n", ncr_xcmd,
		    ncr_xphase, ncr_xbytes, ncr_xresume, ncr_xfall);
}

static void
rd(unit, lba, n)
int unit, lba, n;
{
	struct req *r = mkreq(unit, 0x28, lba, n);

	ncr96queue(&r->sc);
	check(pump(&r, 1, 50000000), "read finishes");
	checkread(r, unit, lba);
}

/* ---- request shaping (sdclust.c) ---- */

#include "sdchost.h"

extern int	sd_clust, sd_ra, sd_bigraw;
extern long	sd_ncmd, sd_nmerge, sd_nra, sd_nhit, sd_nsplit, sd_nraw, sd_nbounce;
extern void	sdc_attach();
int		(*sd_strathook)(), (*sd_rawhook)();
static long	devsize = NBLK;
static caddr_t	woke;
static int	sim_proc, nstock;

static void
step()
{
	if ((c.via_ifr & c.via_ier & V2_SCSIIRQ)) {
		c.via_ifr &= ~V2_SCSIIRQ;
		ncr96intr();
		return;
	}
	callouts();
	advance(20);
	run();
}

bool	sdvalid(bp) struct buf *bp; { return 1; }
uint	sdblkno(bp) struct buf *bp; { return bp->b_blkno; }
uint	sddevsize(d) dev_t d; { return devsize; }
void	iodone(bp) struct buf *bp; { bp->b_flags |= B_DONE; }
void	sdqueue(cp) struct sdcom *cp; { ncr96queue(cp); }
void	bcopy(a, b, n) char *a, *b; uint n; { memmove(b, a, n); }
void	dlm_cachepush(a, n) caddr_t a; uint n; { }
char	*sdhardwarename(c) uint c; { return "sim"; }
char	*kmem_alloc(n, f) uint n; int f; { return malloc(n); }
paddr_t	vtop(a, p) caddr_t a; struct proc *p; { return (paddr_t)a; }
int	wakeup(ch) caddr_t ch; { woke = ch; return 0; }

int
sleep(ch, pri)
caddr_t ch;
int pri;
{
	long end = now + 50000000;

	while (woke != ch && now < end)
		step();
	if (woke != ch)
		simfail("sleep never woken");
	woke = 0;
	return 0;
}

static void
waitbufs(bs, n)
struct buf **bs;
int n;
{
	long end = now + 50000000;
	int i;

	for (;;) {
		for (i = 0; i < n && (bs[i]->b_flags & B_DONE); i++)
			;
		if (i == n || now >= end)
			break;
		step();
	}
	check(i == n, "requests finish");
}

int
uiophysio(strat, bp0, dev, rw, uio)
int (*strat)();
struct buf *bp0;
dev_t dev;
int rw;
struct uio *uio;
{
	struct buf b, *bp = &b;

	memset(&b, 0, sizeof b);
	b.b_flags = rw;
	b.b_bcount = uio->uio_resid;
	b.b_un.b_addr = uio->uio_base;
	b.b_blkno = uio->uio_offset / BSZ;
	b.b_proc = (struct proc *)&sim_proc;
	b.b_edev = dev;
	(*strat)(bp);
	waitbufs(&bp, 1);
	uio->uio_resid = b.b_resid;
	return (b.b_flags & B_ERROR) ? EIO : 0;
}

static uchar
pat(blk, i, seed)
long blk;
int i, seed;
{
	return blk * 7 + i * 13 + seed;
}

static struct buf *
mkbuf(rw, blk, n, seed)
int rw, n, seed;
long blk;
{
	struct buf *bp = calloc(1, sizeof *bp);
	int i;

	bp->b_flags = rw;
	bp->b_bcount = n * BSZ;
	bp->b_un.b_addr = malloc(n * BSZ);
	bp->b_blkno = blk;
	for (i = 0; i < n * BSZ; i++)
		bp->b_un.b_addr[i] = rw == B_READ ? 0xEE : pat(blk + i / BSZ, i % BSZ, seed);
	return bp;
}

/* n blocks of p (or the disk at unit 0) from blk hold the pattern */
static int
same(p, blk, n, seed)
uchar *p;
long blk;
int n, seed;
{
	int i;

	if (!p)
		p = T[0].disk + blk * BSZ;
	for (i = 0; i < n * BSZ; i++)
		if (p[i] != pat(blk + i / BSZ, i % BSZ, seed))
			return 0;
	return 1;
}

static int
stock(bp)
struct buf *bp;
{
	int i;

	nstock++;
	for (i = 0; i < (int)bp->b_bcount; i++)
		bp->b_un.b_addr[i] = T[0].disk[bp->b_blkno * BSZ + i];
	iodone(bp);
	return 0;
}

/* Queue n requests of k blocks from blk, wait, check reads against seed. */
static void
burst(rw, blk, n, k, seed)
int rw, n, k, seed;
long blk;
{
	struct buf *bs[32];
	int i;

	for (i = 0; i < n; i++) {
		bs[i] = mkbuf(rw, blk + i * k, k, seed);
		(*sd_strathook)(bs[i], stock);
	}
	waitbufs(bs, n);
	for (i = 0; i < n; i++) {
		check(!(bs[i]->b_flags & B_ERROR), "no error");
		if (rw == B_READ)
			check(same(bs[i]->b_un.b_addr, blk + i * k, k, seed), "read data");
	}
}

static void
cwarm()
{
	ncr_pdmamode = 4;
	warmup();
	sdc_attach();
}

/* adjacent writes go out as one command; a sequential read fetches ahead */
static void
c_merge()
{
	long n;

	cwarm();
	burst(B_WRITE, 10L, 4, 8, 1);
	check(sd_ncmd == 2 && sd_nmerge == 2, "first alone, then three merged");
	check(same((uchar *)0, 10L, 32, 1), "disk holds the writes");
	n = sd_ncmd;
	burst(B_READ, 10L, 1, 2, 1);
	burst(B_READ, 12L, 1, 2, 1);
	check(sd_nra == 1, "second sequential read fetches ahead");
	burst(B_READ, 14L, 6, 2, 1);
	check(sd_ncmd - n == 2, "two commands for eight reads");
	check(sd_nhit == 6, "six reads from the cache");
	check(nstock == 0, "stock path unused");
}

static int
stockraw(dev, uio, cr)
dev_t dev;
struct uio *uio;
struct cred *cr;
{
	nstock++;
	return 0;
}

/* writes over cached blocks, ours or the stock path's, are seen */
static void
c_cohere()
{
	struct uio u;
	long h;

	cwarm();
	burst(B_WRITE, 0L, 1, 64, 2);
	burst(B_READ, 0L, 1, 2, 2);
	check(sd_nra == 1, "cache filled");
	burst(B_WRITE, 20L, 1, 2, 3);
	h = sd_nhit;
	burst(B_READ, 20L, 1, 2, 3);
	check(sd_nhit == h, "overwritten blocks read from disk");
	burst(B_READ, 22L, 1, 2, 2);
	burst(B_READ, 24L, 1, 2, 2);
	check(sd_nhit == h + 1, "refilled");
	sd_bigraw = 0;
	memset(&u, 0, sizeof u);
	(*sd_rawhook)(0, &u, B_WRITE, stockraw, (struct cred *)0);
	sd_bigraw = 1;
	burst(B_READ, 26L, 1, 2, 2);
	check(sd_nhit == h + 1, "a stock-path write empties the cache");
	sd_clust = 0;
	burst(B_READ, 28L, 1, 2, 2);
	check(nstock == 2, "switched off: stock strategy");
}

/* a read-ahead past the end fails; each request is retried alone */
static void
c_split()
{
	cwarm();
	T[0].oob = 1;
	devsize = NBLK + 64;
	burst(B_WRITE, (long)NBLK - 8, 1, 8, 4);
	burst(B_READ, (long)NBLK - 8, 1, 2, 4);
	burst(B_READ, (long)NBLK - 6, 1, 2, 4);
	check(sd_nsplit == 1, "split once");
	burst(B_READ, (long)NBLK - 4, 1, 4, 4);
}

/* raw 100 KB in 64 KB commands through the bounce buffer */
static void
c_raw()
{
	static uchar ub[200 * BSZ];
	struct uio u;
	int i;

	cwarm();
	for (i = 0; i < 200 * BSZ; i++)
		ub[i] = pat(30L + i / BSZ, i % BSZ, 5);
	u.uio_offset = 30 * BSZ;
	u.uio_resid = 200 * BSZ;
	u.uio_base = (caddr_t)ub;
	check((*sd_rawhook)(0, &u, B_WRITE, stockraw, (struct cred *)0) == 0 && u.uio_resid == 0,
	    "raw write");
	check(same((uchar *)0, 30L, 200, 5), "disk holds the raw write");
	memset(ub, 0, sizeof ub);
	u.uio_offset = 30 * BSZ;
	u.uio_resid = 200 * BSZ;
	u.uio_base = (caddr_t)ub;
	check((*sd_rawhook)(0, &u, B_READ, stockraw, (struct cred *)0) == 0, "raw read");
	check(same(ub, 30L, 200, 5), "raw read data");
	check(sd_nraw == 4 && sd_nbounce == 4, "two commands each way");
	check(nstock == 0, "stock path unused");
}

static void x_rw() { ncr_pdmamode = 4; warmup(); rw(0, 3, 8); xcount(2L, 2L, 8192L); check(ncr_xresume == 0, "no resume"); }
static void x_polled() { ncr_pdmamode = 4; ncr_intrmode = 0; warmup(); rw(1, 0, 4); xcount(2L, 2L, 4096L); }

static void
x_big()
{
	ncr_pdmamode = 4;
	warmup();
	rw(2, 10, 200);			/* 102400 bytes: 0xFE00 + the rest */
	xcount(4L, 2L, 204800L);
}

static void
x_fault()
{
	ncr_pdmamode = 4;
	warmup();
	c.fault_chunk = 1;
	c.fault_word = 1001;
	rw(0, 0, 16);
	c.fault_chunk = c.chunkno + 1;
	c.fault_word = 3;
	rd(0, 0, 16);
	check(ncr_xresume == 2, "two bus errors resumed");
	check(ncr_xfall == 0, "no fallback");
	xcount(3L, 3L, 24576L);
}

/* data moves at IPL 1; a SCSI interrupt nested there is refused */
static void
x_nest()
{
	ncr_pdmamode = 4;
	warmup();
	c.nest_word = 100;
	rw(1, 0, 16);
	check(ncr_nnest == 1, "nested interrupt refused");
	check(c.xfer_sr == 0x2100, "transfer at IPL 1");
	check(sim_sr == 0x2200, "IPL 2 restored");
	check(ncr_ipl2max >= 0 && ncr_ipl2max < 1000, "stretch measured");
	ncr_xipl = 0;
	rd(1, 0, 16);
	check(c.xfer_sr == 0x2200, "transfer at IPL 2 when off");
	ncr_xipl = 1;
	xcount(3L, 3L, 24576L);
}

static void
x_stall()
{
	ncr_pdmamode = 4;
	warmup();
	T[2].stall_at = 3000;
	T[2].stall_us = 500;
	rw(2, 0, 16);
	T[2].stall_at = 4097;
	T[2].stall_until = 0;
	rd(2, 0, 16);
	check(ncr_xresume >= 2, "slow target: bus errors resumed");
	check(ncr_xfall == 0, "no fallback");
	xcount(3L, 3L, 24576L);
}

static void
x_disc()
{
	ncr_pdmamode = 4;
	warmup();
	T[1].disc_at = 2700;		/* mid-transfer: the port stalls */
	T[1].lag = 12;			/* the write leaves bytes in the FIFO */
	rw(1, 0, 16);
	T[1].disc_at = 7001;		/* odd: a byte stays in the FIFO */
	rd(1, 0, 16);
	T[1].disc_at = 512;
	T[1].nosdp = 1;
	rd(1, 3, 4);
	check(ncr_ndisc == 3, "three disconnects");
	check(ncr_xfall == 0, "no fallback");
}

static void
x_interleave()
{
	ncr_pdmamode = 4;
	t_interleave();
}

static void
x_odd()
{
	struct req *r;
	int i;

	ncr_pdmamode = 4;
	warmup();
	r = mkreq(0, 0x3C, 0, 301);	/* 300 by one command, one byte by PIO */
	ncr96queue(&r->sc);
	check(pump(&r, 1, 5000000), "finishes");
	for (i = 0; i < 301 && r->buf[i] == (uchar)(i * 3 + 1); i++)
		;
	check(r->okay && i == 301, "301-byte read");
	xcount(1L, 1L, 300L);
}

static void
x_nodreq()
{
	ncr_pdmamode = 4;
	warmup();
	c.nodreq = 1;
	rw(0, 1, 6);
	check(ncr_xfall == 2, "no DREQ: both finished by FIFO count");
}

static void
x_switch()
{
	warmup();
	ncr_pdmamode = 4;
	rw(0, 0, 4);
	ncr_pdmamode = 2;
	rw(0, 4, 4);
	ncr_pdmamode = 4;
	rw(0, 8, 4);
	check(ncr_nchunk == 16 && ncr_xcmd == 4, "modes switch between commands");
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
	{ "x_rw",	x_rw,		"mode 4: write + read, one command each" },
	{ "x_polled",	x_polled,	"mode 4: polled" },
	{ "x_big",	x_big,		"mode 4: 100 KB, two commands per phase" },
	{ "x_fault",	x_fault,	"mode 4: bus error mid-transfer, read and write" },
	{ "x_nest",	x_nest,		"mode 4: data at IPL 1, nested interrupt refused" },
	{ "c_merge",	c_merge,	"shaping: merged writes, read-ahead, cache hits" },
	{ "c_cohere",	c_cohere,	"shaping: writes over cached blocks" },
	{ "c_split",	c_split,	"shaping: failed read-ahead retried per request" },
	{ "c_raw",	c_raw,		"shaping: 64 KB raw commands" },
	{ "x_stall",	x_stall,	"mode 4: target stall beyond the glue timeout" },
	{ "x_disc",	x_disc,		"mode 4: disconnect mid-transfer, even and odd" },
	{ "x_interleave", x_interleave,	"mode 4: three targets interleaved" },
	{ "x_odd",	x_odd,		"mode 4: odd length, last byte by PIO" },
	{ "x_nodreq",	x_nodreq,	"mode 4: no DREQ, FIFO-count fallback" },
	{ "x_switch",	x_switch,	"modes 4 and 2 alternate" },
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
