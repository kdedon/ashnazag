/*
 * Quadra 800 on-board NCR 53C96 host adapter for the AMIX SCSI layer.
 *
 * Takes struct sdcom requests (sd.h) from sdqueue() and completes them
 * through cp->intr(cp), like the Amiga host adapters.  Each target has
 * its own queue and at most one command issued.  Targets are selected
 * with disconnect privilege; while one is disconnected another can be
 * selected, and a reselection restores the saved data pointer.
 *
 * Completion is driven by the VIA2 CB2 interrupt (level 2, ncr96intr).
 * A once-a-second watchdog services lost interrupts and times out hung
 * commands.  The caller polls the chip instead while interrupts cannot
 * be relied on: before the watchdog has first run (early boot), after a
 * panic, or when ncr_intrmode is 0.
 *
 * Data phases (ncr_pdmamode):
 *   2  256-byte chunks, each moved as 128 back-to-back
 *      16-bit accesses of the pseudo-DMA port after DREQ, in both
 *      directions, with a bus-error landing pad (ncr96pdma.s); the
 *      position after a fault comes from the chip's transfer counter;
 *      remainders go through the FIFO a byte at a time.
 *   1  pseudo-DMA sized from the FIFO count, so no access can stall.
 *   0  programmed I/O through the FIFO, one byte per transfer command.
 *   3  real DMA through an ncr_dmaops backend (hook only).
 */

#ifdef NCR_HOST
#include	"ncrhost.h"
#else
#include	"sys/types.h"
#include	"sys/param.h"
#include	"sys/errno.h"
#include	"rico.h"
#include	"sd.h"
#include	"macspl.h"
#endif
#include	"ncr96reg.h"

#define	TICKS		60		/* clock interrupts per second */
#define	MAXDMA		0x8000		/* bytes per transfer command */
#define	BLIND		256		/* mode 2 chunk */
#define	SPIN		4000000		/* FIFO polls before a data stall */
#define	WAITUS		10		/* poll interval, microseconds */
#define	NTGT		8

/* engine states */
#define	N_IDLE		0		/* bus free (reselection may be enabled) */
#define	N_SEL		1		/* selection running */
#define	N_PHASE		2		/* connected, waiting for the next phase */
#define	N_ICCS		3		/* status + message in running */
#define	N_MSGIN		4		/* one message byte running */
#define	N_MSGOK		5		/* message accepted */
#define	N_DATA		6		/* DMA transfer command running */
#define	N_PIO		7		/* one-byte transfer command running */

/* target states */
#define	T_FREE		0		/* no command issued */
#define	T_BUS		1		/* selecting or connected */
#define	T_DISC		2		/* disconnected */

struct ev {
	uchar	stat, step, intr;
};

struct tgt {
	struct sdcom	*qh, *qt;	/* waiting requests */
	struct sdcom	*cp;		/* issued request */
	int		st;
	caddr_t		dp, sp;		/* current and saved data pointer */
	uint		dl, sl;		/* bytes left at dp and at sp */
	uchar		status;
	bool		gotstatus;
	long		started;
};

/*
 * Real-DMA backend (the AV Quadras' PSC, the Q900/950 PDS card).  With
 * ncr_pdmamode 3 and ncr_dmaops set, each data phase is handed to it:
 * start() programs the engine for n bytes at physical address pa before
 * the chip's DMA transfer command; done() is called at the chip's next
 * interrupt with the bytes the chip counted and returns the bytes that
 * reached memory (reads) or the chip (writes).  None exists for the
 * Q800, whose 53C96 has no bus master.
 */
struct ncr_dmaops {
	int	(*start)();		/* (pa, n, in): 0 ok */
	uint	(*done)();		/* (took, in) */
};

extern long	lbolt;
extern char	*panicstr;
extern int	timeout();
extern void	delay(), delayus();
extern int	ncr_blind();

/* tunables */
int	ncr_intrmode = 2;		/* 0 polled, 1 interrupts, 2 automatic */
int	ncr_pdmamode = 2;
int	ncr_disc = 1;			/* grant disconnect privilege */
int	ncr_busreset = 1;
int	ncr_settle = 2 * TICKS;		/* after the bus reset, ticks */
long	ncr_timo = 30 * TICKS;		/* per command, interrupt mode */
long	ncr_polltimo = 30000000 / WAITUS;	/* per interrupt, polled mode */
long	ncr_spin = 100;			/* microseconds to wait for the next phase */
int	ncr_debug = 0;
struct ncr_dmaops *ncr_dmaops = 0;

