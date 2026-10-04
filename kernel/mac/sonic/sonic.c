/*
 * DP83932 SONIC core for the Quadra 800 on-board Ethernet.
 *
 * 32-bit bus mode.  One 4 KB block holds every descriptor area (the
 * chip takes the upper 16 address bits of each area from a register,
 * so no area may cross a 64 KB boundary).  Frames are copied: each
 * transmit descriptor owns one buffer, and each RBA holds exactly one
 * received frame, because the RBA is shorter than EOBC plus the
 * smallest frame.
 *
 * Receive:  RDA ring with in_use ownership and a moving end-of-list;
 *           the RRA maps slot n to buffer n for good, and RWP trails the
 *           last buffer consumed, which stays back as the guard slot.
 * Transmit: TDA ring, the newest descriptor carries end-of-list; a
 *           new frame clears it on its predecessor and sets TXP.
 */
#include "sonic.h"

#define LO(a)	((a) & 0xffff)
#define HI(a)	(((a) >> 16) & 0xffff)

static void sn_rxintr(), sn_txreclaim();

static void
sn_bzero(p, n)
register unsigned char *p;
register int n;
{
	while (n-- > 0)
		*p++ = 0;
}

int
sn_attach(sc, pool)
register struct sn_softc *sc;
unsigned char *pool;
{
	register unsigned char *p;

	p = pool + ((0 - SN_PA(pool)) & (SN_CTLSZ - 1));
	sn_bzero(p, SN_CTLSZ);
	sc->tda = (struct sn_tda *)p;
	sc->rda = (struct sn_rda *)(p + SN_NTX * sizeof(struct sn_tda));
	sc->rra = (struct sn_rra *)((unsigned char *)sc->rda +
	    SN_NRX * sizeof(struct sn_rda));
	sc->cda = (struct sn_cda *)((unsigned char *)sc->rra +
	    SN_NRX * sizeof(struct sn_rra));
	if ((unsigned char *)(sc->cda + SN_NCAM + 1) > p + SN_CTLSZ)
		return -1;
	sc->txbuf = p + SN_CTLSZ;
	sc->rxbuf = sc->txbuf + SN_NTX * SN_BUFSZ;
	sc->running = 0;
	sc->txcnt = 0;
	sc->rcr = RCR_BRD;
	sc->camvalid = 1;
	return 0;
}

/* Wait for bits in CR to clear, about 2 us per try. */
static int
sn_crwait(bits, tries)
int bits, tries;
{
	while (tries-- > 0) {
		if ((SN_RD(SN_CR) & bits) == 0)
			return 0;
		SN_DELAY(2);
	}
	return -1;
}

/*
 * Load the valid CAM entries.  The enable mask follows the last
 * descriptor used.  The receiver must be off.
 */
int
sn_camload(sc)
register struct sn_softc *sc;
{
	register struct sn_cda *c;
	register unsigned char *a;
	register int k, n;

	c = sc->cda;
	n = 0;
	for (k = 0; k < SN_NCAM; k++) {
		if ((sc->camvalid & (1 << k)) == 0)
			continue;
		a = sc->cam[k];
		c[n].entry = k;
		c[n].port0 = a[0] | a[1] << 8;
		c[n].port1 = a[2] | a[3] << 8;
		c[n].port2 = a[4] | a[5] << 8;
		n++;
	}
	c[n].entry = sc->camvalid;
	SN_PUSH(c, (n + 1) * sizeof(struct sn_cda));
	SN_SYNC();
	SN_WR(SN_CDP, LO(SN_PA(c)));
	SN_WR(SN_CDC, n);
	SN_WR(SN_CR, CR_LCAM);
	if (sn_crwait(CR_LCAM, 10000) < 0) {
		sc->st.camfail++;
		return -1;
	}
	SN_WR(SN_ISR, INT_LCD);
	return 0;
}

/* New CAM contents or RCR on a running chip: receiver off around the load. */
int
sn_filter(sc)
register struct sn_softc *sc;
{
	int r;

	if (!sc->running)
		return 0;
	SN_WR(SN_CR, CR_RXDIS);
	(void)sn_crwait(CR_RXEN, 1000);
	r = sn_camload(sc);
	SN_WR(SN_RCR, sc->rcr);
	SN_WR(SN_CR, CR_RXEN);
	return r;
}

void
sn_stop(sc)
register struct sn_softc *sc;
{
	SN_WR(SN_IMR, 0);
	SN_WR(SN_CR, CR_HTX | CR_RXDIS | CR_STP);
	(void)sn_crwait(CR_TXP | CR_RXEN | CR_ST, 1000);
	SN_WR(SN_ISR, INT_ALL);
	sc->running = 0;
	sc->st.oerrors += sc->txcnt;
	sc->txcnt = 0;
}

