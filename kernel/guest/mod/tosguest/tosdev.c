/*
 * tosdev.c -- the TOS machine's interrupts and faults.
 *
 * The I/O windows are pages of constants mapped by the launcher.  An
 * access outside them is a bus error through the guest's vector 2,
 * which is how TOS probes for chips.
 *
 * K&R C.
 */

#include "tos.h"

extern struct seg *as_segat();

static int ntrace;		/* console lines so far */

static void
trace(what, a, pc)
	char *what;
	long a, pc;
{
	if (tos_trace && ntrace < 60) {
		ntrace++;
		printf("tos: %s %x pc %x\n", what, (int)a, (int)pc);
	}
}

/* ---- vector 2 ---- */

/* bus error through the guest's vector 2, as a 68030 short bus fault frame */
int
tos_berr(gp, r, a, ssw, absent)
	struct guest_proc *gp;
	char *r;
	long a;
	int ssw, absent;
{
	char f[24];

	if (absent)
		tosc.t_st.ts_absent++;
	else
		tosc.t_st.ts_berr++;
	tosc.t_st.ts_refl[2]++;
	trace(absent ? "absent" : "bus error", a, GR_PC(r));
	bzero(f, sizeof f);
	P16(f + 2, guest_ssw030((ssw & ~7) | 5));	/* supervisor data */
	P32(f + 8, a);
	if (guest_reflect(gp, r, GR_PC(r), 0xa008, f, 26, -1) < 0) {
		printf("tos: bus error with no guest stack, pc %x\n", (int)GR_PC(r));
		psignal(curproc, SIGSEGV);
	}
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}

/* writes a 68040 left in its write-back buffers (real 68040s complete them here) */
static int
writebacks(r)
	char *r;
{
	static char so[3] = { 14, 16, 18 }, ao[3] = { 24, 32, 40 };
	unsigned short *sp;
	unsigned long a, d;
	unsigned char b[4];
	int i, k, n = 0, sz;

	for (i = 0; i < 3; i++) {
		sp = (unsigned short *)(r + 64 + so[i]);
		if (!(*sp & 0x80))
			continue;
		sz = (*sp >> 5) & 3;
		sz = sz == 1 ? 1 : sz == 2 ? 2 : 4;
		a = *(unsigned long *)(r + 64 + ao[i]);
		d = *(unsigned long *)(r + 64 + ao[i] + 4);
		for (k = sz - 1; k >= 0; k--, d >>= 8)
			b[k] = d;
		(void)copyout((caddr_t)b, (caddr_t)a, sz);
		*sp &= ~0x80;
		n++;
	}
	return n;
}

/* in an I/O window: a chip this machine lacks */
#define	IOWIN(a)	((a) >= 0xff000000 || ((a) >= 0xf00000 && (a) < 0x1000000))

/*
 * A guest access that faulted at fa.  0: resolved, the access reruns;
 * 1: paged memory, the stock handler's; 2: a bus error reflected.
 */
int
tos_access(gp, r, fa, ssw)
	struct guest_proc *gp;
	char *r;
	unsigned long fa;
	int ssw;
{
	if (!(ssw & 0x100) && tos_iowrite(fa) == 0) {
		/* the constants stay: the write goes to a copy, dropped at the next interrupt */
		tosc.t_st.ts_iow++;
		tosc.t_st.ts_iowpc = GR_PC(r);
		trace("I/O write", fa, GR_PC(r));
	} else if (fa >= 0xfffff000 && tos_toppage() == 0)
		tosc.t_st.ts_top++;
	else if (as_segat(curproc->p_as, (caddr_t)fa) != 0)
		return 1;
	else {
		tos_berr(gp, r, (long)fa, ssw, IOWIN(fa));
		return 2;
	}
	return 0;
}

int
tos_fault(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned long fa;
	int ssw, e;

	if ((gp->gp_flags & TGF_SOLO) || (e = guest_bfault(r, &fa, &ssw)) < 0)
		return 1;		/* a lone guest has no machine registers */
	tosc.t_st.ts_lastpc = GR_PC(r);
	if (e) {
		if (as_segat(curproc->p_as, (caddr_t)fa) != 0)
			return 1;
		return tos_berr(gp, r, (long)fa, ssw, IOWIN(fa));
	}
	if ((e = tos_access(gp, r, fa, ssw)) != 0)
		return e == 1;
	/* a 68040 write is done already, in its write-back buffers */
	if (!(ssw & 0x100) && (GR_FV(r) >> 12) == 7 && writebacks(r))
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}

