/*
 * NCR 5380 command path: arbitration, selection with ATN and an
 * IDENTIFY message (no disconnect), then byte-by-byte REQ/ACK for every
 * phase the target asks for.  Runs at whatever IPL the caller has; a
 * caller below IPL 6 keeps the clock and keyboard running.
 */
#include "ncr5380.h"

#define T_ARB		50000L	/* us: bus free for arbitration */
#define T_SEL		250000L	/* us: target answers selection (SCSI minimum) */
#define T_REQ		1000000L /* us: next REQ within a command */
#define T_FREE		100000L	/* us: BSY released after COMMAND COMPLETE */
#define SPIN		64	/* polls before a wait starts sleeping */

#define M_COMPLETE	0x00
#define M_IDENTIFY	0x80
#define M_NOP		0x08

/* Wait until (reg & mask) == val; 0, or -1 after about us microseconds. */
static int
fs_wait(reg, mask, val, us)
int reg, mask, val;
long us;
{
	int i;

	for (i = 0; i < SPIN; i++)
		if ((N_RD(reg) & mask) == val)
			return 0;
	for (; us > 0; us -= 10) {
		N_DELAY(10);
		if ((N_RD(reg) & mask) == val)
			return 0;
	}
	return -1;
}

static void
fsc_reset()
{
	N_WR(NCR_MR, 0);
	N_WR(NCR_TCR, 0);
	N_WR(NCR_ICR, ICR_RST);
	N_DELAY(30);
	N_WR(NCR_ICR, 0);
	(void)N_RD(NCR_RPI);
}

static int
fsc_init()
{
	int a, b;

	if (!N_PRESENT())
		return -1;
	N_WR(NCR_MR, 0);
	N_WR(NCR_TCR, 0);
	N_WR(NCR_CSBR, 0);	/* no selection interrupts */
	N_WR(NCR_ICR, ICR_ATN);
	a = N_RD(NCR_ICR) & 0x9f;
	N_WR(NCR_ICR, 0);
	b = N_RD(NCR_ICR) & 0x9f;
	(void)N_RD(NCR_RPI);
	return a == ICR_ATN && b == 0 ? 0 : -1;
}

/* Arbitrate and select id with ATN; 0, FS_NOTARGET or FS_BUSY. */
static int
fs_select(id)
int id;
{
	N_WR(NCR_TCR, 0);
	N_WR(NCR_ICR, 0);
	N_WR(NCR_DATA, 1 << SC_MYID);
	N_WR(NCR_MR, MR_ARB);
	if (fs_wait(NCR_ICR, ICR_AIP, ICR_AIP, T_ARB) < 0) {
		N_WR(NCR_MR, 0);
		return FS_BUSY;
	}
	N_DELAY(3);
	if (N_RD(NCR_ICR) & ICR_LA) {	/* ours is the highest ID */
		N_WR(NCR_MR, 0);
		return FS_BUSY;
	}
	N_WR(NCR_ICR, ICR_SEL | ICR_BSY);
	N_DELAY(2);
	N_WR(NCR_DATA, (1 << SC_MYID) | (1 << id));
	N_WR(NCR_ICR, ICR_BSY | ICR_DATA | ICR_ATN | ICR_SEL);
	N_WR(NCR_MR, 0);
	N_DELAY(1);
	N_WR(NCR_ICR, ICR_DATA | ICR_ATN | ICR_SEL);	/* release BSY */
	N_DELAY(1);
	if (fs_wait(NCR_CSBR, CSB_BSY, CSB_BSY, T_SEL) < 0) {
		N_WR(NCR_ICR, 0);
		return FS_NOTARGET;
	}
	N_DELAY(1);
	N_WR(NCR_ICR, ICR_ATN);
	return 0;
}

/* The bus status once REQ is up; -1 when BSY drops or REQ never comes. */
static int
fs_req(us)
long us;
{
	int i, v;

	for (i = 0;; i++) {
		v = N_RD(NCR_CSBR);
		if ((v & CSB_BSY) == 0)
			return -1;
		if (v & CSB_REQ)
			return v;
		if (i >= SPIN) {
			if ((us -= 10) <= 0)
				return -1;
			N_DELAY(10);
		}
	}
}

/*
 * One REQ/ACK cycle of phase ph, already seen with REQ set.  Out phases
 * send *v; in phases store into *v.  atn keeps ATN asserted.
 */
