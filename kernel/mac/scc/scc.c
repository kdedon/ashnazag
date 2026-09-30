/*
 * STREAMS tty driver for the Quadra 800 Z85C30 SCC.
 *
 * Minor 0 is channel A (modem port), minor 1 channel B (printer
 * port); SCC_MODEM and SCC_HWFLOW in the minor add carrier and
 * CTS/DTR handshaking.  ldterm and ttcompat are pushed above.
 *
 * The level-4 handler only moves bytes between the chip and two rings
 * per channel and calls qenable() or wakeup(); every STREAMS message is
 * built or consumed in the service procedures.  Driver code touches
 * the chip only at IPL 4, which also masks the SCC, so register-pointer
 * sequences are never interleaved.  The polled console putchar() reads
 * only RR0 and the data port and so can run beside this driver.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/dir.h"
#include "sys/file.h"
#include "sys/signal.h"
#include "sys/termio.h"
#include "sys/termios.h"
#include "sys/stream.h"
#include "sys/stropts.h"
#include "sys/strtty.h"
#include "sys/errno.h"
#include "sys/conf.h"
#include "sys/sysmacros.h"
#include "sys/systm.h"
#include "sys/cred.h"
#include "scc.h"

#define SCC_IPL		0x2400
#define BRKTICKS	15		/* 1/4 s at 60 Hz */
#define CP_IN		1		/* cq_private: copyin done */
#define CP_OUT		2		/* cq_private: copyout done */
#define RXMSGSZ		128

/*
 * At least IPL 4 (never lowers), with a compiler barrier, since ring
 * indices move under the ISR.
 */
#define scc_spl() ({ int __s; \
	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (__s) : : "memory"); \
	if ((__s & 0x700) < 0x400) \
		__asm__ __volatile__("mov.w &0x2400,%%sr" : : : "memory"); \
	__s; })
#define scc_splx(s) __asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory")

/* one VIA1 access (about 1 us) of recovery time between SCC accesses */
#ifdef SCC_NODELAY
#define SCC_DELAY()
#else
#define SCC_DELAY()	((void)*(VOL unsigned char *)0x50F01A00)
#endif

extern unsigned long mac_scc;

int sccopen(), sccclose(), sccwput(), sccwsrv(), sccrsrv();
void sccintr(), scc_conin();
extern void fbcons_write();
extern int fbcons_active();

static struct module_info scc_minfo = {
	0x5343, "scc", 0, INFPSZ, 512, 128,
};

struct qinit scc_rinit = {
	NULL, sccrsrv, sccopen, sccclose, NULL, &scc_minfo, NULL,
};

struct qinit scc_winit = {
	sccwput, sccwsrv, NULL, NULL, NULL, &scc_minfo, NULL,
};

struct streamtab sccinfo = {
	&scc_rinit, &scc_winit, NULL, NULL,
};

static struct scc scc_sc[SCC_NCHAN];
static int scc_ready;
unsigned long scc_nintr = 0, scc_nspur = 0;	/* interrupts, empty ones */
unsigned long scc_nmirdrop = 0;		/* console bytes not mirrored to the SCC */

/* BRG time constants: 3.6864 MHz RTxC, x16 clock: 115200/baud - 2 */
static unsigned short scc_tc[16] = {
	0, 2302, 1534, 1045, 855, 766, 574, 382,
	190, 94, 62, 46, 22, 10, 4, 1,
};

static unsigned char scc_ccdef[NCCS] = {
	CINTR, CQUIT, CERASE, CKILL, CEOF, 0, 0, CSWTCH,
	CSTART, CSTOP, CSUSP, CDSUSP, CRPRNT, CFLUSH, CWERASE, CLNEXT,
};

static int scc_rreg(), scc_txnext(), scc_drained();
static void scc_wreg(), scc_attach(), scc_param(), scc_setwr5();
static void scc_kick(), scc_throttle(), scc_unthrottle();
static void scc_rxint(), scc_txint(), scc_stint();
static void scc_rflush(), scc_wflush(), scc_shutdown(), scc_waitdrain();
static void scc_tmo(), scc_brkend(), scc_drainto(), scc_bufcb();
static void scc_ioctl(), scc_iocdata(), scc_setattr(), scc_getattr();
static void scc_mctl(), scc_ack(), scc_nak(), scc_copyin(), scc_copyout();
static mblk_t *scc_txfill();
static void scc_conout();

/* ------------------------------------------------------------ chip */

static int
scc_rreg(sp, r)
register struct scc *sp;
int r;
{
	register int v;

	*sp->sc_ctl = r;
	SCC_DELAY();
	v = *sp->sc_ctl;
	SCC_DELAY();
	return v;
}

static void
scc_wreg(sp, r, v)
register struct scc *sp;
int r, v;
{
	if (r) {
		*sp->sc_ctl = r;
		SCC_DELAY();
	}
	*sp->sc_ctl = v;
	SCC_DELAY();
	sp->sc_wr[r] = v;
}

static int
scc_rr0(sp)
register struct scc *sp;
{
	register int v;

	v = *sp->sc_ctl;
	SCC_DELAY();
	return v;
}

#define RR0(sp)		scc_rr0(sp)
#define WCMD(sp, c)	(*(sp)->sc_ctl = (c), SCC_DELAY())

/*
 * Once, at the first open: point the channels at the chip, quiet
 * both, and set the master interrupt enable.
 */
static void
scc_attach()
{
	register struct scc *sp;
	register int i;

	scc_sc[0].sc_ctl = (VOL unsigned char *)(mac_scc + 2);
	scc_sc[0].sc_data = (VOL unsigned char *)(mac_scc + 6);
	scc_sc[1].sc_ctl = (VOL unsigned char *)(mac_scc + 0);
	scc_sc[1].sc_data = (VOL unsigned char *)(mac_scc + 4);
	for (i = 0; i < SCC_NCHAN; i++) {
		sp = &scc_sc[i];
		scc_wreg(sp, 1, 0);
		scc_wreg(sp, 15, 0);
		WCMD(sp, W0_RSTEXT);
		WCMD(sp, W0_RSTEXT);
		WCMD(sp, W0_RSTERR);
		WCMD(sp, W0_RSTTXIP);
		sp->sc_rr0 = RR0(sp);
	}
	/* channel A runs as the polled console: DTR, RTS, Tx on, 8 bits */
	scc_sc[0].sc_wr[5] = W5_DTR | W5_TX8 | W5_TXEN | W5_RTS;
	scc_wreg(&scc_sc[0], 9, W9_MIE | W9_NV);
	scc_ready = 1;
}