/* ---- interrupts: MFP vectors from host timers ---- */

static unsigned short presc[8] = { 0, 4, 10, 16, 50, 64, 100, 200 };

static void
owe(ch, n)
	int ch, n;
{
	if (!(tosc.t_ien >> ch & 1))
		return;
	n += tosc.t_ipend[ch];
	tosc.t_ipend[ch] = n > 8 ? 8 : n;
}

int
mfp_level()
{
	int ch;

	for (ch = 0; ch < 16; ch++)
		if (tosc.t_ipend[ch])
			return 6;
	return 0;
}

/* interrupt acknowledge: the vector, highest channel first */
int
mfp_ack()
{
	int ch;

	for (ch = 15; ch >= 0; ch--) {
		if (tosc.t_ipend[ch]) {
			tosc.t_ipend[ch]--;
			break;
		}
	}
	if (ch < 0)
		return 24;			/* spurious */
	tosc.t_st.ts_ints[ch]++;
	return 0x40 | ch;
}

/* an acknowledge whose frame could not be written */
void
mfp_unack(vec)
	int vec;
{
	int ch = vec & 15;

	if (vec == 24)
		return;
	owe(ch, 1);
	tosc.t_st.ts_ints[ch]--;
}

/*
 * Xbtimer(timer, control, data): A and B run at the rate asked, or
 * count Timer A events.  C always runs at 200 Hz; D never fires.
 */
void
tos_xbtimer(v)
	unsigned long v;
{
	struct tostimer *tm;
	int i = v >> 16 & 3, c = v >> 8 & 0xff, d = v & 0xff ? v & 0xff : 256;
	int s = splhi_();

	if (i < 2) {
		tm = &tosc.t_tm[i];
		tm->tm_per = c >= 1 && c <= 7 ? presc[c] * d : 0;
		tm->tm_acc = 0;
		tm->tm_ev = tm->tm_left = c == 8 ? d : 0;
		tosc.t_ipend[i ? CH_TIMERB : CH_TIMERA] = 0;
	}
	splx_(s);
}

/* Jenabint, Jdisint: a disabled channel drops what it owed */
void
tos_inten(v)
	unsigned long v;
{
	int ch = v >> 8 & 15, s = splhi_();

	if (v & 1)
		tosc.t_ien |= 1 << ch;
	else {
		tosc.t_ien &= ~(1 << ch);
		tosc.t_ipend[ch] = 0;
	}
	splx_(s);
}

/* a Timer A event input */
static void
tevent()
{
	struct tostimer *tm = &tosc.t_tm[0];

	if (tm->tm_ev && --tm->tm_left == 0) {
		tm->tm_left = tm->tm_ev;
		owe(CH_TIMERA, 1);
	}
}

/* each clock tick: the timers' periods */
void
tos_timers()
{
	struct tosctr *t = &tosc;
	struct tostimer *tm;
	int i, n;

	/* subtracted, not divided: a constant divisor is a 64-bit multiply, which the 68060 traps */
	t->t_tcacc += 200;
	for (n = 0; t->t_tcacc >= HZ; n++)
		t->t_tcacc -= HZ;
	owe(CH_TIMERC, n);
	for (i = 0; i < 2; i++) {
		tm = &t->t_tm[i];
		if (tm->tm_per) {
			tm->tm_acc += 2457600 / HZ;
			owe(i ? CH_TIMERB : CH_TIMERA, (int)(tm->tm_acc / tm->tm_per));
			tm->tm_acc %= tm->tm_per;
		}
	}
}

/* the pump's call: a buffer ended (Timer A counts an event, MFP input 7 raises its interrupt) */
void
tos_snd(sn)
	struct tossndio *sn;
{
	int s = splhi_();

	if (sn->sn_ends & TSE_TIMERA)
		tevent();
	if (sn->sn_ends & TSE_GPIP7)
		owe(CH_GPIP7, 1);
	splx_(s);
	if (sn->sn_ends)
		tos_kick(1);
}

/* after a reset of the container */
void
tos_devinit(t)
	struct tosctr *t;
{
	ntrace = 0;
}