static int
fs_xbyte(ph, v, atn, us)
int ph, atn;
unsigned char *v;
long us;
{
	if (ph & CSB_IO) {
		*v = N_RD(NCR_DATA);
		N_WR(NCR_ICR, ICR_ACK | atn);
	} else {
		N_WR(NCR_DATA, *v);
		N_WR(NCR_ICR, ICR_DATA | atn);
		N_WR(NCR_ICR, ICR_DATA | ICR_ACK | atn);
	}
	if (fs_wait(NCR_CSBR, CSB_REQ, 0, us) < 0)
		return -1;
	N_WR(NCR_ICR, atn);
	return 0;
}

/*
 * Data bytes of phase ph (REQ already up) into or out of buf[n..len),
 * until the phase changes or len is reached.  The REQ poll that ends one
 * byte starts the next.  *np is advanced; returns 0, or -1 on a stall.
 */
static int
fs_data(ph, buf, np, len, atn, us)
int ph, atn;
unsigned char *buf;
long *np, len, us;
{
	long n = *np;
	int v, r = 0;

	for (;;) {
		if (ph == PH_DIN) {
			buf[n++] = N_RD(NCR_DATA);
			N_WR(NCR_ICR, ICR_ACK | atn);
		} else {
			N_WR(NCR_DATA, buf[n++]);
			N_WR(NCR_ICR, ICR_DATA | atn);
			N_WR(NCR_ICR, ICR_DATA | ICR_ACK | atn);
		}
		if (fs_wait(NCR_CSBR, CSB_REQ, 0, us) < 0) {
			r = -1;
			break;
		}
		N_WR(NCR_ICR, atn);
		if (n >= len)
			break;
		if ((v = fs_req(us)) < 0) {
			r = -1;
			break;
		}
		if ((v & CSB_PHASE) != ph)
			break;
	}
	*np = n;
	return r;
}

/*
 * Run job j.  FS_IN reads up to sj_len bytes into sj_data, FS_OUT
 * writes up to sj_len from it; sj_done gets the count moved.  Bytes past
 * sj_len are discarded or sent as zeros.  Returns the status byte,
 * FS_NOTARGET, FS_BUSY or FS_BUSERR (the bus was reset).
 */
static int
fsc_cmd(j)
struct scsi_job *j;
{
	unsigned char b, *buf = j->sj_data, *cdb = j->sj_cdb;
	int r, ph, tph, ci, status, atn, msgs, dir = j->sj_dir;
	long n, len = j->sj_len, us = j->sj_timeout ? j->sj_timeout : T_REQ;

	j->sj_done = 0;
	if ((r = fs_select(j->sj_target)) != 0)
		return r;
	atn = ICR_ATN;
	ci = n = msgs = 0;
	status = tph = -1;
	for (;;) {
		if ((ph = fs_req(us)) < 0)
			goto bad;
		ph &= CSB_PHASE;
		if (ph != tph)
			N_WR(NCR_TCR, (tph = ph) >> 2);
		if (((ph == PH_DIN && dir == FS_IN) || (ph == PH_DOUT && dir == FS_OUT)) &&
		    n < len) {
			if (fs_data(ph, buf, &n, len, atn, us) < 0)
				goto bad;
			continue;
		}
		switch (ph) {
		case PH_MOUT:
			b = msgs++ ? M_NOP : M_IDENTIFY | (j->sj_lun & 7);
			atn = 0;	/* ATN drops before the last byte's ACK */
			r = fs_xbyte(ph, &b, 0, us);
			break;
		case PH_CMD:
			b = ci < j->sj_cdblen ? cdb[ci] : 0;
			ci++;
			r = fs_xbyte(ph, &b, atn, us);
			break;
		case PH_DOUT:
			b = 0;
			r = fs_xbyte(ph, &b, atn, us);
			break;
		case PH_DIN:
			r = fs_xbyte(ph, &b, atn, us);
			break;
		case PH_STAT:
			r = fs_xbyte(ph, &b, atn, us);
			status = b;
			break;
		case PH_MIN:
			r = fs_xbyte(ph, &b, atn, us);
			if (r == 0 && b == M_COMPLETE) {
				N_WR(NCR_TCR, 0);
				N_WR(NCR_ICR, 0);
				j->sj_done = n;
				if (fs_wait(NCR_CSBR, CSB_BSY, 0, T_FREE) < 0 ||
				    status < 0)
					goto bad;
				return status & 0x3e;
			}
			break;
		default:
			goto bad;
		}
		if (r < 0)
			goto bad;
	}
bad:
	j->sj_done = n;
	N_WR(NCR_TCR, 0);
	N_WR(NCR_ICR, 0);
	fsc_reset();
	return FS_BUSERR;
}

struct scsi_hba N_HBA = { N_NAME, fsc_init, fsc_cmd, fsc_reset };
