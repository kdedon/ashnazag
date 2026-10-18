/*
 * lisaguest.c -- /dev/lisa and the Lisa profile.
 *
 * The Lisa's boot ROM and OS run natively in user mode with the
 * supervisor state virtual (guestcore).  Logical addresses 0-16 MB are
 * mapped from the segment registers: RAM in whole 4 KB pages is mapped,
 * so the guest runs at full speed there; I/O space stays unmapped, and
 * the launcher patches every routine that touches it into a host call.
 *
 *   segment:     SLR type 4-7  RAM at SOR * 512 (5 and 4 read-only)
 *                context 0 in supervisor mode, else SEG1/SEG2's
 *                       9      I/O space, F  ROM, others absent
 *
 * K&R C.
 */

#include "lisa.h"
#include "sys/mman.h"
#include "vm/seg.h"
#include "vm/as.h"
#include "guest_mem.h"
#include "guest_lone.h"
#include "sys/time.h"
#include "sys/param.h"

extern int nodev(), ttimeout(), untimeout();
extern timestruc_t hrestime;
extern int as_unmap();
extern struct seg *as_segat();
extern void dlm_cacheflush();
extern struct modwrapper lisaguest_wrapper;

struct lisactr lisac;
int	lisa_trace = 1;		/* console lines for unknown accesses and guest bus errors */

static struct guest_profile lisa_profile;
static int lisa_nopen;
static struct guest_ram *lisa_ram, *lisa_romram;
static struct proc *lisa_rproc;
static int lisadevflag[1] = { 0 };
static int ntrace;
static long lasttrace;

#define	M_RAM	0x01000000UL	/* l_map: RAM page | index */
#define	M_ROM	0x02000000UL
#define	M_RO	0x04000000UL

#define	C_NONE	0		/* beyond RAM */
#define	C_RAM	1
#define	C_ROM	2
#define	C_IO	4
#define	C_BAD	5		/* invalid segment: bus error */

#define	LISA_SIG	17	/* carries virtual interrupts */

static void
trace(what, a, pc)
	char *what;
	long a, pc;
{
	if (lisa_trace && ntrace < 200 && a != lasttrace) {
		lasttrace = a;
		ntrace++;
		printf("lisa: %s %x pc %x\n", what, (int)a, (int)pc);
	}
}

/* the class of logical address a, and its offset in RAM, ROM, I/O or the MMU */
static int
lclass(t, a, op)
	struct lisactr *t;
	unsigned long a, *op;
{
	unsigned long s, slr, sor, p;
	int c;

	a &= 0xffffff;
	s = a >> 17;
	slr = t->l_slr[t->l_ctx][s];
	sor = t->l_sor[t->l_ctx][s];
	p = ((sor & 0xfff) << 9) + (a & 0x1ffff);
	switch (slr >> 8 & 0xf) {
	case 4: case 5: case 6: case 7:
		/* the limit counts pages up from the segment's low end, down from its top for stacks */
		c = (a >> 9 & 0xff) + (slr & 0xff);
		if (slr & 0x100 ? c > 0xff : c < 0xff)
			return C_BAD;
		*op = p & 0x1fffff;
		return *op < t->l_ramsize ? C_RAM : C_NONE;
	case 9:
		*op = p & 0x1ffff;
		return C_IO;
	case 0xf:
		*op = a & 0x3fff;
		return C_ROM;
	}
	return C_BAD;
}

/*
 * A page whose first pages are in the segment's limit and the rest are not.
 * It, and a page whose RAM origin is not 4 KB aligned, cannot be mapped.
 */
static int
cutoff(t, va)
	struct lisactr *t;
	unsigned long va;
{
	unsigned long o;

	return lclass(t, va + 0xe00, &o) != C_RAM;
}

/* what page va should map: 0 nothing */
static unsigned long
want(t, va)
	struct lisactr *t;
	unsigned long va;
{
	unsigned long o, slr;
	int c = lclass(t, va, &o);

	if (c == C_ROM)
		return M_ROM | M_RO | o >> 12;
	if (c != C_RAM || o + 0x1000 > t->l_ramsize || (o & 0xfff) || cutoff(t, va))
		return 0;
	slr = t->l_slr[t->l_ctx][va >> 17] >> 8 & 0xf;
	return M_RAM | (slr < 6 ? M_RO : 0) | o >> 12;
}