/*
 * Load the channel from the termios state.  The full sequence runs
 * when the character format or the rate changes; otherwise only
 * WR3, WR5 and WR15 are rewritten.
 */
static void
scc_param(sp)
register struct scc *sp;
{
	register struct strtty *tp = &sp->sc_tty;
	register long cf = tp->t_cflag;
	register int i;
	int wr3, wr4, wr5, wr15, tc, full;

	wr4 = W4_X16 | ((cf & CSTOPB) ? W4_2SB : W4_1SB);
	if (cf & PARENB)
		wr4 |= W4_PAREN | ((cf & PARODD) ? 0 : W4_EVEN);
	switch (cf & CSIZE) {
	case CS5: wr3 = W3_RX5; wr5 = W5_TX5; break;
	case CS6: wr3 = W3_RX6; wr5 = W5_TX6; break;
	case CS7: wr3 = W3_RX7; wr5 = W5_TX7; break;
	default:  wr3 = W3_RX8; wr5 = W5_TX8; break;
	}
	if (cf & CREAD)
		wr3 |= W3_RXEN;
	/* B0 hangs up (scc_setwr5) and keeps the rate */
	if (cf & CBAUD)
		tc = scc_tc[cf & CBAUD];
	else
		tc = sp->sc_wr[13] << 8 | sp->sc_wr[12];
	wr15 = W15_BRKIE;
	if (sp->sc_minor & SCC_MODEM)
		wr15 |= W15_DCDIE;
	if (sp->sc_minor & SCC_HWFLOW)
		wr15 |= W15_CTSIE;

	full = !(sp->sc_flags & SCF_HWINIT) || wr4 != sp->sc_wr[4] ||
	    (wr3 & ~W3_RXEN) != (sp->sc_wr[3] & ~W3_RXEN) ||
	    (wr5 & W5_TX8) != (sp->sc_wr[5] & W5_TX8) ||
	    (tc & 0xFF) != sp->sc_wr[12] || ((tc >> 8) & 0xFF) != sp->sc_wr[13];
	if (full) {
		/* let the last character leave; bounded, a few ms */
		for (i = 0; i < 5000; i++)
			if (scc_rreg(sp, 1) & R1_ALLSENT)
				break;
		scc_wreg(sp, 4, wr4);
		scc_wreg(sp, 10, 0);
		scc_wreg(sp, 3, wr3 & ~W3_RXEN);
		scc_wreg(sp, 5, (sp->sc_wr[5] & (W5_DTR|W5_RTS)) | wr5);
		scc_wreg(sp, 11, W11_BRG);
		scc_wreg(sp, 14, 0);
		scc_wreg(sp, 12, tc & 0xFF);
		scc_wreg(sp, 13, (tc >> 8) & 0xFF);
		scc_wreg(sp, 14, W14_BRGEN);
		sp->sc_flags |= SCF_HWINIT;
	}
	if (full || wr3 != sp->sc_wr[3])
		scc_wreg(sp, 3, wr3);
	sp->sc_wr[5] = (sp->sc_wr[5] & ~(W5_TX8|W5_TXEN)) | wr5 | W5_TXEN;
	scc_setwr5(sp);
	if (full || wr15 != sp->sc_wr[15]) {
		scc_wreg(sp, 15, wr15);
		WCMD(sp, W0_RSTEXT);
		WCMD(sp, W0_RSTEXT);
	}
	if (sp->sc_wr[1] != (W1_RIE|W1_TIE|W1_SIE))
		scc_wreg(sp, 1, W1_RIE|W1_TIE|W1_SIE);
	sp->sc_rr0 = RR0(sp);
}

/* WR5 from the modem-control, throttle and break state */
static void
scc_setwr5(sp)
register struct scc *sp;
{
	register int v = sp->sc_wr[5] & ~(W5_DTR|W5_RTS|W5_BREAK);

	if ((sp->sc_mctl & TIOCM_DTR) && (sp->sc_tty.t_cflag & CBAUD) &&
	    !((sp->sc_minor & SCC_HWFLOW) &&
	      (sp->sc_flags & (SCF_RBLOCK|SCF_IBLOCK))))
		v |= W5_DTR;
	if (sp->sc_mctl & TIOCM_RTS)
		v |= W5_RTS;
	if (sp->sc_flags & SCF_BREAK)
		v |= W5_BREAK;
	scc_wreg(sp, 5, v);
}

/*
 * Put the next character into an empty transmitter.  Returns nonzero
 * if one was written.
 */
static int
scc_txnext(sp)
register struct scc *sp;
{
	register int c;

	if (sp->sc_xchar) {
		c = sp->sc_xchar;
		sp->sc_xchar = 0;
	} else {
		if ((sp->sc_tty.t_state & (TTSTOP|TIMEOUT)) ||
		    (sp->sc_flags & SCF_BREAK))
			return 0;
		if ((sp->sc_minor & SCC_HWFLOW) && !CTS_ON(sp->sc_rr0))
			return 0;
		if (sp->sc_tget == sp->sc_tput)
			return 0;
		c = sp->sc_tbuf[sp->sc_tget];
		sp->sc_tget = (sp->sc_tget + 1) & (TBSZ - 1);
		sp->sc_ntx++;
	}
	*sp->sc_data = c;
	SCC_DELAY();
	sp->sc_flags |= SCF_TXBUSY;
	sp->sc_tty.t_state |= BUSY;
	return 1;
}

/* Start output if the transmitter is idle.  IPL 4. */
static void
scc_kick(sp)
register struct scc *sp;
{
	if (!(sp->sc_flags & SCF_TXBUSY) && (RR0(sp) & R0_TXRDY))
		(void) scc_txnext(sp);
}

/* ------------------------------------------------------- interrupt */

static void
scc_throttle(sp)
register struct scc *sp;
{
	sp->sc_flags |= SCF_RBLOCK;
	if (sp->sc_tty.t_iflag & IXOFF) {
		sp->sc_xchar = sp->sc_tty.t_cc[VSTOP];
		scc_kick(sp);
	}
	if (sp->sc_minor & SCC_HWFLOW)
		scc_setwr5(sp);
}

static void
scc_unthrottle(sp)
register struct scc *sp;
{
	sp->sc_flags &= ~SCF_RBLOCK;
	if (sp->sc_flags & SCF_IBLOCK)
		return;
	if (sp->sc_tty.t_iflag & IXOFF) {
		sp->sc_xchar = sp->sc_tty.t_cc[VSTART];
		scc_kick(sp);
	}
	if (sp->sc_minor & SCC_HWFLOW)
		scc_setwr5(sp);
}