int
sn_init(sc)
register struct sn_softc *sc;
{
	register int i;
	register sn_u32 a;

	sn_stop(sc);
	SN_WR(SN_CR, CR_RST);
	SN_DELAY(1000);
	SN_WR(SN_DCR, SN_DCR_Q800);
	SN_WR(SN_DCR2, 0);
	SN_WR(SN_ISR, INT_ALL);
	SN_WR(SN_CR, 0);
	SN_DELAY(1000);

	/* transmit ring: every descriptor links on and ends the list */
	for (i = 0; i < SN_NTX; i++) {
		sn_bzero((unsigned char *)&sc->tda[i], sizeof(struct sn_tda));
		sc->tda[i].link = LO(SN_PA(&sc->tda[SN_NEXTTX(i)])) | LINK_EOL;
	}
	sc->txhead = sc->txtail = sc->txcnt = sc->txidle = 0;

	/* receive ring: all descriptors free, the last one ends the list */
	for (i = 0; i < SN_NRX; i++) {
		a = SN_PA(sc->rxbuf + i * SN_BUFSZ);
		sc->rra[i].ptr0 = LO(a);
		sc->rra[i].ptr1 = HI(a);
		sc->rra[i].wc0 = SN_BUFSZ / 2;
		sc->rra[i].wc1 = 0;
		sn_bzero((unsigned char *)&sc->rda[i], sizeof(struct sn_rda));
		sc->rda[i].link = LO(SN_PA(&sc->rda[SN_NEXTRX(i)])) |
		    (i == SN_NRX - 1 ? LINK_EOL : 0);
		sc->rda[i].inuse = 1;
	}
	sc->rxnext = 0;
	SN_PUSH(sc->tda, SN_CTLSZ);
	SN_SYNC();

	a = SN_PA(sc->tda);
	SN_WR(SN_UTDA, HI(a));
	SN_WR(SN_CTDA, LO(a));
	a = SN_PA(sc->rda);
	SN_WR(SN_URDA, HI(a));
	SN_WR(SN_CRDA, LO(a));
	a = SN_PA(sc->rra);
	SN_WR(SN_URRA, HI(a));
	SN_WR(SN_RSA, LO(a));
	SN_WR(SN_REA, LO(SN_PA(&sc->rra[SN_NRX])));
	SN_WR(SN_RRP, LO(a));
	SN_WR(SN_RWP, LO(SN_PA(&sc->rra[SN_NRX - 1])));
	SN_WR(SN_EOBC, SN_EOBCVAL);
	SN_WR(SN_RSC, 0);
	SN_WR(SN_CRCT, 0xffff);
	SN_WR(SN_FAET, 0xffff);
	SN_WR(SN_MPT, 0xffff);

	if (sn_camload(sc) < 0)
		return -1;
	SN_WR(SN_RCR, sc->rcr);
	SN_WR(SN_CR, CR_RRRA);
	if (sn_crwait(CR_RRRA, 1000) < 0)
		return -1;
	SN_WR(SN_ISR, INT_ALL);
	SN_WR(SN_IMR, SN_IMRVAL);
	SN_WR(SN_CR, CR_RXEN);
	sc->running = 1;
	return 0;
}

int
sn_txroom(sc)
struct sn_softc *sc;
{
	return sc->running && sc->txcnt < SN_NTX - 1;
}

unsigned char *
sn_txbuf(sc)
struct sn_softc *sc;
{
	return sc->txbuf + sc->txhead * SN_BUFSZ;
}

/* The frame is in sn_txbuf(sc); len excludes the CRC. */
void
sn_txstart(sc, len)
register struct sn_softc *sc;
register int len;
{
	register struct sn_tda *t, *p;
	register unsigned char *b;
	register int i;
	sn_u32 a;

	i = sc->txhead;
	b = sc->txbuf + i * SN_BUFSZ;
	if (len < SN_MINFRAME) {
		sn_bzero(b + len, SN_MINFRAME - len);
		len = SN_MINFRAME;
	}
	t = &sc->tda[i];
	a = SN_PA(b);
	t->status = 0;
	t->config = 0;
	t->size = len;
	t->nfrag = 1;
	t->fptr0 = LO(a);
	t->fptr1 = HI(a);
	t->fsize = len;
	t->link = LO(SN_PA(&sc->tda[SN_NEXTTX(i)])) | LINK_EOL;
	SN_PUSH(b, len);
	SN_PUSH(t, sizeof(struct sn_tda));
	p = &sc->tda[(i - 1) & (SN_NTX - 1)];
	p->link &= ~LINK_EOL;
	SN_PUSH(&p->link, sizeof(p->link));
	SN_SYNC();
	sc->txhead = SN_NEXTTX(i);
	if (sc->txcnt++ == 0)
		sc->txidle = 0;
	SN_WR(SN_CR, CR_TXP);
}