/* n host pages from w at a: 0 done */
static int
mapat(t, a, w, n)
	struct lisactr *t;
	addr_t a;
	unsigned long w, n;
{
	int prot = PROT_READ | PROT_EXEC | PROT_USER | (w & M_RO ? 0 : PROT_WRITE);

	return guest_ram_map(w & M_ROM ? lisa_romram : lisa_ram, t->l_dev, (off_t)((w & 0xfffff) << 12), curproc->p_as,
	    &a, (u_int)(n << 12), (u_int)prot, (u_int)PROT_ALL, (u_int)(MAP_SHARED | MAP_FIXED));
}

/* drop alias window i */
static void
unalias(t, i)
	struct lisactr *t;
	int i;
{
	unsigned long j, k, m = t->l_alias[i].m;

	for (j = 0; j < 32; j = k) {
		for (; j < 32 && !(m & 1UL << j); j++)
			;
		for (k = j; k < 32 && (m & 1UL << k); k++)
			;
		if (k > j)
			(void)as_unmap(curproc->p_as, (addr_t)(t->l_alias[i].b | j << 12), (k - j) << 12);
	}
	t->l_alias[i] = t->l_alias[--t->l_nalias];
}

/* bring the mappings of host pages p..end-1 in line with the MMU; windows too unless kept */
static void
remaprange(t, p, end, keep)
	struct lisactr *t;
	unsigned long p, end;
	int keep;
{
	struct as *as = curproc->p_as;
	unsigned long n, i, w, w2;
	addr_t a;
	int e;

	for (i = 0; !keep && i < t->l_nalias; )
		if ((t->l_alias[i].b >> 12 & 0xfff) + 32 > p && (t->l_alias[i].b >> 12 & 0xfff) < end)
			unalias(t, (int)i);
		else
			i++;
	for (; p < end; p += n) {
		n = 1;
		w = want(t, p << 12);
		if (w == t->l_map[p])
			continue;
		for (; p + n < end; n++) {
			w2 = want(t, (p + n) << 12);
			if (w2 == t->l_map[p + n] || (w ? w2 != w + n : w2 != 0))
				break;
		}
		a = (addr_t)(p << 12);
		e = w ? mapat(t, a, w, n) : as_unmap(as, a, n << 12);
		if (e) {
			trace("map failed", (long)(p << 12), (long)e);
			w = 0;
			(void)as_unmap(as, a, n << 12);
		}
		for (i = 0; i < n; i++)
			t->l_map[p + i] = w ? w + i : 0;
	}
}

/*
 * The 68000 ignores address bits 24-31; the OS keeps flags there.  An
 * access above 16 MB maps the segment's pages at that 128 KB window too,
 * where the process has nothing else.  0 done.
 */
static int
alias(t, fa)
	struct lisactr *t;
	unsigned long fa;
{
	unsigned long b = fa & ~0x1ffffUL, p = fa >> 12 & 0xfe0, m = 0, i, k;

	if (fa < 0x1000000 || !t->l_map[fa >> 12 & 0xfff])
		return -1;
	if (t->l_nalias == sizeof t->l_alias / sizeof t->l_alias[0])
		unalias(t, 0);
	/* runs of pages consecutive in RAM, one mapping each */
	for (i = 0; i < 32; i = k) {
		for (k = i + 1; k < 32 && t->l_map[p + i] && t->l_map[p + k] == t->l_map[p + i] + (k - i) &&
		    !as_segat(curproc->p_as, (caddr_t)(b | k << 12)); k++)
			;
		if (t->l_map[p + i] && !as_segat(curproc->p_as, (caddr_t)(b | i << 12)) &&
		    !mapat(t, (addr_t)(b | i << 12), t->l_map[p + i], k - i))
			m |= (k < 32 ? (1UL << k) - 1 : ~0UL) & ~((1UL << i) - 1);
	}
	for (i = k = 0; i < 32; i++)
		if (t->l_map[p + i] && !(m & 1UL << i) && as_segat(curproc->p_as, (caddr_t)(b | i << 12)))
			k++;
	if (k && !(t->l_st.ls_flags & LSF_SHARED)) {
		t->l_st.ls_flags |= LSF_SHARED;
		printf("lisa: window %x shares its top byte with the launcher: %d pages left to the launcher\n",
		    (int)b, (int)k);
	}
	if (!(m & 1UL << (fa >> 12 & 31)))
		return -1;
	t->l_alias[t->l_nalias].b = b;
	t->l_alias[t->l_nalias].c = t->l_ctx;
	t->l_alias[t->l_nalias++].m = m;
	t->l_st.ls_alias++;
	return 0;
}