/* counters */
long	ncr_ncmd = 0, ncr_nintr = 0, ncr_nerr = 0;
long	ncr_nlost = 0, ncr_nfault = 0, ncr_nnosel = 0;
long	ncr_ndisc = 0, ncr_nresel = 0, ncr_nchunk = 0, ncr_nnodreq = 0;

#ifdef BOOTDIAG
extern unsigned long diag_sdma;
#endif
static struct tgt	tg[NTGT];
static int	act = -1;		/* target on the bus */
static int	state;
static int	rr;			/* last target started */
static int	ndisc;			/* targets disconnected */
static bool	reselon;		/* enable-reselection issued */
static bool	junk;			/* connected to an unknown reselection */
static bool	inited, polling, watching, warm;
static uchar	lastmsg;		/* last message in */
static uchar	msgout;			/* message to send when asked, 0 none */
static int	msgcnt, msgwant;	/* extended message in progress */
static uint	xlen;			/* bytes in the running DMA command */
static bool	xin, pdisc;		/* direction; PIO byte discarded */

static void	ncr_start(), ncr_finish(), ncr_abort(), ncr_service();
static void	ncr_watch(), ncr_setup(), ncr_event(), ncr_phase(), ncr_data();
static void	ncr_msgin(), ncr_busfree(), ncr_resel(), ncr_resetall();
static void	ncr_dmaend(), ncr_pioend(), ncr_poll();
static int	ncr_wait();
static uint	ncr_pdma_in(), ncr_pdma_out(), ncr_tc();

static int
cdblen(op)
uchar op;
{
	switch (op >> 5) {
	case 0:
		return 6;
	case 5:
		return 12;
	default:
		return 10;
	}
}

static void
ncr_regs(e)
struct ev *e;
{
	e->stat = NCR_RD(STAT);
	e->step = NCR_RD(STEP) & 7;
	e->intr = NCR_RD(INTR);		/* last: the read clears the interrupt */
#ifdef BOOTDIAG
	diag_scsi(e->stat, e->step, e->intr, state);
#endif
}

static void
ncr_setup()
{
	NCR_WR(CMD, C_RSTCHIP);
	NCR_WR(CMD, C_NOP);
	delayus(500);			/* the chip needs time after a reset */
	NCR_WR(CCF, NCR_CCF);
	NCR_WR(SELTO, NCR_SELTO);
	NCR_WR(CFG1, NCR_CFG1);
	NCR_WR(CFG2, NCR_CFG2);
	NCR_WR(CFG3, NCR_CFG3);
	NCR_WR(SYNCOFF, 0);
	NCR_WR(SYNCTP, 5);
}

/* Interrupts cannot be relied on: poll in the caller. */
static int
ncr_polled()
{
	return ncr_intrmode == 0 || panicstr || (ncr_intrmode == 2 && !warm);
}

/*
 * Reset the chip and the bus, enable the VIA2 CB2 interrupt and start
 * the watchdog.  cansleep: process context with the clock running, so
 * the settle time may sleep.
 */
void
ncr96init(cansleep)
int cansleep;
{
	int s;

	if (inited)
		return;
	inited = TRUE;
	VIA2_WR(VIA_IER, V2_SCSIIRQ);
	ncr_setup();
	if (ncr_busreset) {
		NCR_WR(CMD, C_RSTSCSI);
		NCR_WR(CMD, C_NOP);
		if (cansleep)
			delay(ncr_settle);
		else
			delayus(ncr_settle * (1000000 / TICKS));
	}
	if (NCR_RD(STAT) & S_INT)
		(void)NCR_RD(INTR);
	printf("ncr96: 53C96 at 0x%x, id %d, %s, data mode %d\n", NCR_BASE, NCR_ID,
	    ncr_intrmode == 0 ? "polled" : "interrupts", ncr_pdmamode);
	if (ncr_intrmode) {
		s = splscsi();
		/* CB2 (IRQ) and CA2 (DREQ): independent falling-edge inputs */
		VIA2_WR(VIA_PCR, (VIA2_RD(VIA_PCR) & 0x11) | 0x22);
		VIA2_WR(VIA_IFR, V2_SCSIIRQ);
		VIA2_WR(VIA_IER, 0x80 | V2_SCSIIRQ);
		splrestore(s);
	}
	if (!watching) {
		watching = TRUE;
		timeout(ncr_watch, (caddr_t)0, TICKS);
	}
}

