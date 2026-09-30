/*
 * adb.c -- ADB bus driver for the VIA1 shift-register transceiver of the
 * Quadra 800 class.
 *
 * The transceiver is driven through three things: the state outputs
 * ST1:ST0 (PB5:PB4), the shift register, and the /INT input PB3.
 *   CMD  (00) with SR shifting out: send the command byte in SR.
 *   EVEN (01) / ODD (10), toggled per byte: move the next data byte,
 *        in (talk) or out (listen).
 *   IDLE (11): the transceiver repeats the last talk command by itself
 *        (auto-poll) and interrupts with the command byte in SR and PB3
 *        low when the device answers or another device raises SRQ.
 * Every byte ends with the SR interrupt (IFR bit 2).  PB3, sampled at
 * that interrupt:
 *   after the command byte     low = some other device asserts SRQ
 *   on data byte 0             low = no reply (timeout)
 *   on data byte 1             byte valid; low = reply ends here
 *   on data byte 2..7          low = reply ended, byte is filler
 *
 * adb_intr() runs the per-byte state machine at IPL 4 and hands finished
 * packets to adb_soft() through a ring.  adb_soft() runs at IPL 1 and
 * calls request completions and the device decoders.
 */
#ifdef ADB_HOST
#include <stdio.h>
#else
#include "sys/types.h"
#endif
#include "adb.h"

#define NQ	8		/* queued requests */
#define NPK	16		/* finished packets awaiting adb_soft */
#define WDOG	6		/* ticks without progress before reset */
#define SCANMAX	16		/* SRQ scan steps without data */
#define OPUS	30000		/* sync op budget, VIA reads (>= 1 us each) */

/* software states */
#define S_IDLE		0
#define S_CMD		1
#define S_LISTEN	2
#define S_TALK		3
#define S_AUTO		4

struct adbpkt {
	unsigned char	p_cmd;
	short		p_n;
	unsigned char	p_data[8];
	struct adbreq	*p_req;
};

struct adbdev adb_dev[16] = { { 0, 0 } };
int adb_ndev = 0, adb_ready = 0, adb_polladdr = ADB_ADDR_KBD;
unsigned long adb_nintr = 0, adb_nspur = 0, adb_ntmo = 0, adb_nsrq = 0;
unsigned long adb_nwdog = 0, adb_nlost = 0;

static VOL int adb_st = S_IDLE;
static VOL int adb_bus = ST_IDLE;	/* last state written */
static struct adbreq *adb_q[NQ];
static VOL int adb_qh = 0, adb_qt = 0;
static struct adbreq *adb_cur = 0;
static struct adbpkt adb_pk;		/* packet being received */
static int adb_idx = 0;			/* listen byte index */
static int adb_lastcmd = -1;		/* last command on the bus */
static int adb_srq = 0;			/* SRQ seen on the last command */
static int adb_scan = 0;		/* SRQ scan steps left, 0 = not scanning */
static int adb_scanaddr = 0;
static int adb_scanhit = -1;		/* address that answered during the scan */
static struct adbpkt adb_ring[NPK];
static VOL int adb_rh = 0, adb_rt = 0;
static VOL unsigned long adb_prog = 0;	/* bumps on every SR interrupt */
static unsigned long adb_wprog = 0;
static int adb_wticks = 0;
static int adb_insoft = 0;
static struct adbreq adb_pollreq;	/* internal talk R0 */

void adb_next();

#ifdef ADB_HOST
#define adb_spl()	0
#define adb_splx(s)	((void)(s))
#else
/* raise to IPL 4, never lower; returns the old SR */
#define SR_GET(s)	__asm__ __volatile__("movew %%sr,%0" : "=d" (s) : : "memory")
#define SR_SET(s)	__asm__ __volatile__("movew %0,%%sr" : : "d" (s) : "memory")

static int
adb_spl()
{
	int s, n;

	SR_GET(s);
	if ((s & 0x700) < 0x400) {
		n = (s & ~0x700) | 0x400;
		SR_SET(n);
	}
	return s;
}
#define adb_splx(s)	SR_SET(s)
#endif

