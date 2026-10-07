/*
 * DaynaPORT SCSI/Link protocol.  Six-byte CDBs, length in bytes 3-4:
 *
 *   08 read     byte 5 = c0 multi-packet, 80 one frame;
 *               in: [len16][flags32] frame+CRC, len 0 = nothing queued;
 *               flags 00000010 = another frame follows;
 *               flags FFFFFFFF = the adapter dropped a frame and stays
 *               wedged until disabled and enabled again
 *   09 stats    in: MAC[6] + three counters
 *   0A write    out: one raw frame (byte 5 = 0)
 *   0C mode     byte 4 = 04, byte 5 = 80: receive broadcasts (old ROMs)
 *   0D addmc    out: one multicast address
 *   0E enable   byte 5 = 80 enable, 00 disable
 *
 * The real ROM refuses data commands while disabled and for about half
 * a second after ENABLE; the emulations accept them at once.
 */
#include "dp.h"

#define S_GOOD		SS_GOOD
#define S_CHECK		SS_CHECK

static int
dp_eq(a, b, n)
unsigned char *a;
char *b;
int n;
{
	while (n-- > 0)
		if (*a++ != (unsigned char)*b++)
			return 0;
	return 1;
}

static void
dp_cdb(c, op, len, ctl)
unsigned char *c;
int op, len, ctl;
{
	c[0] = op;
	c[1] = c[2] = 0;
	c[3] = len >> 8;
	c[4] = len;
	c[5] = ctl;
}

/*
 * One command, run now.  CHECK CONDITION comes with the sense
 * (an illegal-request key means the adapter wants re-enabling); a
 * stalled bus was reset.  Failures outside the settle window count
 * towards a re-enable.
 */
static int
dp_job(j, id, op, len, ctl, buf, dir)
struct scsi_job *j;
int id, op, len, ctl, dir;
unsigned char *buf;
{
	bzero((char *)j, sizeof *j);
	j->sj_target = id;
	dp_cdb(j->sj_cdb, op, len, ctl);
	j->sj_cdblen = 6;
	j->sj_data = buf;
	j->sj_len = len;
	j->sj_dir = dir;
	return scsi_run(j);
}

static int
dp_cmd(sc, op, len, ctl, buf, dir, done)
struct dp_softc *sc;
int op, len, ctl, dir, *done;
unsigned char *buf;
{
	struct scsi_job j;
	int r;

	r = dp_job(&j, sc->id, op, len, ctl, buf, dir);
	*done = j.sj_done;
	if (r == S_GOOD) {
		sc->errs = 0;
		return 0;
	}
	if (r == FS_DEFER)	/* the bus is busy: not the adapter's fault */
		return -1;
	sc->st.cmderr++;
	if (r == S_CHECK) {
		if (j.sj_senselen >= 3 && (j.sj_sense[2] & 0x0f) == SK_ILLEGAL &&
		    DP_NOW() - sc->settle >= 0)
			sc->reinit = 1;
	} else if (r == FS_BUSERR) {
		sc->st.busreset++;
		sc->reinit = 1;
	}
	if (DP_NOW() - sc->settle >= 0 && ++sc->errs >= DP_MAXERRS)
		sc->reinit = 1;
	return -1;
}

/* Find the first DaynaPORT on the bus; INQUIRY on IDs 0-6. */
int
dp_probe(sc)
struct dp_softc *sc;
{
	unsigned char inq[36];
	struct scsi_job j;
	int id;

	sc->id = -1;
	if (scsi_attach() < 0)
		return -1;
	for (id = 0; id < SC_MYID; id++) {
		if (dp_job(&j, id, 0x12, (int)sizeof(inq), 0, inq, FS_IN) != S_GOOD ||
		    j.sj_done < 32 || (inq[0] & 0x1f) != 0x03)
			continue;
		if (dp_eq(inq + 8, "Dayna", 5) && dp_eq(inq + 16, "SCSI/Link", 9)) {
			sc->id = id;
			bcopy((char *)inq + 32, (char *)sc->rev, 4);
			return 0;
		}
	}
	return -1;
}

static int
dp_enable(sc, on)
struct dp_softc *sc;
int on;
{
	int n;

	return dp_cmd(sc, DPC_ENABLE, 0, on ? 0x80 : 0, sc->rxbuf, FS_NONE, &n);
}

/*
 * Enable, then read the address until the adapter accepts it (up to a
 * second).  The ROM sends 22 bytes, the emulations 18; an emulation that
 * checks the length refuses 22, so 18 is tried when 22 fails.
 */