/*
 * sdqueue() entry: append the request to its target's queue and start
 * it if the bus is free.  Polls to completion when interrupts cannot be
 * relied on.
 */
void
ncr96queue(cp)
struct sdcom *cp;
{
	struct tgt *t;
	int s;

	s = splscsi();
	ncr96init(0);
	cp->okay = FALSE;
	cp->next = 0;
	if (cp->card != 0 || cp->unit >= NTGT || cp->unit == NCR_ID) {
		cp->status = 0xFF;
		(*cp->intr)(cp);
		splrestore(s);
		return;
	}
	t = &tg[cp->unit];
	if (t->qh)
		t->qt->next = cp;
	else
		t->qh = cp;
	t->qt = cp;
	if (!ncr_polled())
		ncr_start();
	else if (!polling)
		ncr_poll();
	splrestore(s);
}

static int
ncr_busy()
{
	int u;

	for (u = 0; u < NTGT; u++)
		if (tg[u].cp || tg[u].qh)
			return 1;
	return 0;
}

/* Run the chip until every queue is empty.  At IPL 2. */
static void
ncr_poll()
{
	/*
	 * CB2 off while polling: where the VIA2 flag follows the chip's
	 * INT line, an interrupt taken now would be ignored and repeat.
	 */
	if (inited && ncr_intrmode)
		VIA2_WR(VIA_IER, V2_SCSIIRQ);
	polling = TRUE;
	for (;;) {
		ncr_start();
		if (!ncr_busy())
			break;
		if (ncr_wait(ncr_polltimo * WAITUS))
			ncr_service();
		else
			ncr_abort("command timeout");
	}
	polling = FALSE;
	if (inited && ncr_intrmode)
		VIA2_WR(VIA_IER, 0x80 | V2_SCSIIRQ);
}

/*
 * VIA2 CB2 interrupt, level 2.  The dispatcher has acknowledged the VIA.
 */
void
ncr96intr()
{
	ncr_nintr++;
#ifdef BOOTDIAG
	diag_poke();
#endif
	if (!inited) {
		if (NCR_RD(STAT) & S_INT)
			(void)NCR_RD(INTR);
		return;
	}
	if (polling)
		return;
	ncr_service();
	ncr_start();
}

/*
 * Handle chip interrupts until none is pending.  While connected past
 * selection, wait up to ncr_spin for the next one: most phases follow
 * within microseconds, cheaper than another VIA interrupt.
 */
static void
ncr_service()
{
	struct ev e;

	for (;;) {
		if (!(NCR_RD(STAT) & S_INT)
		&& (act < 0 || state == N_SEL || !ncr_wait(ncr_spin)))
			break;
		ncr_regs(&e);
		if (ncr_debug)
			printf("ncr96: st %d tgt %d stat %x step %d intr %x\n",
			    state, act, e.stat, e.step, e.intr);
		ncr_event(&e);
	}
}

static void
ncr_watch()
{
	int s, u;

	s = splscsi();
	warm = TRUE;
	if (!polling && ncr_intrmode) {
		if (NCR_RD(STAT) & S_INT) {
			ncr_nlost++;
			ncr_service();
		}
		for (u = 0; u < NTGT; u++)
			if (tg[u].cp && lbolt - tg[u].started > ncr_timo) {
				ncr_abort("command timeout");
				break;
			}
		ncr_start();
	}
	splrestore(s);
	timeout(ncr_watch, (caddr_t)0, TICKS);
}

/*
 * Bus free: select the next target that has a request and no command
 * issued, round robin.  With a target disconnected, reselection is
 * enabled first.
 */
