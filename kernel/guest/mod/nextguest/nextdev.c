/*
 * nextdev.c -- the board: constants in the I/O window, interrupt requests,
 * and the host services the kernel's patched routines call.
 *
 * The I/O windows are memory; nothing acts on what the kernel writes there.
 * Time, the timer, software interrupts, the clock chip and NVRAM reach the
 * kernel through its own routines, patched by the host program to make
 * host calls (HF_HCALL).
 *
 * K&R C.
 */

#include "next.h"
#include "../../next/hfswire.h"

extern timestruc_t hrestime;
extern void drv_usecwait();
extern int ttimeout(), untimeout();
extern long drv_usectohz();

struct nx nx;

/* what the kernel reads from the board: the machine ids, SCR2 at power-on, the screen's size */
static struct {
	unsigned long a, v;
} board[] = {
	{ 0x0200c000UL, 0xf0004000UL },
	{ 0x0200c800UL, 0xf0004000UL },
	{ 0x0200d000UL, 0x00001080UL },
	{ 0x02200000UL, 0xffff4fcfUL },
	{ 0x02200088UL, 0 },		/* width / 4 */
	{ 0x0220008cUL, 0 },		/* rows */
};

void
nx_board(w, h)
	unsigned long w, h;
{
	int i;

	board[4].v = w / 4;
	board[5].v = h;
	for (i = 0; i < sizeof board / sizeof board[0]; i++)
		(void)copyout((caddr_t)&board[i].v, (caddr_t)(NX_PHYS + board[i].a), 4);
}

unsigned long
nx_us()
{
	timestruc_t t = hrestime;

	return (unsigned long)t.tv_sec * 1000000UL + (unsigned long)t.tv_nsec / 1000;
}

/* the highest level asked for, 0 none */
int
nx_level()
{
	if (nx.stat & INT_TIMER)
		return 6;
	if (nx.stat & INT_SOFT2)
		return 2;
	return nx.stat & INT_SOFT1 ? 1 : 0;
}

/*
 * The request at level l goes to the guest.  Software interrupts are taken
 * by it; the timer's stays in what the level 6 dispatcher's status read
 * returns until the timer handler acknowledges it.  Guest context.
 */
void
nx_post(l)
	int l;
{
	int s = guest_splhi();

	if (l == 6)
		nx.istat |= INT_TIMER;
	nx.stat &= l == 6 ? ~INT_TIMER : l == 2 ? ~INT_SOFT2 : ~INT_SOFT1;
	guest_splx(s);
}

/* the timer the kernel set is due */
static void
nx_tick(arg)
	caddr_t arg;
{
	int s = guest_splhi();

	nx.tid = 0;
	if (nx.state == 1) {
		nx.st.ticks++;
		nx.stat |= INT_TIMER;
		nx_kick();
	}
	guest_splx(s);
}

/* a guest buffer of n bytes at a lies in the guest's space */
static int
inguest(a, n)
	unsigned long a, n;
{
	return a < NX_TOP && n <= NX_TOP - a;
}

/* NVRAM's checksum: the first 15 words summed with end-around carry, complemented */
static void
nvsum(b)
	unsigned char *b;
{
	unsigned long sum = 0;
	int i;

	for (i = 0; i < 30; i += 2) {
		sum += b[i] << 8 | b[i + 1];
		if (sum >= 0x10000)
			sum -= 0xffff;
	}
	sum = 0xffff - sum;
	b[30] = sum >> 8;
	b[31] = sum;
}

/*
 * The kernel's copyin and copyout: a page at a time through the guest's
 * user map.  A page not mapped for the access, or a kernel address that
 * faults, runs the routine's own code from the start, which faults as it
 * would on the machine.  1: the PC is set.
 */
static int
nx_copy(gp, r, out, a)
	struct guest_proc *gp;
	char *r;
	int out;
	unsigned long *a;
{
	static char b[4096];	/* one machine at a time */
	unsigned long u = out ? a[1] : a[0], k = out ? a[0] : a[1], len = a[2], n, ret;
	caddr_t h;

	if ((long)len < 0 || !inguest(k, len))
		goto slow;
	for (; len; u += n, k += n, len -= n) {
		n = 4096 - (u & 4095);
		n = n < len ? n : len;
		if ((h = guest_mmu_user(gp, u, (int)n, out)) == 0 ||
		    (out ? copyin((caddr_t)k, b, n) || copyout(b, h, n) : copyin(h, b, n) || copyout(b, (caddr_t)k, n)))
			goto slow;
	}
	if (!inguest((unsigned long)GR_USP(r), 4UL) || copyin((caddr_t)GR_USP(r), (caddr_t)&ret, 4))
		goto slow;
	GR_USP(r) += 4;
	GR_PC(r) = ret;
	GR_D(r, 0) = 0;
	return 1;
slow:
	GR_PC(r) = HF_TRAMP + 16 * out;
	return 1;
}