static void
scc_rxint(sp)
register struct scc *sp;
{
	register int c, rr1, n, got;

	got = 0;
	for (n = 0; n < 16 && (RR0(sp) & R0_RXRDY); n++) {
		rr1 = scc_rreg(sp, 1);
		c = *sp->sc_data;
		SCC_DELAY();
		if (rr1 & (R1_FE|R1_OE|R1_PE)) {
			WCMD(sp, W0_RSTERR);
			if (rr1 & R1_OE) {
				sp->sc_noe++;
				c |= RX_OE;
			}
			if (rr1 & R1_PE) {
				sp->sc_npe++;
				c |= RX_PE;
			}
			if (rr1 & R1_FE) {
				sp->sc_nfe++;
				c |= RX_FE;
			}
		}
		if (sp->sc_flags & SCF_BRKEND) {
			sp->sc_flags &= ~SCF_BRKEND;
			if ((c & 0xFF) == 0)
				continue;
		}
		if (!sp->sc_rq || !(sp->sc_tty.t_cflag & CREAD))
			continue;
		if (((sp->sc_rput + 1) & (RBSZ - 1)) == sp->sc_rget) {
			sp->sc_novf++;
			continue;
		}
		sp->sc_rbuf[sp->sc_rput] = c;
		sp->sc_rput = (sp->sc_rput + 1) & (RBSZ - 1);
		sp->sc_nrx++;
		got = 1;
	}
	if (got) {
		if (RCOUNT(sp) >= RHIWAT && !(sp->sc_flags & SCF_RBLOCK))
			scc_throttle(sp);
		qenable(sp->sc_rq);
	}
}

static void
scc_txint(sp)
register struct scc *sp;
{
	register queue_t *wq;

	sp->sc_flags &= ~SCF_TXBUSY;
	if (!scc_txnext(sp)) {
		WCMD(sp, W0_RSTTXIP);
		sp->sc_tty.t_state &= ~BUSY;
	}
	if (!sp->sc_rq)
		return;
	wq = WR(sp->sc_rq);
	if (sp->sc_tget == sp->sc_tput && !(sp->sc_flags & SCF_TXBUSY)) {
		if (sp->sc_tty.t_state & TTIOW) {
			sp->sc_tty.t_state &= ~TTIOW;
			wakeup((caddr_t)&sp->sc_tty.t_oflag);
		}
		if (sp->sc_flags & SCF_DRAIN) {
			sp->sc_flags &= ~SCF_DRAIN;
			qenable(wq);
		}
	}
	if (wq->q_first && TCOUNT(sp) <= TLOWAT)
		qenable(wq);
}

static void
scc_stint(sp)
register struct scc *sp;
{
	register struct strtty *tp = &sp->sc_tty;
	register int rr0, delta;

	rr0 = RR0(sp);
	WCMD(sp, W0_RSTEXT);
	delta = rr0 ^ sp->sc_rr0;
	sp->sc_rr0 = rr0;

	if (delta & R0_BREAK) {
		if (rr0 & R0_BREAK) {
			sp->sc_nbrk++;
			if (sp->sc_rq &&
			    ((sp->sc_rput + 1) & (RBSZ - 1)) != sp->sc_rget) {
				sp->sc_rbuf[sp->sc_rput] = RX_BRK;
				sp->sc_rput = (sp->sc_rput + 1) & (RBSZ - 1);
				qenable(sp->sc_rq);
			}
		} else
			sp->sc_flags |= SCF_BRKEND;
	}
	if ((delta & R0_DCD) && (sp->sc_minor & SCC_MODEM) &&
	    !(tp->t_cflag & CLOCAL)) {
		if (rr0 & R0_DCD) {
			tp->t_state |= CARR_ON;
			if (tp->t_state & WOPEN)
				wakeup((caddr_t)tp);
		} else if (tp->t_state & CARR_ON) {
			tp->t_state &= ~CARR_ON;
			if (sp->sc_rq) {
				sp->sc_flags |= SCF_HUP;
				qenable(sp->sc_rq);
			}
		}
	}
	if ((delta & R0_CTS) && (sp->sc_minor & SCC_HWFLOW) && CTS_ON(rr0))
		scc_kick(sp);
}

/*
 * Level-4 autovector handler body, called from p4int at IPL 4.
 * Only channel A has RR3.
 */
void
sccintr()
{
	register struct scc *a = &scc_sc[0], *b = &scc_sc[1];
	register int rr3, n;

	scc_nintr++;
	if (!scc_ready) {
		*(VOL unsigned char *)(mac_scc + 2) = 9;
		SCC_DELAY();
		*(VOL unsigned char *)(mac_scc + 2) = 0;
		SCC_DELAY();
		scc_nspur++;
		return;
	}
	for (n = 0; n < 32; n++) {
		rr3 = scc_rreg(a, 3);
		if ((rr3 & (R3_A|R3_B)) == 0)
			break;
		if (rr3 & R3_ARX)
			scc_rxint(a);
		if (rr3 & R3_AEXT)
			scc_stint(a);
		if (rr3 & R3_ATX)
			scc_txint(a);
		if (rr3 & R3_BRX)
			scc_rxint(b);
		if (rr3 & R3_BEXT)
			scc_stint(b);
		if (rr3 & R3_BTX)
			scc_txint(b);
	}
	if (n == 0)
		scc_nspur++;
	WCMD(a, W0_RSTIUS);
}

/* ------------------------------------------------------ open, close */