static void
ncr_start()
{
	struct sdcom *cp;
	struct tgt *t;
	int i, n, u;

	if (state != N_IDLE || !inited)
		return;
	if (NCR_RD(STAT) & S_INT)		/* a reselection, most likely */
		return;
	for (i = 1; i <= NTGT; i++) {
		u = (rr + i) % NTGT;
		if (!tg[u].cp && tg[u].qh)
			break;
	}
	if (ndisc && !reselon) {
		NCR_WR(CMD, C_FLUSH);
		NCR_WR(CMD, C_ENSEL);
		reselon = TRUE;
	}
	if (i > NTGT)
		return;
	rr = u;
	t = &tg[u];
	cp = t->qh;
	t->qh = (struct sdcom *)cp->next;
	cp->next = 0;
	t->cp = cp;
	t->st = T_BUS;
	t->dp = t->sp = cp->addr;
	t->dl = t->sl = cp->nbyte;
	t->status = 0xFF;
	t->gotstatus = FALSE;
	t->started = lbolt;
	act = u;
	junk = FALSE;
	lastmsg = 0xFF;
	msgout = 0;
	msgcnt = msgwant = 0;
	ncr_ncmd++;
	NCR_WR(CMD, C_FLUSH);
	NCR_WR(SELID, u);
	NCR_WR(SELTO, NCR_SELTO);
	NCR_WR(SYNCOFF, 0);
	NCR_WR(FIFO, ncr_disc ? M_IDENTIFY | M_DISCPRIV : M_IDENTIFY);
	n = cdblen(cp->cdb[0]);
	for (i = 0; i < n; i++)
		NCR_WR(FIFO, cp->cdb[i]);
	state = N_SEL;
	reselon = FALSE;
	NCR_WR(CMD, C_SELATN);
}

/*
 * Hand target u's request back.  The engine is idle (for this target)
 * before the callback runs, so the callback may queue the next request.
 */
static void
ncr_finish(u, okay)
int u, okay;
{
	struct sdcom *cp;
	struct tgt *t;

	t = &tg[u];
	cp = t->cp;
	if (act == u) {
		act = -1;
		state = N_IDLE;
	}
	if (t->st == T_DISC)
		ndisc--;
	t->cp = 0;
	t->st = T_FREE;
	if (cp) {
		cp->okay = okay;
		cp->status = t->status;
		(*cp->intr)(cp);
	}
	ncr_start();
}

/* Fail every issued request (the bus was reset); queued ones stay. */
static void
ncr_resetall()
{
	struct sdcom *cps[NTGT];
	int u;

	for (u = 0; u < NTGT; u++) {
		cps[u] = tg[u].cp;
		tg[u].cp = 0;
		tg[u].st = T_FREE;
	}
	act = -1;
	state = N_IDLE;
	ndisc = 0;
	reselon = FALSE;
	junk = FALSE;
	for (u = 0; u < NTGT; u++)
		if (cps[u]) {
			cps[u]->okay = FALSE;
			cps[u]->status = tg[u].status;
			(*cps[u]->intr)(cps[u]);
		}
	ncr_start();
}

/* Reset the bus and chip, fail the issued requests. */
static void
ncr_abort(why)
char *why;
{
	struct sdcom *cp;

	ncr_nerr++;
	cp = act >= 0 ? tg[act].cp : 0;
	printf("ncr96: %s, target %d, cdb 0x%x, state %d; resetting the bus\n", why,
	    act, cp ? cp->cdb[0] : 0, state);
	NCR_WR(CMD, C_RSTSCSI);
	NCR_WR(CMD, C_NOP);
	delayus(ncr_settle * (1000000 / TICKS));	/* as long as at init */
	ncr_setup();
	if (NCR_RD(STAT) & S_INT)
		(void)NCR_RD(INTR);
	ncr_resetall();
}

/* A selection lost to a reselection: the request goes back in front. */
static void
ncr_backoff()
{
	struct tgt *t;

	t = &tg[act];
	t->cp->next = t->qh;
	if (!t->qh)
		t->qt = t->cp;
	t->qh = t->cp;
	t->cp = 0;
	t->st = T_FREE;
	act = -1;
	state = N_IDLE;
	ncr_ncmd--;
}

/* One chip interrupt. */
static void
ncr_event(e)
struct ev *e;
{
	struct tgt *t;
	int n;