static void
setst(st)
int st;
{
	VWR(V_ORB, (VRD(V_ORB) & ~PB_ST) | st);
	adb_bus = st;
}

static void
setsr(dir)
int dir;
{
	VWR(V_ACR, (VRD(V_ACR) & ~ACR_SRMASK) | dir);
}

static void
toggle()
{
	setst(adb_bus == ST_EVEN ? ST_ODD : ST_EVEN);
}

/* spin about n microseconds: every VIA access takes at least one */
static void
adb_delay(n)
register int n;
{
	while (n-- > 0)
		(void)VRD(V_IFR);
}

static void
adb_startreq(r)
register struct adbreq *r;
{
	adb_cur = r;
	adb_lastcmd = r->r_cmd;
	adb_st = S_CMD;
	if (adb_bus != ST_IDLE)
		setst(ST_IDLE);		/* every command starts from IDLE */
	setsr(ACR_SROUT);
	VWR(V_SR, r->r_cmd);
	setst(ST_CMD);
}

/* bus back to IDLE so the transceiver auto-polls adb_lastcmd */
static void
adb_goidle()
{
	adb_cur = 0;
	adb_st = S_IDLE;
	setsr(ACR_SRIN);
	(void)VRD(V_SR);
	setst(ST_IDLE);
}

static void
adb_talk0(a)
int a;
{
	adb_pollreq.r_cmd = ADB_TALK(a, 0);
	adb_pollreq.r_len = 0;
	adb_pollreq.r_flags = 0;
	adb_pollreq.r_done = 0;
	adb_startreq(&adb_pollreq);
}

/* next address with a device after a, wrapping; a if none */
static int
nextdev(a)
int a;
{
	register int i, b;

	for (i = 1; i <= 15; i++) {
		b = (a + i - 1) % 15 + 1;
		if (adb_dev[b].d_orig)
			return b;
	}
	return a;
}

/* a packet is complete: hand it to adb_soft, then start what is next */
static void
adb_end(n)
int n;
{
	register struct adbreq *r = adb_cur;
	register int i;

	adb_pk.p_n = n;
	if (n < 0)
		adb_ntmo++;
	if (r) {
		r->r_n = n;
		for (i = 0; i < n; i++)
			r->r_data[i] = adb_pk.p_data[i];
	}
	adb_pk.p_req = (r && r != &adb_pollreq) ? r : 0;
	if (ADB_ISTALK(adb_pk.p_cmd) && ADB_REG(adb_pk.p_cmd) == 0 && n > 0 &&
	    adb_scan)
		adb_scanhit = ADB_ADDR(adb_pk.p_cmd);
	if (((adb_rt + 1) & (NPK - 1)) == adb_rh)
		adb_nlost++;
	else {
		adb_ring[adb_rt] = adb_pk;
		adb_rt = (adb_rt + 1) & (NPK - 1);
	}
	if (r)
		r->r_flags = (r->r_flags & ~RF_QUEUED) | RF_DONE;
	adb_cur = 0;
	adb_next();
}

/*
 * Choose the next bus action: queued request, SRQ scan step, or idle
 * with the auto-poll command reloaded.
 */
void
adb_next()
{
	register struct adbreq *r;

	if (adb_qh != adb_qt) {
		r = adb_q[adb_qh];
		adb_qh = (adb_qh + 1) & (NQ - 1);
		adb_startreq(r);
		return;
	}
	if (adb_srq && adb_scan == 0) {
		adb_nsrq++;
		adb_scan = SCANMAX;
		adb_scanhit = -1;
		adb_scanaddr = ADB_ADDR(adb_lastcmd);
	}
	if (adb_scan) {
		if (adb_srq && --adb_scan > 0) {
			adb_scanaddr = nextdev(adb_scanaddr);
			if (adb_ndev == 0)
				adb_scanaddr = adb_scanaddr == ADB_ADDR_KBD ?
				    ADB_ADDR_MOUSE : ADB_ADDR_KBD;
			adb_srq = 0;
			adb_talk0(adb_scanaddr);
			return;
		}
		adb_scan = 0;
		if (adb_scanhit > 0)
			adb_polladdr = adb_scanhit;
	}
	adb_srq = 0;
	if (adb_lastcmd == ADB_TALK(adb_polladdr, 0))
		adb_goidle();
	else
		adb_talk0(adb_polladdr);
}