int
sccopen(rq, devp, flag, sflag, credp)
queue_t *rq;
dev_t *devp;
int flag, sflag;
cred_t *credp;
{
	register struct scc *sp;
	register struct strtty *tp;
	register int m, s, i;
	register mblk_t *mop;
	struct stroptions *sop;

	if (sflag)
		return EINVAL;
	m = getminor(*devp);
	if (m & ~SCC_MINMASK)
		return ENXIO;
	sp = &scc_sc[SCC_CHAN(m)];
	tp = &sp->sc_tty;

	s = scc_spl();
	if (!scc_ready)
		scc_attach();
	if (tp->t_state & (ISOPEN|WOPEN)) {
		if (sp->sc_minor != m) {
			scc_splx(s);
			return EBUSY;
		}
	} else {
		sp->sc_minor = m;
		sp->sc_flags &= SCF_HWINIT;
		sp->sc_rput = sp->sc_rget = 0;
		sp->sc_tput = sp->sc_tget = 0;
		sp->sc_xchar = 0;
		sp->sc_mctl = TIOCM_DTR | TIOCM_RTS;
		tp->t_state = 0;
		tp->t_dev = m;
		tp->t_line = 0;
		tp->t_iflag = ICRNL | IXON | BRKINT | IGNPAR;
		tp->t_oflag = OPOST | ONLCR;
		tp->t_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
		tp->t_cflag = B9600 | CS8 | CREAD;
		tp->t_cflag |= (m & SCC_MODEM) ? HUPCL : CLOCAL;
		for (i = 0; i < NCCS; i++)
			tp->t_cc[i] = scc_ccdef[i];
		scc_param(sp);
		if (!(m & SCC_MODEM) || (tp->t_cflag & CLOCAL) ||
		    (sp->sc_rr0 & R0_DCD))
			tp->t_state |= CARR_ON;
	}

	if (!(flag & (FNDELAY|FNONBLOCK)))
		while (!(tp->t_state & CARR_ON) && !(tp->t_cflag & CLOCAL)) {
			tp->t_state |= WOPEN;
			if (sleep((caddr_t)tp, TTIPRI | PCATCH)) {
				tp->t_state &= ~WOPEN;
				if (!(tp->t_state & ISOPEN))
					scc_shutdown(sp);
				scc_splx(s);
				return EINTR;
			}
		}
	tp->t_state &= ~WOPEN;

	if (tp->t_state & ISOPEN) {
		scc_splx(s);
		return 0;
	}
	if ((mop = allocb(sizeof (struct stroptions), BPRI_MED)) == NULL) {
		scc_shutdown(sp);
		scc_splx(s);
		return EAGAIN;
	}
	rq->q_ptr = WR(rq)->q_ptr = (caddr_t)sp;
	sp->sc_rq = rq;
	tp->t_rdqp = rq;
	tp->t_state |= ISOPEN;
	scc_splx(s);

	mop->b_datap->db_type = M_SETOPTS;
	sop = (struct stroptions *)mop->b_wptr;
	mop->b_wptr += sizeof (struct stroptions);
	sop->so_flags = SO_HIWAT | SO_LOWAT | SO_ISTTY;
	sop->so_hiwat = 512;
	sop->so_lowat = 256;
	putnext(rq, mop);
	return 0;
}

int
sccclose(q, flag, credp)
queue_t *q;
int flag;
cred_t *credp;
{
	register struct scc *sp = (struct scc *)q->q_ptr;
	register struct strtty *tp;
	register int s;

	if (sp == NULL)
		return 0;
	tp = &sp->sc_tty;
	s = scc_spl();
	if (!(flag & (FNDELAY|FNONBLOCK)))
		while ((tp->t_state & CARR_ON) &&
		    (WR(q)->q_first || sp->sc_tget != sp->sc_tput ||
		     (sp->sc_flags & SCF_TXBUSY))) {
			tp->t_state |= TTIOW;
			scc_kick(sp);
			if (sleep((caddr_t)&tp->t_oflag, TTOPRI | PCATCH)) {
				tp->t_state &= ~TTIOW;
				break;
			}
		}
	scc_shutdown(sp);
	scc_splx(s);
	q->q_ptr = WR(q)->q_ptr = NULL;
	return 0;
}

/* Last close or failed open: cancel callbacks, quiet the channel.  IPL 4. */
static void
scc_shutdown(sp)
register struct scc *sp;
{
	register struct strtty *tp = &sp->sc_tty;

	if (sp->sc_tid)
		untimeout(sp->sc_tid);
	if (sp->sc_did)
		untimeout(sp->sc_did);
	if (sp->sc_bid)
		unbufcall(sp->sc_bid);
	sp->sc_tid = sp->sc_did = sp->sc_bid = 0;
	sp->sc_rq = NULL;
	sp->sc_rget = sp->sc_rput;
	sp->sc_tget = sp->sc_tput;
	sp->sc_xchar = 0;
	sp->sc_flags &= SCF_HWINIT;

	scc_wreg(sp, 1, 0);
	scc_wreg(sp, 15, 0);
	WCMD(sp, W0_RSTEXT);
	WCMD(sp, W0_RSTTXIP);
	if (tp->t_cflag & HUPCL)
		sp->sc_mctl &= ~TIOCM_DTR;
	scc_setwr5(sp);

	tp->t_state = 0;
	tp->t_rdqp = NULL;
}

/* ----------------------------------------------------------- read */

/*
 * Read service: carrier loss, then the receive ring into M_DATA
 * blocks (and M_BREAK) as far as upstream flow control allows.
 */
int
sccrsrv(q)
register queue_t *q;
{
	register struct scc *sp = (struct scc *)q->q_ptr;
	register struct strtty *tp;
	register mblk_t *mp;
	register int c, e, s, mask;
	int brk;

	if (sp == NULL)
		return 0;
	tp = &sp->sc_tty;

	if (sp->sc_flags & SCF_HUP) {
		s = scc_spl();
		sp->sc_flags &= ~SCF_HUP;
		scc_splx(s);
		if (!(tp->t_state & CARR_ON)) {
			scc_rflush(sp);
			scc_wflush(sp, WR(q));
			(void) putctl(q->q_next, M_HANGUP);
		}
	}

	switch (tp->t_cflag & CSIZE) {
	case CS5: mask = 0x1F; break;
	case CS6: mask = 0x3F; break;
	case CS7: mask = 0x7F; break;
	default:  mask = 0xFF; break;
	}

	while (sp->sc_rget != sp->sc_rput) {
		if (!canput(q->q_next))
			return 0;
		if ((mp = allocb(RXMSGSZ, BPRI_HI)) == NULL) {
			if (!sp->sc_bid)
				sp->sc_bid = bufcall(RXMSGSZ, BPRI_HI,
				    scc_bufcb, (long)sp);
			return 0;
		}
		brk = 0;
		s = scc_spl();
		while (sp->sc_rget != sp->sc_rput &&
		    mp->b_wptr + 3 <= mp->b_datap->db_lim) {
			e = sp->sc_rbuf[sp->sc_rget];
			if (e & RX_BRK) {
				if (mp->b_wptr == mp->b_rptr) {
					sp->sc_rget = (sp->sc_rget + 1) & (RBSZ - 1);
					brk = 1;
				}
				break;
			}
			sp->sc_rget = (sp->sc_rget + 1) & (RBSZ - 1);
			c = e & mask;
			if ((e & RX_FE) || ((e & RX_PE) && (tp->t_iflag & INPCK))) {
				if (tp->t_iflag & IGNPAR)
					continue;
				if (tp->t_iflag & PARMRK) {
					*mp->b_wptr++ = 0377;
					*mp->b_wptr++ = 0;
					*mp->b_wptr++ = c;
				} else
					*mp->b_wptr++ = 0;
			} else
				*mp->b_wptr++ = c;
		}
		if ((sp->sc_flags & SCF_RBLOCK) && RCOUNT(sp) < RLOWAT)
			scc_unthrottle(sp);
		scc_splx(s);

		if (mp->b_wptr > mp->b_rptr)
			putnext(q, mp);
		else
			freeb(mp);
		if (brk)
			(void) putctl(q->q_next, M_BREAK);
	}
	return 0;
}