	if (e->intr & I_SBR) {			/* reset by someone else */
		ncr_setup();
		ncr_resetall();
		return;
	}
	if (e->intr & I_ILL) {
		ncr_abort("illegal command");
		return;
	}
	if (e->intr & I_RESEL) {
		if (state == N_SEL)
			ncr_backoff();
		if (state != N_IDLE) {
			ncr_abort("reselection while connected");
			return;
		}
		ncr_resel(e);
		return;
	}
	t = &tg[act < 0 ? 0 : act];
	switch (state) {
	case N_IDLE:
		return;
	case N_SEL:
		if (e->intr & I_DIS) {
			NCR_WR(CMD, C_FLUSH);
			if (e->step == 0)
				ncr_nnosel++;		/* selection timeout */
			ncr_finish(act, FALSE);
			return;
		}
		state = N_PHASE;
		break;
	case N_DATA:
		ncr_dmaend(e);
		break;
	case N_PIO:
		ncr_pioend();
		break;
	case N_ICCS:
		n = NCR_RD(FFLAG) & FF_COUNT;
		if (n > 0) {
			t->status = NCR_RD(FIFO);
			t->gotstatus = TRUE;
		}
		if ((e->intr & I_FC) && n > 1) {
			ncr_msgin(NCR_RD(FIFO));
			return;
		}
		state = N_PHASE;
		break;
	case N_MSGIN:
		if (e->intr & I_FC) {
			ncr_msgin(NCR_RD(FIFO));
			return;
		}
		state = N_PHASE;
		break;
	}
	if (e->intr & I_DIS)
		ncr_busfree();
	else if (e->intr & I_BS)
		ncr_phase(e);
}

/* The target released the bus. */
static void
ncr_busfree()
{
	struct tgt *t;

	NCR_WR(CMD, C_FLUSH);
	t = &tg[act];
	if (junk) {
		junk = FALSE;
		act = -1;
		state = N_IDLE;
		ncr_start();
	} else if (lastmsg == M_DISCONNECT) {
		ncr_ndisc++;
		t->st = T_DISC;
		ndisc++;
		act = -1;
		state = N_IDLE;
		ncr_start();
	} else
		ncr_finish(act, t->gotstatus);
}

/*
 * Reselected: the FIFO holds the bus ID bits and the IDENTIFY message,
 * with ACK held on the IDENTIFY.  Restore the target's saved pointers
 * and accept the message.  An unknown nexus gets ABORT.
 */
static void
ncr_resel(e)
struct ev *e;
{
	struct tgt *t;
	int n, u, bits;
	uchar id, ident;

	ncr_nresel++;
	n = NCR_RD(FFLAG) & FF_COUNT;
	id = n > 0 ? NCR_RD(FIFO) : 0;
	ident = n > 1 ? NCR_RD(FIFO) : M_IDENTIFY;
	NCR_WR(CMD, C_FLUSH);
	reselon = FALSE;
	bits = id & ~(1 << NCR_ID) & 0xFF;
	for (u = 0; u < NTGT && !(bits & 1 << u); u++)
		;
	if (u == NTGT || bits != 1 << u || (e->stat & S_PHASE) != P_MSGIN) {
		ncr_abort("bad reselection");
		return;
	}
	t = &tg[u];
	act = u;
	lastmsg = 0xFF;
	msgout = 0;
	msgcnt = msgwant = 0;
	if (t->st != T_DISC || (ident & ~M_DISCPRIV) != M_IDENTIFY) {
		junk = TRUE;
		msgout = M_ABORT;
		NCR_WR(CMD, C_SETATN);
	} else {
		junk = FALSE;
		t->st = T_BUS;
		ndisc--;
		t->dp = t->sp;
		t->dl = t->sl;
	}
	state = N_MSGOK;
	NCR_WR(CMD, C_MSGOK);
}

/* One message byte is in; ACK is held until MESSAGE ACCEPTED. */
static void
ncr_msgin(b)
uchar b;
{
	struct tgt *t;

	t = &tg[act];
	if (msgwant) {
		if (++msgcnt == 2)
			msgwant = b + 2;
		if (msgcnt >= msgwant) {	/* extended message done: refuse it */
			msgwant = 0;
			msgout = M_REJECT;
			NCR_WR(CMD, C_SETATN);
		}
	} else
		switch (b) {
		case M_CMDCOMPLETE:
		case M_DISCONNECT:
			lastmsg = b;
			break;
		case M_SAVEDP:
			if (!junk) {
				t->sp = t->dp;
				t->sl = t->dl;
			}
			break;
		case M_RESTOREDP:
			if (!junk) {
				t->dp = t->sp;
				t->dl = t->sl;
			}
			break;
		case M_REJECT:
		case M_NOP:
			break;
		case M_EXTENDED:
			msgcnt = 1;
			msgwant = 2;
			break;
		default:
			if (b & M_IDENTIFY)	/* IDENTIFY again: same nexus */
				break;
			msgout = M_REJECT;
			NCR_WR(CMD, C_SETATN);
		}
	state = N_MSGOK;
	NCR_WR(CMD, C_MSGOK);
}