/* SR interrupt.  IPL 4 (called from p1int, or polled). */
void
adb_intr()
{
	register int low, b;
	register struct adbreq *r;

	VWR(V_IFR, IFR_SR);
	adb_nintr++;
	adb_prog++;
	low = !(VRD(V_ORB) & PB_INT);

	switch (adb_st) {
	case S_IDLE:
		b = VRD(V_SR);
		if (!low) {
			adb_nspur++;
			return;
		}
		/* auto-poll: SR holds the command it repeated */
		adb_pk.p_cmd = b;
		adb_pk.p_n = 0;
		adb_st = S_AUTO;
		setsr(ACR_SRIN);
		setst(ST_EVEN);
		return;

	case S_CMD:
		(void)VRD(V_SR);
		r = adb_cur;
		adb_srq = low;
		adb_pk.p_cmd = r->r_cmd;
		adb_pk.p_n = 0;
		if (ADB_ISTALK(r->r_cmd)) {
			adb_st = S_TALK;
			setsr(ACR_SRIN);
			(void)VRD(V_SR);
			setst(ST_EVEN);
		} else if (ADB_ISLISTEN(r->r_cmd) && r->r_len > 0) {
			adb_st = S_LISTEN;
			adb_idx = 0;
			VWR(V_SR, r->r_data[adb_idx++]);
			setst(ST_EVEN);
		} else
			adb_end(0);
		return;

	case S_LISTEN:
		r = adb_cur;
		if (adb_idx < r->r_len && adb_idx < 8) {
			VWR(V_SR, r->r_data[adb_idx++]);
			toggle();
		} else
			adb_end(adb_idx);
		return;

	case S_TALK:
	case S_AUTO:
		b = VRD(V_SR);
		if (low) {
			if (adb_pk.p_n == 0) {
				/* no reply; unsolicited = SRQ from another device */
				if (adb_st == S_AUTO)
					adb_srq = 1;
				adb_end(-1);
				return;
			}
			if (adb_pk.p_n == 1)
				adb_pk.p_data[adb_pk.p_n++] = b;
			adb_end(adb_pk.p_n);
			return;
		}
		adb_pk.p_data[adb_pk.p_n++] = b;
		if (adb_pk.p_n == 8)
			adb_end(8);
		else
			toggle();
		return;
	}
}

/* Queue a request; start it now if the bus is idle and quiet. */
int
adb_queue(r)
register struct adbreq *r;
{
	register int s;

	s = adb_spl();
	if (((adb_qt + 1) & (NQ - 1)) == adb_qh) {
		adb_splx(s);
		return -1;
	}
	r->r_flags = RF_QUEUED;
	r->r_n = -1;
	if (adb_st == S_IDLE && (VRD(V_ORB) & PB_INT))
		adb_startreq(r);
	else {
		adb_q[adb_qt] = r;
		adb_qt = (adb_qt + 1) & (NQ - 1);
	}
	adb_splx(s);
	return 0;
}

/* drop r from the queue; IPL 4 */
static void
adb_unqueue(r)
struct adbreq *r;
{
	register int i, j;

	for (i = adb_qh; i != adb_qt; i = (i + 1) & (NQ - 1))
		if (adb_q[i] == r) {
			for (j = i; ((j + 1) & (NQ - 1)) != adb_qt; j = (j + 1) & (NQ - 1))
				adb_q[j] = adb_q[(j + 1) & (NQ - 1)];
			adb_qt = (adb_qt - 1) & (NQ - 1);
			return;
		}
}

/*
 * Abandon the transaction in flight and restart the bus: the request
 * (if any) completes with RF_ABORT and no reply.  IPL 4.
 */