/* ---------------------------------------------------------- write */

int
sccwput(q, mp)
register queue_t *q;
register mblk_t *mp;
{
	register struct scc *sp = (struct scc *)q->q_ptr;
	register struct iocblk *iocp;
	register int s;

	switch (mp->b_datap->db_type) {
	case M_DATA:
	case M_DELAY:
	case M_BREAK:
		putq(q, mp);
		break;

	case M_IOCTL:
		iocp = (struct iocblk *)mp->b_rptr;
		switch (iocp->ioc_cmd) {
		case TCGETS: case TCGETA:
		case TCXONC: case TCFLSH:	/* must pass stopped output */
		case TIOCMGET: case TIOCMSET: case TIOCMBIS: case TIOCMBIC:
			scc_ioctl(q, mp);
			break;
		default:
			putq(q, mp);	/* in order with the data */
			break;
		}
		break;

	case M_IOCDATA:
		scc_iocdata(q, mp);
		break;

	case M_FLUSH:
		if (*mp->b_rptr & FLUSHW) {
			scc_wflush(sp, q);
			*mp->b_rptr &= ~FLUSHW;
		}
		if (*mp->b_rptr & FLUSHR) {
			scc_rflush(sp);
			flushq(RD(q), FLUSHDATA);
			qreply(q, mp);
		} else
			freemsg(mp);
		break;

	case M_STOP:
		s = scc_spl();
		sp->sc_tty.t_state |= TTSTOP;
		scc_splx(s);
		freemsg(mp);
		break;

	case M_START:
		s = scc_spl();
		sp->sc_tty.t_state &= ~TTSTOP;
		scc_kick(sp);
		scc_splx(s);
		freemsg(mp);
		qenable(q);		/* the screen waits in wsrv */
		break;

	case M_STOPI:
		s = scc_spl();
		sp->sc_flags |= SCF_IBLOCK;
		sp->sc_xchar = sp->sc_tty.t_cc[VSTOP];
		scc_kick(sp);
		if (sp->sc_minor & SCC_HWFLOW)
			scc_setwr5(sp);
		scc_splx(s);
		freemsg(mp);
		break;

	case M_STARTI:
		s = scc_spl();
		sp->sc_flags &= ~SCF_IBLOCK;
		if (!(sp->sc_flags & SCF_RBLOCK)) {
			sp->sc_xchar = sp->sc_tty.t_cc[VSTART];
			scc_kick(sp);
			if (sp->sc_minor & SCC_HWFLOW)
				scc_setwr5(sp);
		}
		scc_splx(s);
		freemsg(mp);
		break;

	case M_CTL:
		iocp = (struct iocblk *)mp->b_rptr;
		if (mp->b_wptr - mp->b_rptr == sizeof (struct iocblk) &&
		    iocp->ioc_cmd == MC_CANONQUERY) {
			iocp->ioc_cmd = MC_DO_CANON;
			qreply(q, mp);
		} else
			freemsg(mp);
		break;

	default:
		freemsg(mp);
		break;
	}
	return 0;
}

/* Copy data blocks into the transmit ring; returns what did not fit. */
static mblk_t *
scc_txfill(sp, mp)
register struct scc *sp;
register mblk_t *mp;
{
	register mblk_t *nmp;
	register int s, next;

	while (mp) {
		s = scc_spl();
		while (mp->b_rptr < mp->b_wptr) {
			next = (sp->sc_tput + 1) & (TBSZ - 1);
			if (next == sp->sc_tget)
				break;
			sp->sc_tbuf[sp->sc_tput] = *mp->b_rptr++;
			sp->sc_tput = next;
		}
		scc_kick(sp);
		scc_splx(s);
		if (mp->b_rptr < mp->b_wptr)
			return mp;
		nmp = unlinkb(mp);
		freeb(mp);
		mp = nmp;
	}
	return NULL;
}

/*
 * Console output while the screen is the console: draw all of it now and
 * mirror what fits into the SCC ring, dropping and counting the rest, so
 * the serial line never paces the screen.  Frees mp.
 */
static void
scc_conout(sp, mp)
register struct scc *sp;
mblk_t *mp;
{
	register mblk_t *bp;
	register int s, next;

	for (bp = mp; bp; bp = bp->b_cont)
		if (bp->b_wptr > bp->b_rptr)
			fbcons_write(bp->b_rptr, (int)(bp->b_wptr - bp->b_rptr));
	s = scc_spl();
	for (bp = mp; bp; bp = bp->b_cont)
		while (bp->b_rptr < bp->b_wptr) {
			next = (sp->sc_tput + 1) & (TBSZ - 1);
			if (next == sp->sc_tget) {
				scc_nmirdrop += bp->b_wptr - bp->b_rptr;
				break;
			}
			sp->sc_tbuf[sp->sc_tput] = *bp->b_rptr++;
			sp->sc_tput = next;
		}
	scc_kick(sp);
	scc_splx(s);
	freemsg(mp);
}

/*
 * A byte for the console tty from the keyboard (fbcons_input): into
 * channel A's receive ring as if received.  Any IPL.
 */
void
scc_conin(c)
int c;
{
	register struct scc *sp = &scc_sc[0];
	register int s, next;

	s = scc_spl();
	if (sp->sc_rq && (sp->sc_tty.t_cflag & CREAD)) {
		next = (sp->sc_rput + 1) & (RBSZ - 1);
		if (next == sp->sc_rget)
			sp->sc_novf++;
		else {
			sp->sc_rbuf[sp->sc_rput] = c & 0xFF;
			sp->sc_rput = next;
			sp->sc_nrx++;
			qenable(sp->sc_rq);
		}
	}
	scc_splx(s);
}