int
dp_init(sc)
struct dp_softc *sc;
{
	int t, n;

	sc->running = 0;
	if (sc->id < 0 || dp_enable(sc, 1) < 0)
		return -1;
	sc->settle = DP_NOW() + DP_SETTLE;
	for (t = 0;; t++) {
		if ((dp_cmd(sc, DPC_STATS, 22, 0, sc->rxbuf, FS_IN, &n) == 0 ||
		    dp_cmd(sc, DPC_STATS, 18, 0, sc->rxbuf, FS_IN, &n) == 0) &&
		    n >= 6)
			break;
		if (t >= 2 * DP_SETTLE) {
			(void)dp_enable(sc, 0);
			return -1;
		}
		DP_PAUSE();
	}
	bcopy((char *)sc->rxbuf, (char *)sc->ea, 6);
	sc->errs = sc->reinit = 0;
	sc->bcast = 1;
	sc->mcsent = 0;
	sc->running = 1;
	return 0;
}

void
dp_stop(sc)
struct dp_softc *sc;
{
	if (sc->id >= 0)
		(void)dp_enable(sc, 0);
	sc->running = 0;
}

/*
 * Housekeeping before frames move: a due re-enable (then the settle
 * window passes with the bus left alone), the broadcast mode, and
 * multicast addresses not yet sent.
 */
int
dp_ready(sc)
struct dp_softc *sc;
{
	int n;

	if (!sc->running || DP_NOW() - sc->settle < 0)
		return 0;
	if (sc->reinit) {
		sc->reinit = 0;
		sc->st.reinit++;
		(void)dp_enable(sc, 0);
		/* emulations accept reads while disabled: retry until enabled */
		if (dp_enable(sc, 1) < 0)
			sc->reinit = 1;
		sc->settle = DP_NOW() + DP_SETTLE;
		sc->errs = 0;
		sc->bcast = 1;
		sc->mcsent = 0;
		return 0;
	}
	if (sc->bcast) {
		sc->bcast = 0;
		(void)dp_cmd(sc, DPC_MODE, 4, 0x80, sc->rxbuf, FS_NONE, &n);
	}
	while (sc->mcsent < sc->nmc && !sc->reinit)
		(void)dp_cmd(sc, DPC_ADDMC, 6, 0, sc->mc[sc->mcsent++], FS_OUT, &n);
	return !sc->reinit;
}

int
dp_addmc(sc, a)
struct dp_softc *sc;
unsigned char *a;
{
	int i;

	for (i = 0; i < sc->nmc; i++)
		if (dp_eq(sc->mc[i], (char *)a, 6))
			return 0;
	if (sc->nmc == DP_NMC)
		return -1;
	bcopy((char *)a, (char *)sc->mc[sc->nmc++], 6);
	return 0;
}

int
dp_send(sc, len)
struct dp_softc *sc;
int len;
{
	int n;

	while (len < DP_MINFRAME)
		sc->txbuf[len++] = 0;
	if (dp_cmd(sc, DPC_WRITE, len, 0, sc->txbuf, FS_OUT, &n) < 0 || n != len) {
		sc->st.oerrors++;
		return -1;
	}
	sc->st.opackets++;
	return 0;
}

/*
 * One READ.  The adapter returns up to two frames per command, each
 * behind a header whose last byte has 0x10 set when another follows;
 * what was actually moved bounds the parse.
 */
int
dp_recv(sc)
struct dp_softc *sc;
{
	unsigned char *h;
	int got, n, off, frames;

	sc->st.polls++;
	if (dp_cmd(sc, DPC_READ, DP_RXASK, DP_RXCTL, sc->rxbuf, FS_IN, &got) < 0) {
		sc->st.ierrors++;
		return -1;
	}
	for (off = frames = 0; off + 6 <= got; off += 6 + n) {
		h = sc->rxbuf + off;
		n = h[0] << 8 | h[1];
		if (h[2] == 0xff && h[3] == 0xff && h[4] == 0xff && h[5] == 0xff) {
			sc->st.wedged++;
			sc->reinit = 1;
			break;
		}
		if (n == 0)
			break;
		if (n < 14 + 4 || n > DP_MAXFRAME + 4 || off + 6 + n > got) {
			sc->st.rxbad++;
			break;
		}
		sc->st.ipackets++;
		frames++;
		dp_input(sc, h + 6, n - 4);
	}
	if (frames == 0)
		sc->st.empty++;
	return frames;
}