/* Bus service: start the transfer the target's phase asks for. */
static void
ncr_phase(e)
struct ev *e;
{
	struct sdcom *cp;
	int i, n, ph;

	cp = tg[act].cp;
	ph = e->stat & S_PHASE;
	NCR_WR(CMD, C_FLUSH);
	if (junk && ph != P_MSGOUT && ph != P_MSGIN) {
		ncr_abort("unknown reselection");
		return;
	}
	switch (ph) {
	case P_DATAOUT:
	case P_DATAIN:
		ncr_data(ph);
		return;
	case P_CMD:
		n = cdblen(cp->cdb[0]);
		for (i = 0; i < n; i++)
			NCR_WR(FIFO, cp->cdb[i]);
		NCR_WR(CMD, C_TRANS);
		state = N_PHASE;
		return;
	case P_STATUS:
		NCR_WR(CMD, C_ICCS);
		state = N_ICCS;
		return;
	case P_MSGOUT:
		NCR_WR(FIFO, msgout ? msgout : M_NOP);
		msgout = 0;
		NCR_WR(CMD, C_TRANS);
		state = N_PHASE;
		return;
	case P_MSGIN:
		NCR_WR(CMD, C_TRANS);
		state = N_MSGIN;
		return;
	}
	ncr_abort("bad phase");
}

/* One byte through the FIFO (non-DMA transfer information). */
static void
ncr_pio(in, discard)
int in, discard;
{
	xin = in;
	pdisc = discard;
	if (!in)
		NCR_WR(FIFO, *tg[act].dp);
	NCR_WR(CMD, C_TRANS);
	state = N_PIO;
}

static void
ncr_pioend()
{
	struct tgt *t;
	uchar b;
	int n;

	t = &tg[act];
	n = NCR_RD(FFLAG) & FF_COUNT;
	if (xin) {
		if (n > 0) {
			b = NCR_RD(FIFO);
			if (!pdisc) {
				*t->dp++ = b;
				t->dl--;
			}
		}
	} else if (n == 0) {
		t->dp++;
		t->dl--;
	}
	state = N_PHASE;
}

/* Current transfer count, read consistently while the chip may count. */
static uint
ncr_tc()
{
	uint h, l;

	do {
		h = NCR_RD(TCM);
		l = NCR_RD(TCL);
	} while (h != NCR_RD(TCM));
	return h << 8 | l;
}

/*
 * Bytes of the running DMA command that the CPU has moved.  The counter
 * counts bytes entering the FIFO: from the bus on reads (the CPU has
 * those not still in the FIFO), from the CPU on writes.
 */
static uint
ncr_cpudone()
{
	uint took;

	took = xlen - ncr_tc();
	if (xin)
		took -= NCR_RD(FFLAG) & FF_COUNT;
	return took;
}

/*
 * Data phase.  Up to MAXDMA bytes per DMA transfer command (BLIND in
 * mode 2); a remainder, an odd address or mode 0 moves one byte.  The
 * chip's next interrupt ends the command (ncr_dmaend).
 */