/* one host service: 0 done, 1 done and the PC set, -1 not one */
int
nx_service(gp, r)
	struct guest_proc *gp;
	char *r;
{
	unsigned long a[3], us;
	unsigned char b[32];
	int svc = GR_D(r, 0), s;

	if (svc <= 0 || svc >= HC_N)
		return -1;
	nx.st.hcalls[svc]++;
	if (!inguest((unsigned long)GR_USP(r) + 4, (unsigned long)sizeof a) ||
	    copyin((caddr_t)GR_USP(r) + 4, (caddr_t)a, sizeof a))
		bzero((caddr_t)a, sizeof a);
	switch (svc) {
	case HC_HFS:
		psignal(curproc, SIGPWR);
		break;
	case HC_MONITOR:
	case HC_OFF:
		nx.st.monexit = (svc == HC_MONITOR ? GR_D(r, 1) : 0) | 0x80000000UL;
		psignal(curproc, SIGUSR1);
		break;
	case HC_US:
		GR_D(r, 1) = nx_us() & 0xfffff;
		break;
	case HC_DELAY:
		drv_usecwait(a[0] < 20000 ? a[0] : 20000);
		break;
	case HC_TIMER:
		if (a[0])
			break;
		us = a[1] ? 65535 : a[2] / 1000;
		us = us < 500 ? 500 : us > 65535 ? 65535 : us;
		s = guest_splhi();
		if (nx.tid > 0)
			untimeout(nx.tid);
		nx.stat &= ~INT_TIMER;
		if ((nx.tid = ttimeout(nx_tick, (caddr_t)0, drv_usectohz(us))) == -1)
			nx.tid = 0;
		guest_splx(s);
		break;
	case HC_STAT:
		GR_D(r, 0) = nx.istat;
		return 0;
	case HC_COPYIN:
	case HC_COPYOUT:
		return nx_copy(gp, r, svc == HC_COPYOUT, a);
	case HC_TACK:
		s = guest_splhi();
		nx.istat &= ~INT_TIMER;
		guest_splx(s);
		GR_D(r, 1) = nx_us() & 0xfffff;
		break;
	case HC_SOFT:
		s = guest_splhi();
		nx.stat |= GR_D(r, 1) >> 24 & (INT_SOFT1 | INT_SOFT2);
		nx_kick();
		guest_splx(s);
		break;
	case HC_TIME:
		GR_D(r, 0) = hrestime.tv_sec;
		GR_D(r, 1) = 0;
		return 0;
	case HC_NVGET:
		if (inguest(a[0], (unsigned long)sizeof b))
			(void)copyout((caddr_t)nx.st.nvram, (caddr_t)a[0], sizeof b);
		break;
	case HC_NVSET:
		if (!inguest(a[0], (unsigned long)sizeof b) || copyin((caddr_t)a[0], (caddr_t)b, sizeof b))
			break;
		nvsum(b);
		(void)copyout((caddr_t)b + 30, (caddr_t)a[0] + 30, 2);
		if (bcmp((caddr_t)b, (caddr_t)nx.st.nvram, sizeof b)) {
			bcopy((caddr_t)b, (caddr_t)nx.st.nvram, sizeof b);
			nx.st.nvseq++;
		}
		break;
	}
	GR_D(r, 0) = 0;
	return 0;
}

/* an access nothing backs, counted by address; 1 the first at a */
int
nx_census(a, wr)
	unsigned long a;
	int wr;
{
	struct nx_census *c;
	int i;

	nx.st.lasta = a;
	nx.st.lastpc = GR_PC(u.u_ar0);
	for (i = 0; i < nx.st.ncensus; i++)
		if (nx.st.census[i].addr == a && nx.st.census[i].write == wr) {
			nx.st.census[i].count++;
			nx.st.census[i].lastpc = GR_PC(u.u_ar0);
			return 0;
		}
	if (nx.st.ncensus >= NX_NCENSUS)
		return 0;
	c = &nx.st.census[nx.st.ncensus++];
	c->addr = a;
	c->pc = c->lastpc = GR_PC(u.u_ar0);
	c->write = wr;
	c->count = 1;
	return 1;
}