/* context 0 in supervisor mode, else SEG1/SEG2's */
static void
remap(t)
	struct lisactr *t;
{
	t->l_st.ls_remap++;
	t->l_ctx = t->l_gp && !(t->l_gp->gp_vsr & 0x2000) ? t->l_seg : 0;
	remaprange(t, 0UL, 4096UL, 0);
	dlm_cacheflush();
}

/* map context c: only the segments it differs in from the one mapped */
static void
setctx(t, c)
	struct lisactr *t;
	int c;
{
	int s, i, w, o = t->l_ctx;

	if (c == o)
		return;
	t->l_ctx = c;
	t->l_st.ls_ctxsw++;
	/*
	 * A window shows its own context's segment.  It stays while that
	 * segment is the same in c, and on the way into context 0 where
	 * context 0 has no RAM there, so it is still there when its context
	 * comes back.
	 */
	for (i = 0; i < t->l_nalias; ) {
		w = t->l_alias[i].c;
		s = t->l_alias[i].b >> 17 & 0x7f;
		if (w == c || (t->l_slr[c][s] == t->l_slr[w][s] && t->l_sor[c][s] == t->l_sor[w][s]) ||
		    (c == 0 && (t->l_slr[0][s] >> 8 & 0xc) != 4))
			i++;
		else
			unalias(t, i);
	}
	for (s = 0; s < 128; s++)
		if (t->l_slr[c][s] != t->l_slr[o][s] || t->l_sor[c][s] != t->l_sor[o][s])
			remaprange(t, (unsigned long)s << 5, (unsigned long)(s + 1) << 5, 1);
}

/* vSR.S changed: context 0 in supervisor mode, else SEG1/SEG2's */
static void
lisa_space(gp, sr)
	struct guest_proc *gp;
	int sr;
{
	if (lisac.l_gp == gp)
		setctx(&lisac, sr & 0x2000 ? 0 : lisac.l_seg);
}

static int splhi_();
static void splx_(), lisa_post();

/* ---- vector 2 ---- */

static int
lisa_berr(gp, r, a, ssw)
	struct guest_proc *gp;
	char *r;
	long a;
	int ssw;
{
	char f[24];
	u_long pc;

	if ((u_long)GR_PC(r) - gp->gp_hostlo < gp->gp_hosthi - gp->gp_hostlo || (u_long)GR_USP(r) >= 0x1000000) {
		printf("lisa: bus error at %x in a host call, pc %x\n", (int)a, (int)GR_PC(r));
		psignal(curproc, SIGSEGV);
		guest_trapret();
		return 0;
	}
	lisac.l_st.ls_berr++;
	lisac.l_st.ls_berraddr = a;
	lisac.l_st.ls_berrpc = GR_PC(r);
	lisac.l_st.ls_flags |= LSF_FAULT;
	trace("bus error", a, GR_PC(r));
	bzero(f, sizeof f);
	/*
	 * The 68000 frame: access word (function code, read), address, instruction word.
	 * The OS restarts a fetch fault at the address when the word is rte's, which is
	 * where this CPU resumes; a data fault gives the instruction and its pc + 2.
	 */
	P16(f, (ssw & 0x100 ? 0x10 : 0) | (a == GR_PC(r) ? 2 : 1) | (gp->gp_vsr & 0x2000 ? 4 : 0));
	P32(f + 2, a);
	pc = GR_PC(r);
	if (a == pc)
		P16(f + 6, 0x4e73);
	else if (copyin((caddr_t)pc, f + 6, 2) == 0)
		pc += 2;
	if (guest_reflect(gp, r, pc, 8, f, 10, -1) < 0) {
		printf("lisa: bus error with no guest stack, pc %x\n", (int)GR_PC(r));
		psignal(curproc, SIGSEGV);
	}
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}