static void
ncr_data(ph)
int ph;
{
	struct tgt *t;
	uint n, done;
	int in, mode, r;

	t = &tg[act];
	in = ph == P_DATAIN;
	if (in != (t->cp->reading != 0)) {
		ncr_abort(in ? "unexpected data in" : "unexpected data out");
		return;
	}
	if (t->dl == 0) {
		if (in)
			ncr_pio(in, TRUE);	/* read overrun: drain it */
		else
			ncr_abort("data out overrun");
		return;
	}
	n = t->dl > MAXDMA ? MAXDMA : t->dl;
	mode = ncr_pdmamode;
	if (mode == 3 && !ncr_dmaops)
		mode = 2;
	if (mode != 3 && (n < 2 || ((long)t->dp & 1)
	|| (mode == 2 && n < BLIND) || (mode == 1 && n < FIFO_SIZE)))
		mode = 0;
	if (mode == 0) {
		ncr_pio(in, FALSE);
		return;
	}
	if (mode == 2)
		n = BLIND;
	else if (mode == 1)
		n &= ~1;
	xlen = n;
	xin = in;
	if (!in)
		NCR_WR(CMD, C_FLUSH);	/* no stale bytes ahead of the data */
	if (mode == 3 && (*ncr_dmaops->start)(t->dp, n, in) != 0) {
		ncr_abort("DMA start");
		return;
	}
	NCR_WR(TCL, n);
	NCR_WR(TCM, n >> 8);
	NCR_WR(CMD, C_DMA | C_TRANS);
	state = N_DATA;
	if (mode == 3)
		return;
	done = 0;
	if (mode == 2) {
		ncr_nchunk++;
		r = ncr_blind(t->dp, !in);
		if (r == BLIND)
			return;
		if (r == -2)
			ncr_nfault++;
		else
			ncr_nnodreq++;
		if (NCR_RD(STAT) & S_INT)
			return;
		done = ncr_cpudone();		/* still running: finish by FIFO count */
	}
	if (done < n)
		done += in ? ncr_pdma_in(t->dp + done, n - done)
			   : ncr_pdma_out(t->dp + done, n - done);
	if (done < n && !(NCR_RD(STAT) & S_INT))
		ncr_abort("data phase stall");
}

/*
 * The DMA transfer command ended (count reached or phase change).  The
 * position is what the chip counted, less bytes of a write still in the
 * FIFO; bytes of a read still in the FIFO are taken through it.
 */
static void
ncr_dmaend(e)
struct ev *e;
{
	struct tgt *t;
	uint took, n;
	caddr_t p;

	t = &tg[act];
	took = xlen - ((e->stat & S_TC) ? 0 : ncr_tc());
	n = NCR_RD(FFLAG) & FF_COUNT;
	if (ncr_pdmamode == 3 && ncr_dmaops)
		took = (*ncr_dmaops->done)(took, xin);
	else if (xin) {
		if (n > took)
			n = took;
		for (p = t->dp + took - n; n > 0; n--)
			*p++ = NCR_RD(FIFO);
	} else if (n > 0) {
		took = n > took ? 0 : took - n;
		NCR_WR(CMD, C_FLUSH);
	}
#ifdef BOOTDIAG
	diag_sdma += took;
#endif
	t->dp += took;
	t->dl -= took;
	state = N_PHASE;
}

/*
 * Mode 1 reads: take only what the FIFO holds, a word at a time, so the
 * DACK cycle never waits.  Stops when the chip interrupts with less than
 * a word left in the FIFO (phase change).
 */
static uint
ncr_pdma_in(p, n)
caddr_t p;
uint n;
{
	register ushort *d;
	register uint k, left;
	long spin;

	d = (ushort *)p;
	left = n;
	spin = 0;
	while (left > 1) {
		k = NCR_RD(FFLAG) & FF_COUNT;
		if (k >= 2) {
			k >>= 1;
			if (k > left >> 1)
				k = left >> 1;
			left -= k << 1;
			while (k--)
				*d++ = PDMA_RD(PDMA_PORT);
			spin = 0;
		} else if (NCR_RD(STAT) & S_INT) {
			if ((NCR_RD(FFLAG) & FF_COUNT) < 2)
				break;
		} else if (++spin > SPIN)
			break;
	}
	return n - left;
}

static uint
ncr_pdma_out(p, n)
caddr_t p;
uint n;
{
	register ushort *s;
	register uint k, left;
	long spin;

	s = (ushort *)p;
	left = n;
	spin = 0;
	while (left > 1) {
		k = FIFO_SIZE - (NCR_RD(FFLAG) & FF_COUNT);
		if (k >= 2) {
			k >>= 1;
			if (k > left >> 1)
				k = left >> 1;
			left -= k << 1;
			while (k--)
				PDMA_WR(PDMA_PORT, *s++);
			spin = 0;
		} else if (NCR_RD(STAT) & S_INT)
			break;
		else if (++spin > SPIN)
			break;
	}
	return n - left;
}

/* Wait for a chip interrupt: 1 if one is pending. */
static int
ncr_wait(us)
long us;
{
	long n;

	for (n = 0; n < us; n += WAITUS) {
		if (NCR_RD(STAT) & S_INT)
			return 1;
		delayus(WAITUS);
#ifdef BOOTDIAG
		diag_poke();
#endif
	}
	return (NCR_RD(STAT) & S_INT) != 0;
}