/* Transmitter and ring empty, last stop bit sent.  IPL 4. */
static int
scc_drained(sp)
register struct scc *sp;
{
	return sp->sc_tget == sp->sc_tput && !(sp->sc_flags & SCF_TXBUSY) &&
	    (scc_rreg(sp, 1) & R1_ALLSENT);
}

/* Arrange for wsrv to run again when the transmitter drains. */
static void
scc_waitdrain(sp)
register struct scc *sp;
{
	register int s = scc_spl();

	if (sp->sc_tget != sp->sc_tput || (sp->sc_flags & SCF_TXBUSY))
		sp->sc_flags |= SCF_DRAIN;
	else if (!sp->sc_did)
		sp->sc_did = timeout(scc_drainto, (caddr_t)sp, 1);
	scc_splx(s);
}

int
sccwsrv(q)
register queue_t *q;
{
	register struct scc *sp = (struct scc *)q->q_ptr;
	register mblk_t *mp;
	register struct iocblk *iocp;
	register int s, ok;

	if (sp == NULL)
		return 0;
	while ((mp = getq(q)) != NULL) {
		if (sp->sc_tty.t_state & TIMEOUT) {
			putbq(q, mp);
			return 0;
		}
		switch (mp->b_datap->db_type) {
		case M_DATA:
			if (sp == &scc_sc[0] && fbcons_active()) {
				if (sp->sc_tty.t_state & TTSTOP) {
					putbq(q, mp);	/* ^S stops the screen too */
					return 0;
				}
				scc_conout(sp, mp);
				break;
			}
			if ((mp = scc_txfill(sp, mp)) != NULL) {
				putbq(q, mp);
				return 0;
			}
			break;

		case M_DELAY:
		case M_BREAK:
			s = scc_spl();
			ok = scc_drained(sp);
			scc_splx(s);
			if (!ok) {
				putbq(q, mp);
				scc_waitdrain(sp);
				return 0;
			}
			s = scc_spl();
			sp->sc_tty.t_state |= TIMEOUT;
			if (mp->b_datap->db_type == M_BREAK) {
				sp->sc_flags |= SCF_BREAK;
				scc_setwr5(sp);
				sp->sc_tid = timeout(scc_brkend, (caddr_t)sp, BRKTICKS);
			} else
				sp->sc_tid = timeout(scc_tmo, (caddr_t)sp,
				    (long)*mp->b_rptr);
			scc_splx(s);
			freemsg(mp);
			break;

		case M_IOCTL:
			iocp = (struct iocblk *)mp->b_rptr;
			switch (iocp->ioc_cmd) {
			case TCSETSW: case TCSETSF:
			case TCSETAW: case TCSETAF:
			case TCSBRK:
				s = scc_spl();
				ok = scc_drained(sp);
				scc_splx(s);
				if (!ok) {
					putbq(q, mp);
					scc_waitdrain(sp);
					return 0;
				}
				break;
			}
			scc_ioctl(q, mp);
			break;

		default:
			freemsg(mp);
			break;
		}
	}
	return 0;
}

static void
scc_rflush(sp)
register struct scc *sp;
{
	register int s = scc_spl();

	sp->sc_rget = sp->sc_rput;
	if (sp->sc_flags & SCF_RBLOCK)
		scc_unthrottle(sp);
	scc_splx(s);
}

static void
scc_wflush(sp, wq)
register struct scc *sp;
queue_t *wq;
{
	register int s;

	flushq(wq, FLUSHDATA);
	s = scc_spl();
	sp->sc_tget = sp->sc_tput;
	if (!(sp->sc_flags & SCF_TXBUSY) &&
	    (sp->sc_tty.t_state & TTIOW)) {
		sp->sc_tty.t_state &= ~TTIOW;
		wakeup((caddr_t)&sp->sc_tty.t_oflag);
	}
	scc_splx(s);
	qenable(wq);
}

/* ------------------------------------------------------- callbacks */

static void
scc_tmo(sp)
register struct scc *sp;
{
	register int s = scc_spl();

	sp->sc_tid = 0;
	sp->sc_tty.t_state &= ~TIMEOUT;
	scc_kick(sp);
	scc_splx(s);
	if (sp->sc_rq)
		qenable(WR(sp->sc_rq));
}

static void
scc_brkend(sp)
register struct scc *sp;
{
	register int s = scc_spl();

	sp->sc_flags &= ~SCF_BREAK;
	scc_setwr5(sp);
	scc_splx(s);
	scc_tmo(sp);
}

static void
scc_drainto(sp)
register struct scc *sp;
{
	sp->sc_did = 0;
	if (sp->sc_rq)
		qenable(WR(sp->sc_rq));
}

static void
scc_bufcb(sp)
register struct scc *sp;
{
	sp->sc_bid = 0;
	if (sp->sc_rq)
		qenable(sp->sc_rq);
}

/* ---------------------------------------------------------- ioctls */

static void
scc_ack(q, mp, count)
queue_t *q;
register mblk_t *mp;
int count;
{
	register struct iocblk *iocp = (struct iocblk *)mp->b_rptr;

	mp->b_datap->db_type = M_IOCACK;
	iocp->ioc_count = count;
	iocp->ioc_error = 0;
	iocp->ioc_rval = 0;
	if (count == 0 && mp->b_cont) {
		freemsg(mp->b_cont);
		mp->b_cont = NULL;
	}
	qreply(q, mp);
}

static void
scc_nak(q, mp, err)
queue_t *q;
register mblk_t *mp;
int err;
{
	register struct iocblk *iocp = (struct iocblk *)mp->b_rptr;

	mp->b_datap->db_type = M_IOCNAK;
	iocp->ioc_count = 0;
	iocp->ioc_error = err;
	if (mp->b_cont) {
		freemsg(mp->b_cont);
		mp->b_cont = NULL;
	}
	qreply(q, mp);
}

/* Transparent ioctl: ask the stream head to copy in size bytes. */
static void
scc_copyin(q, mp, size)
queue_t *q;
register mblk_t *mp;
int size;
{
	register struct copyreq *cqp = (struct copyreq *)mp->b_rptr;
	caddr_t addr;

	if (mp->b_cont == NULL) {
		scc_nak(q, mp, EINVAL);
		return;
	}
	addr = *(caddr_t *)mp->b_cont->b_rptr;
	freemsg(mp->b_cont);
	mp->b_cont = NULL;
	cqp->cq_addr = addr;
	cqp->cq_size = size;
	cqp->cq_flag = 0;
	cqp->cq_private = (mblk_t *)CP_IN;
	mp->b_datap->db_type = M_COPYIN;
	mp->b_wptr = mp->b_rptr + sizeof (struct copyreq);
	qreply(q, mp);
}