static int
lisa_fault(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	struct lisactr *t = &lisac;
	unsigned long fa, d;
	int ssw, e, k;

	if (t->l_gp != gp || (e = guest_bfault(r, &fa, &ssw)) < 0)
		return 1;
	if (fa >= 0x1000000) {
		for (k = 0; k < t->l_nalias && (fa & ~0x1ffffUL) != t->l_alias[k].b; k++)
			;
		if (GR_USP(r) >= 0x1000000)
			return 1;		/* the launcher's own */
		if (k == t->l_nalias && as_segat(curproc->p_as, (caddr_t)fa)) {
			if ((u_long)GR_PC(r) >= 0x1000000)
				return 1;	/* launcher code, e.g. a signal's return */
			t->l_st.ls_hwfault++;
			printf("lisa: guest address %x is in the launcher's space (top byte %x), pc %x\n",
			    (int)fa, (int)(fa >> 24), (int)GR_PC(r));
			psignal(curproc, SIGSEGV);
			guest_trapret();
			return 0;
		}
		if (fa == GR_PC(r)) {		/* the 68000 ignores the top byte */
			GR_PC(r) = fa & 0xffffff;
			guest_trapret();
			return 0;
		}
		if (k == t->l_nalias && !alias(t, fa)) {
			guest_trapret();
			return 0;
		}
		fa &= 0xffffff;
	}
	/* a mapped page not yet loaded: the stock handler */
	if (fa < 0x1000000 && t->l_map[fa >> 12] && ((ssw & 0x100) || !(t->l_map[fa >> 12] & M_RO)))
		return 1;
	if (!(t->l_st.ls_flags & LSF_BOOT) && (GR_PC(r) & 0xffffff) < 0xfe0000) {
		t->l_st.ls_flags |= LSF_BOOT;
		t->l_st.ls_bootpc = GR_PC(r);
	}
	if (!e && lclass(t, fa, &d) == C_RAM) {
		if (t->l_map[fa >> 12])		/* a write to a read-only segment */
			return lisa_berr(gp, r, (long)fa, ssw);
		t->l_st.ls_hwfault++;
		e = t->l_ctx;
		printf("lisa: RAM at %x (offset %x) is not in whole 4 KB pages, pc %x, context %d SLR %x SOR %x\n",
		    (int)fa, (int)d, (int)GR_PC(r), e, t->l_slr[e][fa >> 17 & 0x7f], t->l_sor[e][fa >> 17 & 0x7f]);
		psignal(curproc, SIGSEGV);
		guest_trapret();
		return 0;
	}
	if (e || lclass(t, fa, &d) == C_BAD)
		return lisa_berr(gp, r, (long)fa, ssw);
	/* I/O, ROM writes, no RAM: a hardware access no patch covers */
	t->l_st.ls_hwfault++;
	t->l_st.ls_hwaddr = fa;
	t->l_st.ls_hwpc = GR_PC(r);
	printf("lisa: hardware access at %x, pc %x\n", (int)fa, (int)GR_PC(r));
	psignal(curproc, SIGSEGV);
	guest_trapret();
	return 0;
}

/* user-mode A-lines the guest's dispatcher still takes through supervisor mode */
static int
lisa_aline(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	if (!gp->gp_ualine && !(gp->gp_vsr & 0x2000))
		lisac.l_st.ls_aline++;
	return guest_lone_trap(gp, r, v);
}

/* ---- interrupts ---- */