static void
sn_txreclaim(sc)
register struct sn_softc *sc;
{
	register struct sn_tda *t;
	register unsigned int s;
	int n;

	for (n = 0; sc->txcnt > 0; n++) {
		t = &sc->tda[sc->txtail];
		SN_INVAL(t, sizeof(struct sn_tda));
		s = t->status & 0xffff;
		if ((s & ~TCR_CFG) == 0)
			break;
		if (s & TCR_PTX)
			sc->st.opackets++;
		else {
			sc->st.oerrors++;
			if (s & TCR_FU)
				sc->st.txfu++;
			if (s & TCR_EXC)
				sc->st.txexc++;
			if (s & (TCR_CRSL | TCR_NCRS))
				sc->st.txcarrier++;
		}
		sc->st.collisions += TCR_NCOL(s);
		sc->txtail = SN_NEXTTX(sc->txtail);
		sc->txcnt--;
	}
	if (n) {
		sc->txidle = 0;
		sn_txdone(sc);
	}
	/* an error stops the transmitter; so can a TXP lost to a race */
	if (sc->txcnt > 0 && (SN_RD(SN_CR) & CR_TXP) == 0) {
		sc->st.txkick++;
		SN_WR(SN_CR, CR_TXP);
	}
}

static void
sn_rxintr(sc)
register struct sn_softc *sc;
{
	register struct sn_rda *r, *p;
	register unsigned int st, cnt;
	sn_u32 a, off;
	int b, last, n;

	last = -1;
	for (n = 0; n < SN_NRX; n++) {
		r = &sc->rda[sc->rxnext];
		SN_INVAL(r, sizeof(struct sn_rda));
		if (r->inuse & 0xffff)
			break;
		st = r->status & 0xffff;
		cnt = r->count & 0xffff;
		a = (r->ptr1 & 0xffff) << 16 | (r->ptr0 & 0xffff);
		off = a - SN_PA(sc->rxbuf);
		b = off / SN_BUFSZ;
		if (a < SN_PA(sc->rxbuf) || b >= SN_NRX ||
		    off - b * SN_BUFSZ + cnt > SN_BUFSZ) {
			sc->st.rxbad++;
			sc->st.ierrors++;
		} else {
			last = b;
			if ((st & RCR_PRX) && cnt >= 14 + 4 &&
			    cnt <= SN_MAXFRAME + 4) {
				SN_INVAL(SN_VA(a), cnt);
				sc->st.ipackets++;
				sn_input(sc, SN_VA(a), (int)cnt - 4, st);
			} else {
				sc->st.ierrors++;
				if (st & RCR_CRCR)
					sc->st.crc++;
				if (st & RCR_FAER)
					sc->st.fae++;
			}
		}
		/* back to the chip as the new end of the list */
		r->status = r->count = r->seq = 0;
		r->link = LO(SN_PA(&sc->rda[SN_NEXTRX(sc->rxnext)])) | LINK_EOL;
		r->inuse = 1;
		SN_PUSH(r, sizeof(struct sn_rda));
		p = &sc->rda[SN_PREVRX(sc->rxnext)];
		p->link &= ~LINK_EOL;
		SN_PUSH(&p->link, sizeof(p->link));
		sc->rxnext = SN_NEXTRX(sc->rxnext);
	}
	if (last >= 0) {
		SN_SYNC();
		SN_WR(SN_RWP, LO(SN_PA(&sc->rra[last])));
	}
}

int
sn_intr(sc)
register struct sn_softc *sc;
{
	register unsigned int isr;
	int k, handled;

	handled = 0;
	for (k = 0; k < 16; k++) {
		isr = SN_RD(SN_ISR) & SN_IMRVAL;
		if (isr == 0)
			break;
		handled = 1;
		sc->st.intr++;
		/* RBE and RDE are cleared only after the rings are refilled */
		SN_WR(SN_ISR, isr & ~(INT_RBE | INT_RDE));
		if (isr & INT_RFO)
			sc->st.rfo++;
		if (isr & INT_RBAE) {
			sc->st.rbae++;
			sc->st.ierrors++;
		}
		if (isr & (INT_PRX | INT_RBE | INT_RDE | INT_RBAE | INT_RFO))
			sn_rxintr(sc);
		if (isr & (INT_RBE | INT_RDE)) {
			if (isr & INT_RBE)
				sc->st.rbe++;
			if (isr & INT_RDE)
				sc->st.rde++;
			SN_SYNC();
			SN_WR(SN_ISR, isr & (INT_RBE | INT_RDE));
		}
		if (isr & (INT_PTX | INT_TXER))
			sn_txreclaim(sc);
	}
	return handled;
}

/*
 * Once a second: service what a lost interrupt edge left behind, kick
 * a stalled transmitter, and reset a chip that stopped or failed to
 * start.  1 if reset, -1 if the reset failed.
 */
int
sn_watch(sc)
register struct sn_softc *sc;
{
	if (!sc->running) {
		sc->st.reinit++;
		return sn_init(sc) == 0 ? 1 : -1;
	}
	(void)sn_intr(sc);
	if ((SN_RD(SN_CR) & CR_RXEN) == 0 ||
	    (sc->txcnt > 0 && ++sc->txidle >= 5)) {
		sc->st.reinit++;
		return sn_init(sc) == 0 ? 1 : -1;
	}
	if (sc->txcnt > 0 && sc->txidle >= 2) {
		sc->st.txkick++;
		SN_WR(SN_CR, CR_TXP);
	}
	return 0;
}