static void
adb_restart()
{
	register struct adbreq *r = adb_cur;

	adb_nwdog++;
	if (r) {
		r->r_n = -1;
		r->r_flags = (r->r_flags & ~RF_QUEUED) | RF_DONE | RF_ABORT;
	}
	adb_cur = 0;
	adb_scan = 0;
	adb_srq = 0;
	adb_lastcmd = -1;
	VWR(V_IFR, IFR_SR);
	adb_next();
}

/*
 * Run r to completion by polling the SR flag, within a fixed budget.
 * Returns the reply length, -1 for no reply, -2 if the transceiver
 * stopped interrupting (the bus is then restarted).
 */
int
adb_op_sync(r)
register struct adbreq *r;
{
	register int n, s;

	if (adb_queue(r) < 0)
		return -2;
	for (n = OPUS; n > 0 && !(r->r_flags & RF_DONE); n--)
		if (VRD(V_IFR) & IFR_SR) {
			s = adb_spl();
			adb_intr();
			adb_splx(s);
		}
	if (!(r->r_flags & RF_DONE)) {
		s = adb_spl();
		if (adb_cur == r)
			adb_restart();
		else {
			adb_unqueue(r);
			r->r_flags = RF_DONE | RF_ABORT;
		}
		adb_splx(s);
	}
	adb_soft();
	return (r->r_flags & RF_ABORT) ? -2 : r->r_n;
}

/* IPL 1: completions and device data */
void
adb_soft()
{
	register struct adbpkt *p;
	register int a = 0;

	if (adb_insoft)
		return;
	adb_insoft = 1;
	while (adb_rh != adb_rt) {
		p = &adb_ring[adb_rh];
		if (p->p_req && p->p_req->r_done)
			(*p->p_req->r_done)(p->p_req);
		if (adb_ready && ADB_ISTALK(p->p_cmd) && ADB_REG(p->p_cmd) == 0 &&
		    p->p_n > 0) {
			a = ADB_ADDR(p->p_cmd);
			switch (adb_dev[a].d_orig ? adb_dev[a].d_orig : a) {
			case ADB_ADDR_KBD:
				adbkbd_input(a, p->p_data, p->p_n);
				break;
			case ADB_ADDR_MOUSE:
				adbms_input(a, p->p_data, p->p_n);
				break;
			}
		}
		adb_rh = (adb_rh + 1) & (NPK - 1);
	}
	adb_insoft = 0;
}

/* Clock tick (IPL 2): restart a bus that stopped making progress. */
void
adb_tick()
{
	register int s;

	if (!adb_ready)
		return;
	s = adb_spl();
	if (adb_prog != adb_wprog || (adb_st == S_IDLE && adb_qh == adb_qt)) {
		adb_wprog = adb_prog;
		adb_wticks = 0;
	} else if (adb_st == S_IDLE) {
		if (++adb_wticks >= 2) {
			adb_wticks = 0;
			adb_next();	/* request waited out a low PB3 */
		}
	} else if (++adb_wticks >= WDOG) {
		adb_wticks = 0;
		adb_restart();
	}
	adb_splx(s);
	adb_soft();
	adbkbd_tick();
}

/* ------------------------------------------------------ enumeration */

static int
talk3(a, hp)
int a;
int *hp;
{
	struct adbreq r;
	int n;

	r.r_cmd = ADB_TALK(a, 3);
	r.r_len = 0;
	r.r_done = 0;
	n = adb_op_sync(&r);
	if (n >= 2 && hp)
		*hp = r.r_data[1];
	return n;
}

static int
listen3(a, b0, b1)
int a, b0, b1;
{
	struct adbreq r;

	r.r_cmd = ADB_LISTEN(a, 3);
	r.r_len = 2;
	r.r_data[0] = b0;
	r.r_data[1] = b1;
	r.r_done = 0;
	return adb_op_sync(&r);
}

static int
freeaddr()
{
	register int a;

	for (a = 15; a > 3; a--)
		if (!adb_dev[a].d_orig)
			return a;
	return 0;
}

/*
 * Move devices off shared addresses: a listen R3 with handler 0xFE
 * moves only a device that saw no collision on the last talk R3, so
 * each round separates one device.
 */