/* Transparent ioctl: ask the stream head to copy out dp. */
static void
scc_copyout(q, mp, dp)
queue_t *q;
register mblk_t *mp, *dp;
{
	register struct copyreq *cqp = (struct copyreq *)mp->b_rptr;
	caddr_t addr;

	if (mp->b_cont == NULL) {
		freemsg(dp);
		scc_nak(q, mp, EINVAL);
		return;
	}
	addr = *(caddr_t *)mp->b_cont->b_rptr;
	freemsg(mp->b_cont);
	mp->b_cont = dp;
	cqp->cq_addr = addr;
	cqp->cq_size = dp->b_wptr - dp->b_rptr;
	cqp->cq_flag = 0;
	cqp->cq_private = (mblk_t *)CP_OUT;
	mp->b_datap->db_type = M_COPYOUT;
	mp->b_wptr = mp->b_rptr + sizeof (struct copyreq);
	qreply(q, mp);
}

/* Apply a termio or termios block.  IPL 4. */
static void
scc_setattr(sp, cmd, data)
register struct scc *sp;
int cmd;
caddr_t data;
{
	register struct strtty *tp = &sp->sc_tty;
	register struct termio *to;
	register struct termios *ts;
	register int i;

	switch (cmd) {
	case TCSETA: case TCSETAW: case TCSETAF:
		to = (struct termio *)data;
		tp->t_iflag = (tp->t_iflag & 0xFFFF0000) | to->c_iflag;
		tp->t_oflag = (tp->t_oflag & 0xFFFF0000) | to->c_oflag;
		tp->t_cflag = (tp->t_cflag & 0xFFFF0000) | to->c_cflag;
		tp->t_lflag = (tp->t_lflag & 0xFFFF0000) | to->c_lflag;
		for (i = 0; i < NCC; i++)
			tp->t_cc[i] = to->c_cc[i];
		break;
	default:
		ts = (struct termios *)data;
		tp->t_iflag = ts->c_iflag;
		tp->t_oflag = ts->c_oflag;
		tp->t_cflag = ts->c_cflag;
		tp->t_lflag = ts->c_lflag;
		for (i = 0; i < NCCS; i++)
			tp->t_cc[i] = ts->c_cc[i];
		break;
	}
	if (cmd == TCSETAF || cmd == TCSETSF) {
		sp->sc_rget = sp->sc_rput;
		if (sp->sc_flags & SCF_RBLOCK)
			scc_unthrottle(sp);
	}
	scc_param(sp);
	if (!(sp->sc_minor & SCC_MODEM) || (tp->t_cflag & CLOCAL) ||
	    (sp->sc_rr0 & R0_DCD)) {
		tp->t_state |= CARR_ON;
		if (tp->t_state & WOPEN)
			wakeup((caddr_t)tp);
	}
}

static void
scc_getattr(q, mp)
queue_t *q;
register mblk_t *mp;
{
	register struct scc *sp = (struct scc *)q->q_ptr;
	register struct strtty *tp = &sp->sc_tty;
	register struct iocblk *iocp = (struct iocblk *)mp->b_rptr;
	register mblk_t *dp;
	register struct termios *ts;
	register struct termio *to;
	register int i, size;

	size = iocp->ioc_cmd == TCGETS ?
	    sizeof (struct termios) : sizeof (struct termio);
	if ((dp = allocb(size, BPRI_MED)) == NULL) {
		scc_nak(q, mp, EAGAIN);
		return;
	}
	if (iocp->ioc_cmd == TCGETS) {
		ts = (struct termios *)dp->b_wptr;
		ts->c_iflag = tp->t_iflag;
		ts->c_oflag = tp->t_oflag;
		ts->c_cflag = tp->t_cflag;
		ts->c_lflag = tp->t_lflag;
		for (i = 0; i < NCCS; i++)
			ts->c_cc[i] = tp->t_cc[i];
	} else {
		to = (struct termio *)dp->b_wptr;
		to->c_iflag = tp->t_iflag;
		to->c_oflag = tp->t_oflag;
		to->c_cflag = tp->t_cflag;
		to->c_lflag = tp->t_lflag;
		to->c_line = tp->t_line;
		for (i = 0; i < NCC; i++)
			to->c_cc[i] = tp->t_cc[i];
	}
	dp->b_wptr += size;
	if (iocp->ioc_count == TRANSPARENT) {
		scc_copyout(q, mp, dp);
		return;
	}
	if (mp->b_cont)
		freemsg(mp->b_cont);
	mp->b_cont = dp;
	scc_ack(q, mp, size);
}

/* Modem-control bits: TIOCMGET, TIOCMSET, TIOCMBIS, TIOCMBIC. */
static void
scc_mctl(q, mp)
queue_t *q;
register mblk_t *mp;
{
	register struct scc *sp = (struct scc *)q->q_ptr;
	register struct iocblk *iocp = (struct iocblk *)mp->b_rptr;
	register mblk_t *dp;
	register int bits, s;

	if (iocp->ioc_cmd == TIOCMGET) {
		if ((dp = allocb(sizeof (int), BPRI_MED)) == NULL) {
			scc_nak(q, mp, EAGAIN);
			return;
		}
		s = scc_spl();
		sp->sc_rr0 = RR0(sp);
		bits = TIOCM_LE;
		if (sp->sc_wr[5] & W5_DTR)
			bits |= TIOCM_DTR;
		if (sp->sc_wr[5] & W5_RTS)
			bits |= TIOCM_RTS;
		if (CTS_ON(sp->sc_rr0))
			bits |= TIOCM_CTS;
		if (sp->sc_rr0 & R0_DCD)
			bits |= TIOCM_CAR;
		scc_splx(s);
		*(int *)dp->b_wptr = bits;
		dp->b_wptr += sizeof (int);
		if (iocp->ioc_count == TRANSPARENT) {
			scc_copyout(q, mp, dp);
			return;
		}
		if (mp->b_cont)
			freemsg(mp->b_cont);
		mp->b_cont = dp;
		scc_ack(q, mp, sizeof (int));
		return;
	}
	if (iocp->ioc_count == TRANSPARENT) {
		scc_copyin(q, mp, sizeof (int));
		return;
	}
	if (mp->b_cont == NULL ||
	    mp->b_cont->b_wptr - mp->b_cont->b_rptr < sizeof (int)) {
		scc_nak(q, mp, EINVAL);
		return;
	}
	bits = *(int *)mp->b_cont->b_rptr & (TIOCM_DTR|TIOCM_RTS);
	s = scc_spl();
	switch (iocp->ioc_cmd) {
	case TIOCMSET: sp->sc_mctl = bits; break;
	case TIOCMBIS: sp->sc_mctl |= bits; break;
	case TIOCMBIC: sp->sc_mctl &= ~bits; break;
	}
	scc_setwr5(sp);
	scc_splx(s);
	scc_ack(q, mp, 0);
}