static int
splhi_()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s;
}

static void
splx_(s)
	int s;
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}

/* the level asking: 2 for input, 1 for retrace or the timer */
static int
lisa_level(t)
	struct lisactr *t;
{
	if (!t->l_irqon)
		return 0;
	return t->l_inh != t->l_int ? 2 : t->l_vpend || t->l_tpend;
}

#define	VIPL(gp)	((gp)->gp_vsr >> 8 & 7)

/* the guest's IPL lets a request through: the carrier takes it there */
static void
lisa_post(t)
	struct lisactr *t;
{
	if (t->l_state != 1 || !t->l_gp)
		return;
	t->l_gp->gp_vpend = lisa_level(t) << 8;
	if (lisa_level(t) > VIPL(t->l_gp))
		psignal(t->l_proc, LISA_SIG);
}

/* every request above the IPL, a frame each, between instructions */
static void
lisa_intr(gp, r)
	struct guest_proc *gp;
	char *r;
{
	struct lisactr *t = &lisac;
	int l, s;

	/* a host call finishes first, in our code or the C library on our stack; the request stays latched */
	if (t->l_gp != gp || (u_long)GR_PC(r) - gp->gp_hostlo < gp->gp_hosthi - gp->gp_hostlo ||
	    (u_long)GR_USP(r) >= 0x1000000)
		return;
	switch (GR_FV(r) >> 12) {
	case 7: case 9: case 0xa: case 0xb:
		if (!(u.u_sigflag & USTKCLEAR))
			return;		/* resumes an instruction part way */
	}
	s = splhi_();
	while ((l = lisa_level(t)) > VIPL(gp)) {
		splx_(s);
		t->l_st.ls_intr++;
		if (guest_reflect(gp, r, GR_PC(r), (24 + l) << 2, (char *)0, 2, l) < 0) {
			printf("lisa: interrupt with no guest stack, pc %x\n", (int)GR_PC(r));
			psignal(curproc, SIGSEGV);
			return;
		}
		if (GR_FV(r) >> 12)
			u.u_sigflag |= USTKCLEAR;
		s = splhi_();
	}
	gp->gp_vpend = l << 8;
	sigdelset(&gp->gp_proc->p_sig, LISA_SIG);
	splx_(s);
}

/* the carrier waits while the IPL masks every request */
static int
lisa_fsig(p, gp)
	struct proc *p;
	struct guest_proc *gp;
{
	k_sigset_t h;
	int s, n;

	s = splhi_();
	h = p->p_hold;
	sigdelset(&p->p_hold, LISA_SIG);
	if (lisac.l_gp != gp || lisa_level(&lisac) <= VIPL(gp))
		sigaddset(&p->p_hold, LISA_SIG);
	n = __amix_fsig(p);
	p->p_hold = h;
	splx_(s);
	return n;
}

static int
lisa_sendsig(gp, sig, sip, hdlr)
	struct guest_proc *gp;
	int sig;
	char *sip;
	int (*hdlr)();
{
	if (sig != LISA_SIG)
		return __amix_sendsig(sig, sip, hdlr);
	lisa_intr(gp, (char *)u.u_ar0);
	return 1;
}

/* the host tick: a frame of video went by */
static void
lisa_tick(arg)
	caddr_t arg;
{
	struct lisactr *t = &lisac;
	int s = splhi_();

	t->l_timer = -1;
	if (t->l_state != 1) {
		splx_(s);
		return;
	}
	if (t->l_irqon) {
		t->l_vpend = 1;
		for (t->l_tacc += 1000000L / HZ; t->l_tacc >= 20000; t->l_tacc -= 20000)
			if (t->l_tpend < 8)
				t->l_tpend++;
	}
	lisa_post(t);
	t->l_timer = ttimeout(lisa_tick, arg, 1L);
	splx_(s);
}

/* ---- /dev/lisa ---- */

static void
lisa_pexit(gp)
	struct guest_proc *gp;
{
	if (lisac.l_gp == gp) {
		if (lisac.l_timer >= 0)
			untimeout(lisac.l_timer);
		lisac.l_timer = -1;
		lisac.l_state = 0;
		lisac.l_gp = 0;
		lisac.l_proc = 0;
	}
}