static void
adb_resolve()
{
	int pass, a, f, h, n, stuck;

	for (pass = 0; pass < 2; pass++)
		for (a = 1; a <= 15; a++) {
			if (!adb_dev[a].d_orig)
				continue;
			for (stuck = 0; stuck < 4; stuck++) {
				if ((f = freeaddr()) == 0)
					return;
				(void)talk3(a, (int *)0);
				if (listen3(a, 0x60 | f, 0xFE) == -2)
					return;
				adb_delay(1000);
				if (talk3(f, &h) < 2)
					break;		/* nothing moved */
				adb_dev[f].d_orig = adb_dev[a].d_orig;
				adb_dev[f].d_handler = h;
				if ((n = talk3(a, &h)) >= 2) {
					adb_dev[a].d_handler = h;
					adb_ndev++;	/* another one: keep going */
					continue;
				}
				/* it was alone: move it back */
				(void)listen3(f, 0x60 | a, 0xFE);
				adb_delay(1000);
				adb_dev[f].d_orig = adb_dev[f].d_handler = 0;
				break;
			}
		}
}

static char *
devname(o)
int o;
{
	switch (o) {
	case 1: return "protection";
	case 2: return "keyboard";
	case 3: return "mouse";
	case 4: return "tablet";
	case 5: return "modem";
	case 7: return "appl";
	}
	return "device";
}

void
adb_init()
{
	struct adbreq r;
	int a, h, n, s, dead;

	adb_ready = 0;
	adb_ndev = 0;
	for (a = 0; a < 16; a++)
		adb_dev[a].d_orig = adb_dev[a].d_handler = 0;
	adb_qh = adb_qt = adb_rh = adb_rt = 0;
	adb_cur = 0;
	adb_scan = adb_srq = 0;
	adb_lastcmd = -1;

	VWR(V_IER, IFR_SR);			/* SR interrupt off: polled */
	VWR(V_DDRB, (VRD(V_DDRB) | PB_ST) & ~PB_INT);
	adb_goidle();
	VWR(V_IFR, IFR_SR);
	/*
	 * /INT low in IDLE: the ROM's driver left an auto-poll reply
	 * unread and its interrupt is gone.  Read it out, or no command
	 * starts (adb_queue waits for /INT high).
	 */
	if (!(VRD(V_ORB) & PB_INT)) {
		s = adb_spl();
		adb_intr();
		adb_splx(s);
	}
	adb_delay(1000);

	r.r_cmd = ADB_RESET;
	r.r_len = 0;
	r.r_done = 0;
	dead = adb_op_sync(&r) == -2;
	adb_delay(5000);

	for (a = 1; a <= 15; a++) {
		n = talk3(a, &h);
		if (n == -2 && ++dead >= 3)
			break;
		if (n >= 2) {
			adb_dev[a].d_orig = a;
			adb_dev[a].d_handler = h;
			adb_ndev++;
		}
	}
	if (dead >= 3) {
		printf("adb: transceiver not responding, ADB disabled\n");
		VWR(V_IER, IFR_SR);
		return;
	}
	adb_resolve();

	adb_ndev = 0;
	for (a = 1; a <= 15; a++)
		if (adb_dev[a].d_orig) {
			adb_ndev++;
			printf("adb: %s at %d (default %d, handler %d)\n",
			    devname(adb_dev[a].d_orig), a, adb_dev[a].d_orig,
			    adb_dev[a].d_handler);
		}
	adb_polladdr = ADB_ADDR_KBD;
	if (!adb_dev[ADB_ADDR_KBD].d_orig) {
		printf("adb: no keyboard found");
		for (a = 1; a <= 15; a++)
			if (adb_dev[a].d_orig) {
				adb_polladdr = a;
				break;
			}
		printf("; polling address %d\n", adb_polladdr);
	}
	if (adb_ndev == 0)
		printf("adb: no devices\n");

	adb_ready = 1;
	n = adb_spl();
	adb_restart();		/* start auto-polling */
	adb_nwdog = 0;
	VWR(V_IER, 0x80 | IFR_SR);
	adb_splx(n);
	if (adb_dev[ADB_ADDR_KBD].d_orig)
		adbkbd_setleds(0);
}