/* M_IOCTL taken off the write queue (after draining, when required). */
static void
scc_ioctl(q, mp)
queue_t *q;
register mblk_t *mp;
{
	register struct scc *sp = (struct scc *)q->q_ptr;
	register struct iocblk *iocp = (struct iocblk *)mp->b_rptr;
	register int s, arg, size;

	switch (iocp->ioc_cmd) {
	case TCSETS: case TCSETSW: case TCSETSF:
		size = sizeof (struct termios);
		goto set;
	case TCSETA: case TCSETAW: case TCSETAF:
		size = sizeof (struct termio);
	set:
		if (iocp->ioc_count == TRANSPARENT) {
			scc_copyin(q, mp, size);
			return;
		}
		if (mp->b_cont == NULL ||
		    mp->b_cont->b_wptr - mp->b_cont->b_rptr < size) {
			scc_nak(q, mp, EINVAL);
			return;
		}
		s = scc_spl();
		scc_setattr(sp, iocp->ioc_cmd, (caddr_t)mp->b_cont->b_rptr);
		scc_splx(s);
		scc_ack(q, mp, 0);
		return;

	case TCGETS:
	case TCGETA:
		scc_getattr(q, mp);
		return;

	case TIOCMGET: case TIOCMSET: case TIOCMBIS: case TIOCMBIC:
		scc_mctl(q, mp);
		return;
	}

	/* the rest take an int argument by value */
	if (mp->b_cont == NULL ||
	    mp->b_cont->b_wptr - mp->b_cont->b_rptr < sizeof (int)) {
		if ((iocp->ioc_cmd & IOCTYPE) == LDIOC)
			scc_ack(q, mp, 0);
		else
			scc_nak(q, mp, EINVAL);
		return;
	}
	arg = *(int *)mp->b_cont->b_rptr;

	switch (iocp->ioc_cmd) {
	case TCSBRK:
		if (arg == 0) {
			s = scc_spl();
			sp->sc_tty.t_state |= TIMEOUT;
			sp->sc_flags |= SCF_BREAK;
			scc_setwr5(sp);
			sp->sc_tid = timeout(scc_brkend, (caddr_t)sp, BRKTICKS);
			scc_splx(s);
		}
		scc_ack(q, mp, 0);
		return;

	case TCXONC:
		s = scc_spl();
		switch (arg) {
		case TCOOFF:
			sp->sc_tty.t_state |= TTSTOP;
			break;
		case TCOON:
			sp->sc_tty.t_state &= ~TTSTOP;
			scc_kick(sp);
			qenable(q);
			break;
		case TCIOFF:
			sp->sc_flags |= SCF_IBLOCK;
			sp->sc_xchar = sp->sc_tty.t_cc[VSTOP];
			scc_kick(sp);
			scc_setwr5(sp);
			break;
		case TCION:
			sp->sc_flags &= ~SCF_IBLOCK;
			sp->sc_xchar = sp->sc_tty.t_cc[VSTART];
			scc_kick(sp);
			scc_setwr5(sp);
			break;
		default:
			scc_splx(s);
			scc_nak(q, mp, EINVAL);
			return;
		}
		scc_splx(s);
		scc_ack(q, mp, 0);
		return;

	case TCFLSH:
		switch (arg) {
		case TCIFLUSH:
			scc_rflush(sp);
			break;
		case TCOFLUSH:
			scc_wflush(sp, q);
			break;
		case TCIOFLUSH:
			scc_rflush(sp);
			scc_wflush(sp, q);
			break;
		default:
			scc_nak(q, mp, EINVAL);
			return;
		}
		scc_ack(q, mp, 0);
		return;
	}

	if ((iocp->ioc_cmd & IOCTYPE) == LDIOC)
		scc_ack(q, mp, 0);
	else
		scc_nak(q, mp, EINVAL);
}

/* Reply to our M_COPYIN / M_COPYOUT. */
static void
scc_iocdata(q, mp)
queue_t *q;
register mblk_t *mp;
{
	register struct scc *sp = (struct scc *)q->q_ptr;
	register struct copyresp *csp = (struct copyresp *)mp->b_rptr;
	register mblk_t *dp = mp->b_cont;
	register int s, bits;

	if (csp->cp_rval) {
		freemsg(mp);
		return;
	}
	if (csp->cp_private == (mblk_t *)CP_OUT) {
		scc_ack(q, mp, 0);
		return;
	}
	switch (csp->cp_cmd) {
	case TCSETS: case TCSETSW: case TCSETSF:
	case TCSETA: case TCSETAW: case TCSETAF:
		bits = (csp->cp_cmd == TCSETS || csp->cp_cmd == TCSETSW ||
		    csp->cp_cmd == TCSETSF) ?
		    sizeof (struct termios) : sizeof (struct termio);
		if (dp == NULL || dp->b_wptr - dp->b_rptr < bits) {
			scc_nak(q, mp, EINVAL);
			return;
		}
		s = scc_spl();
		scc_setattr(sp, csp->cp_cmd, (caddr_t)dp->b_rptr);
		scc_splx(s);
		scc_ack(q, mp, 0);
		return;

	case TIOCMSET: case TIOCMBIS: case TIOCMBIC:
		if (dp == NULL || dp->b_wptr - dp->b_rptr < sizeof (int)) {
			scc_nak(q, mp, EINVAL);
			return;
		}
		bits = *(int *)dp->b_rptr & (TIOCM_DTR|TIOCM_RTS);
		s = scc_spl();
		if (csp->cp_cmd == TIOCMSET)
			sp->sc_mctl = bits;
		else if (csp->cp_cmd == TIOCMBIS)
			sp->sc_mctl |= bits;
		else
			sp->sc_mctl &= ~bits;
		scc_setwr5(sp);
		scc_splx(s);
		scc_ack(q, mp, 0);
		return;
	}
	scc_nak(q, mp, EINVAL);
}