int
lisaopen(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	struct cred *cr;
{
	if (getminor(*devp) != 0)
		return ENXIO;
	lisa_nopen++;
	return 0;
}

int
lisaclose(dev, flag, otyp, cr)
	dev_t dev;
	int flag, otyp;
	struct cred *cr;
{
	lisa_nopen = 0;			/* the last close only */
	guest_ram_free(lisa_ram);
	guest_ram_free(lisa_romram);
	lisa_ram = lisa_romram = 0;
	lisa_rproc = 0;
	return 0;
}

int
lisardwr(dev, uiop, cr)
	dev_t dev;
	struct uio *uiop;
	struct cred *cr;
{
	return EINVAL;
}

int
lisammap(dev, off, prot)
	dev_t dev;
	off_t off;
	int prot;
{
	if (off < 0)
		return -1;
	return off < LISA_ROMOFF ? guest_ram_frame(lisa_ram, (unsigned long)off) :
	    guest_ram_frame(lisa_romram, (unsigned long)(off - LISA_ROMOFF));
}

int
lisasegmap(dev, off, as, addrp, len, prot, maxprot, flags, cr)
	dev_t dev;
	off_t off;
	struct as *as;
	addr_t *addrp;
	u_int len, prot, maxprot, flags;
	struct cred *cr;
{
	if (curproc != lisa_rproc)
		return EACCES;
	if (off >= LISA_ROMOFF)
		return guest_ram_map(lisa_romram, dev, off - LISA_ROMOFF, as, addrp, len, prot, maxprot, flags);
	return guest_ram_map(lisa_ram, dev, off, as, addrp, len, prot, maxprot, flags);
}

static int
ram(size)
	unsigned long size;
{
	int e;

	if (GUESTP(curproc))
		return EPERM;
	if (lisa_ram)
		return EBUSY;
	if (size < 0x80000 || size > LISA_RAMMAX || (size & 0x7fff))
		return EINVAL;
	if ((e = guest_ram_new(size, 0, 0UL, &lisa_ram)) != 0)
		return e;
	if ((e = guest_ram_new((unsigned long)LISA_ROMSZ, 0, 0UL, &lisa_romram)) != 0) {
		guest_ram_free(lisa_ram);
		lisa_ram = 0;
		return e;
	}
	lisa_rproc = curproc;
	lisac.l_ramsize = size;
	return 0;
}

static int
enter(dev, arg)
	dev_t dev;
	caddr_t arg;
{
	struct lisactr *t = &lisac;
	struct lisaenter le;
	unsigned long sz = t->l_ramsize;
	int e, c, i;

	if (copyin(arg, (caddr_t)&le, sizeof le))
		return EFAULT;
	if (!lisa_ram || curproc != lisa_rproc || t->l_state)
		return EBUSY;
	if ((e = guest_lone_enter(&lisa_profile)) != 0)
		return e;
	GUESTP(curproc)->gp_flags = (GUESTP(curproc)->gp_flags & ~GPF_FTRAP) | GPF_FRAME68K;
	GUESTP(curproc)->gp_space = lisa_space;
	GUESTP(curproc)->gp_hostlo = le.le_hostlo;
	GUESTP(curproc)->gp_hosthi = le.le_hosthi;
	bzero((caddr_t)t, sizeof *t);
	t->l_ramsize = sz;
	t->l_tzoff = le.le_tzoff;
	t->l_proc = curproc;
	t->l_gp = GUESTP(curproc);
	t->l_dev = dev;
	t->l_phys = le.le_phys;
	t->l_rom = le.le_rom;
	/* the segment tables as the boot code finds them: RAM identity-mapped, I/O and ROM above */
	for (c = 0; c < 4; c++) {
		for (i = 0; i < t->l_ramsize >> 17; i++) {
			t->l_slr[c][i] = 0x700;
			t->l_sor[c][i] = i << 8 | 0x40;
		}
		t->l_slr[c][0x7e] = 0x900;
		t->l_slr[c][0x7f] = 0xf00;
	}
	t->l_st.ls_pid = curproc->p_pid;
	ntrace = 0;
	(void)as_unmap(curproc->p_as, (addr_t)0, 0x1000000);
	remap(t);
	t->l_state = 1;
	t->l_timer = ttimeout(lisa_tick, (caddr_t)0, 1L);
	return 0;
}

/* the level 1 sources: started, or the next one taken (retrace first) */
static int
irq(how, rvp)
	int how, *rvp;
{
	struct lisactr *t = &lisac;
	int s;

	if (!t->l_state || curproc != t->l_proc)
		return ENXIO;
	s = splhi_();
	*rvp = 0;
	if (how == LIRQ_ON)
		t->l_irqon = 1;
	else if (t->l_vpend) {
		t->l_vpend = 0;
		*rvp = LIRQ_RETRACE;
	} else if (t->l_tpend) {
		t->l_tpend--;
		*rvp = LIRQ_TIMER;
	}
	splx_(s);
	return 0;
}

/* an input event queued, from the display process; or the next one taken, by the guest's */
static int
input(ev, rvp)
	unsigned long ev;
	int *rvp;
{
	struct lisactr *t = &lisac;
	int s;

	if (!t->l_state || (!ev && curproc != t->l_proc))
		return ENXIO;
	s = splhi_();
	*rvp = 0;
	if (ev) {
		if (t->l_inh - t->l_int < sizeof t->l_in / sizeof t->l_in[0])
			t->l_in[t->l_inh++ % (sizeof t->l_in / sizeof t->l_in[0])] = ev;
		lisa_post(t);
	} else if (t->l_inh != t->l_int)
		*rvp = t->l_in[t->l_int++ % (sizeof t->l_in / sizeof t->l_in[0])];
	splx_(s);
	return 0;
}

/* guest memory at logical addresses, through the segment registers */
static int
mem(arg)
	caddr_t arg;
{
	struct lisactr *t = &lisac;
	struct lisamem m;
	unsigned long a, o;
	int i, n;

	if (!t->l_state || curproc != t->l_proc)
		return ENXIO;
	if (copyin(arg, (caddr_t)&m, sizeof m))
		return EFAULT;
	if (m.lm_len < 0 || m.lm_len > sizeof m.lm_data)
		return EINVAL;
	for (i = 0; i < m.lm_len; i += n) {
		a = m.lm_addr + i;
		n = 0x200 - (a & 0x1ff);
		if (n > m.lm_len - i)
			n = m.lm_len - i;
		if (lclass(t, a, &o) != C_RAM || o + n > t->l_ramsize)
			return EFAULT;
		if (m.lm_flags & LM_WRITE ? copyout((caddr_t)m.lm_data + i, (caddr_t)(t->l_phys + o), n) :
		    copyin((caddr_t)(t->l_phys + o), (caddr_t)m.lm_data + i, n))
			return EFAULT;
	}
	if (m.lm_flags & LM_FLUSH)
		dlm_cacheflush();
	if (m.lm_flags & LM_BLOCK) {
		t->l_st.ls_pfreads++;
		t->l_st.ls_pfblock = m.lm_blk;
		t->l_st.ls_flags |= LSF_PFREAD;
	}
	if (!(m.lm_flags & LM_WRITE) && copyout((caddr_t)&m, arg, sizeof m))
		return EFAULT;
	return 0;
}

/* segment registers from the guest's own table, as its setup-mode routine would write them */
static int
seg(arg)
	caddr_t arg;
{
	struct lisactr *t = &lisac;
	struct lisaseg g;
	int i, s;

	if (!t->l_state || curproc != t->l_proc)
		return ENXIO;
	if (copyin(arg, (caddr_t)&g, sizeof g))
		return EFAULT;
	if (g.lg_n < 0 || g.lg_n > 128)
		return EINVAL;
	for (i = 0; i < g.lg_n; i++) {
		s = (g.lg_first + i) & 0x7f;
		t->l_slr[g.lg_ctx & 3][s] = g.lg_slr[i] & 0xfff;
		t->l_sor[g.lg_ctx & 3][s] = g.lg_sor[i] & 0xfff;
	}
	t->l_seg = g.lg_final & 3;
	remap(t);
	return 0;
}

int
lisaioctl(dev, cmd, arg, mode, cr, rvp)
	dev_t dev;
	int cmd, mode;
	caddr_t arg;
	struct cred *cr;
	int *rvp;
{
	switch (cmd) {
	case LISAIOC_RAM:
		return ram((unsigned long)arg);
	case LISAIOC_ENTER:
		return enter(dev, arg);
	case LISAIOC_MEM:
		return mem(arg);
	case LISAIOC_SEG:
		return seg(arg);
	case LISAIOC_IRQ:
		return irq((int)arg, rvp);
	case LISAIOC_INPUT:
		return input((unsigned long)arg, rvp);
	case LISAIOC_CTX:
		if (!lisac.l_state || curproc != lisac.l_proc)
			return ENXIO;
		lisac.l_seg = (int)arg & 3;
		if (!(lisac.l_gp->gp_vsr & 0x2000))
			setctx(&lisac, lisac.l_seg);
		return 0;
	case LISAIOC_ALINE:
		if (!lisac.l_state || curproc != lisac.l_proc)
			return ENXIO;
		if (((unsigned long)arg & 1) || (unsigned long)arg < 0x1000000UL)
			return EINVAL;
		dlm_cacheflush();
		lisac.l_gp->gp_ualine = (unsigned long)arg;
		return 0;
	case LISAIOC_VIDEO:
		if (!lisac.l_state || curproc != lisac.l_proc)
			return ENXIO;
		lisac.l_st.ls_vid = (int)arg & 0x3f;
		lisac.l_st.ls_flags |= LSF_VIDEO;
		return 0;
	case LISAIOC_PATCH: {
		struct lisapatch lp;

		if (!lisac.l_state || curproc != lisac.l_proc)
			return ENXIO;
		if (copyin(arg, (caddr_t)&lp, sizeof lp))
			return EFAULT;
		if (lp.lp_len < 1 || lp.lp_len > sizeof lp.lp_data || lp.lp_addr > lisac.l_ramsize - lp.lp_len)
			return EINVAL;
		if (copyout((caddr_t)lp.lp_data, (caddr_t)(lisac.l_phys + lp.lp_addr), lp.lp_len))
			return EFAULT;
		dlm_cacheflush();
		return 0;
	}
	case LISAIOC_STAT:
		if (!lisac.l_state)
			return ENXIO;
		return copyout((caddr_t)&lisac.l_st, arg, sizeof lisac.l_st) ? EFAULT : 0;
	}
	return EINVAL;
}

/* ---- module ---- */

static int
lisaguest_load()
{
	int e;

	if ((e = guest_lone_init(&lisa_profile, "lisa", &lisaguest_wrapper)) != 0)
		return e;
	lisa_profile.gpf_disp[2].gd_kind = GD_EMULATE;
	lisa_profile.gpf_disp[2].gd_fn = lisa_fault;
	lisa_profile.gpf_disp[10].gd_fn = lisa_aline;
	lisa_profile.gpf_exit = lisa_pexit;
	lisa_profile.gpf_intr = lisa_intr;
	lisa_profile.gpf_fsig = lisa_fsig;
	lisa_profile.gpf_sendsig = lisa_sendsig;
	return 0;
}

static int
lisaguest_unload()
{
	if (lisac.l_state || lisa_nopen)
		return EBUSY;
	guest_profile_del(&lisa_profile);
	return 0;
}

struct mod_drv_data lisaguest_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	  { lisaopen, lisaclose, lisardwr, lisardwr, lisaioctl, lisammap, lisasegmap, nodev,
	    nodev, nodev, 0, 0, lisadevflag }, LISA_MAJOR, 1 }
};

MOD_DRV_WRAPPER(lisaguest, lisaguest_load, lisaguest_unload, 0, "Apple Lisa guest");
