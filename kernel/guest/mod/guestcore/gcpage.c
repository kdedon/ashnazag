/*
 * gcpage.c -- constant pages: register pages whose reads are constants.
 *
 * The pages are frames of one guest RAM object per session, mapped
 * read-only where the guest expects the registers; reads never trap.
 * A write faults to the environment's ge_access with the store, its
 * value from a 68040's write-back or a 68030's data cycle.  ge_access
 * drops it, or sends it to a copy.  A write the CPU restarts, which
 * carries no value, also goes to the copy: the page is saved to the sink
 * frame, made writable and the instruction rerun traced.  After it, the
 * words that changed go to ge_access and the constants come back.
 * The environment may change a constant: a register that follows state.
 *
 * K&R C.
 */

#include "kinc.h"
#include "sys/sysm68k.h"
#include "sys/mman.h"
#include "vm/seg.h"
#include "vm/as.h"
#include "vm/hat.h"
#include "vm/page.h"
#include "guest_rpage.h"
#include "guest_env.h"
#include "guest_mem.h"
#include "gmem_impl.h"

extern void hat_memload(), flushmmu(), ppcopy(), dlm_cacheflush();

#define	PG	0x1000

static struct guest_cpage *
cp_find(s, a)
	struct guest_sess *s;
	unsigned long a;
{
	int i;

	for (i = 0; i < s->gs_ncpage; i++)
		if (a - s->gs_cpage[i].cp_va < s->gs_cpage[i].cp_len)
			return &s->gs_cpage[i];
	return 0;
}

/* the object's frames at off loaded at va with prot; 0, or -1 */
static int
cp_load(s, va, off, prot)
	struct guest_sess *s;
	unsigned long va, off;
	u_int prot;
{
	struct guest_ram *m = s->gs_cram;
	struct seg *seg;
	unsigned long n, step;

	if (curproc != s->gs_proc || !m)
		return -1;
	step = 1UL << m->shift;
	if (!(seg = as_segat(curproc->p_as, (caddr_t)va)))
		return -1;
	for (n = 0; n < PG; n += step) {
		if (guest_ram_frame(m, off + n) < 0)
			return -1;
		hat_memload(seg, (addr_t)(va + n), m->page[(off + n) >> m->shift],
		    prot | PROT_USER, HAT_NOFLAGS);
		flushmmu((addr_t)(va + n), 1);	/* the ATC may hold the old protection */
	}
	return 0;
}

static void
cp_copy(s, from, to)
	struct guest_sess *s;
	unsigned long from, to;
{
	struct guest_ram *m = s->gs_cram;
	unsigned long n, step = 1UL << m->shift;

	for (n = 0; n < PG; n += step)
		if (guest_ram_frame(m, from + n) >= 0 && guest_ram_frame(m, to + n) >= 0)
			ppcopy(m->page[(from + n) >> m->shift], m->page[(to + n) >> m->shift]);
}

static unsigned char cp_buf[PG];

/* the sink page's changed words, read into cp_buf, to ge_access as stores */
static void
cp_report(s, va, r)
	struct guest_sess *s;
	unsigned long va;
	char *r;
{
	struct guest_cpage *cp = cp_find(s, va);
	struct guest_access ga;
	unsigned long o, k;
	unsigned char c[2];
	int i;

	if (!cp || !s->gs_env->ge_access)
		return;
	for (o = 0; o < PG; o += 2) {
		k = va + o - cp->cp_va;
		for (i = 0; i < 2; i++)
			c[i] = k + i < cp->cp_dlen ? cp->cp_data[k + i] : cp->cp_fill;
		if (cp_buf[o] == c[0] && cp_buf[o + 1] == c[1])
			continue;
		ga.ga_addr = va + o;
		ga.ga_size = 2;
		ga.ga_val = cp_buf[o] << 8 | cp_buf[o + 1];
		ga.ga_write = 1;
		ga.ga_rmw = ga.ga_movep = 0;
		(void)(*s->gs_env->ge_access)(s, r, cp, &ga);
	}
}

/*
 * The sink's page back, read-only, its stores reported.  r, when the
 * guest's frame: the trace for the rerun off.
 */
void
guest_cpage_unsink(s, r)
	struct guest_sess *s;
	char *r;
{
	unsigned long sink, va;
	int e;

	if (r && s->gs_sinktrace) {
		if (!(s->gs_sinktrace & 2))
			GR_SR(r) &= ~0x8000;
		s->gs_sinktrace = 0;
	}
	if (!s->gs_sinkva || curproc != s->gs_proc)
		return;
	sink = guest_ram_size(s->gs_cram) - PG;
	va = s->gs_sinkva;
	dlm_cacheflush();	/* the guest's stores reach the frame first */
	e = copyin((caddr_t)va, (caddr_t)cp_buf, PG);
	cp_copy(s, sink, s->gs_sinkoff);
	(void)cp_load(s, va, s->gs_sinkoff, PROT_READ);
	s->gs_sinkva = 0;
	/* after the restore, so ge_access may change the page */
	if (!e)
		cp_report(s, va, r ? r : (char *)u.u_ar0);
}

/* the page holding a writable, its constants saved; 0, or -1 */
static int
cp_sink(s, cp, a, r)
	struct guest_sess *s;
	struct guest_cpage *cp;
	unsigned long a;
	char *r;
{
	unsigned long va = a & ~(PG - 1), off = cp->cp_off + (va - cp->cp_va);

	guest_cpage_unsink(s, r);
	cp_copy(s, off, guest_ram_size(s->gs_cram) - PG);
	if (cp_load(s, va, off, PROT_READ | PROT_WRITE) < 0)
		return -1;
	s->gs_sinkva = va;
	s->gs_sinkoff = off;
	return 0;
}

/*
 * Map n constant pages into the caller, a session's process.  cp stays
 * the environment's until the session ends.
 */
int
guest_cpage_map(s, cp, n)
	struct guest_sess *s;
	struct guest_cpage *cp;
	int n;
{
	static unsigned char b[256];
	unsigned long off = 0, o, k;
	addr_t a;
	dev_t dev = makedevice(s->gs_env->ge_major, 0);
	int i, e;

	if (s->gs_cram || curproc != s->gs_proc)
		return EBUSY;
	for (i = 0; i < n; i++) {
		if ((cp[i].cp_va | cp[i].cp_len) & (PG - 1) || !cp[i].cp_len ||
		    cp[i].cp_dlen > cp[i].cp_len)
			return EINVAL;
		cp[i].cp_off = off;
		off += cp[i].cp_len;
	}
	if ((e = guest_ram_new(off + PG, 0, 0UL, &s->gs_cram)) != 0)
		return e;
	s->gs_cpage = cp;
	s->gs_ncpage = n;
	for (i = 0; i < n; i++) {
		a = (addr_t)cp[i].cp_va;
		if ((e = guest_ram_map(s->gs_cram, dev, (off_t)cp[i].cp_off, curproc->p_as, &a,
		    (u_int)cp[i].cp_len, PROT_READ | PROT_USER, PROT_READ | PROT_WRITE | PROT_USER,
		    MAP_SHARED | MAP_FIXED)) != 0)
			break;
		/* the constants go in through a writable mapping */
		for (o = 0; o < cp[i].cp_len; o += PG)
			if (cp_load(s, cp[i].cp_va + o, cp[i].cp_off + o, PROT_READ | PROT_WRITE) < 0)
				e = EFAULT;
		for (o = 0; !e && o < cp[i].cp_len; o += k) {
			k = cp[i].cp_len - o < sizeof b ? cp[i].cp_len - o : sizeof b;
			if (o >= cp[i].cp_dlen)
				for (k = 0; k < sizeof b && o + k < cp[i].cp_len; k++)
					b[k] = cp[i].cp_fill;
			else {
				if (k > cp[i].cp_dlen - o)
					k = cp[i].cp_dlen - o;
				bcopy((caddr_t)cp[i].cp_data + o, (caddr_t)b, (u_int)k);
			}
			if (copyout((caddr_t)b, (caddr_t)(cp[i].cp_va + o), (u_int)k))
				e = EFAULT;
		}
		dlm_cacheflush();
		for (o = 0; o < cp[i].cp_len; o += PG)
			(void)cp_load(s, cp[i].cp_va + o, cp[i].cp_off + o, PROT_READ);
		if (e)
			break;
	}
	if (e)
		guest_cpage_free(s);
	return e;
}

/*
 * n bytes from b into cp_data at va and into the page: registers whose
 * reads follow state.  Session context; 0, or -1.
 */
int
guest_cpage_set(s, va, b, n)
	struct guest_sess *s;
	unsigned long va, n;
	unsigned char *b;
{
	struct guest_cpage *cp = cp_find(s, va);
	unsigned long pva = va & ~(PG - 1), off;
	int e;

	if (!cp || curproc != s->gs_proc || va - cp->cp_va + n > cp->cp_dlen ||
	    ((va + n - 1) & ~(PG - 1)) != pva)
		return -1;
	if (pva == s->gs_sinkva)
		guest_cpage_unsink(s, (char *)0);
	bcopy((caddr_t)b, (caddr_t)cp->cp_data + (va - cp->cp_va), (u_int)n);
	off = cp->cp_off + (pva - cp->cp_va);
	if (cp_load(s, pva, off, PROT_READ | PROT_WRITE) < 0)
		return -1;
	e = copyout((caddr_t)b, (caddr_t)va, (u_int)n);
	dlm_cacheflush();
	(void)cp_load(s, pva, off, PROT_READ);
	return e ? -1 : 0;
}

/* the session's reference; the mappings keep theirs until unmapped */
void
guest_cpage_free(s)
	struct guest_sess *s;
{
	if (s->gs_cram)
		guest_ram_free(s->gs_cram);
	s->gs_cram = 0;
	s->gs_cpage = 0;
	s->gs_ncpage = 0;
	s->gs_sinkva = 0;
	s->gs_sinktrace = 0;
}

static int
cp_size(ssw)
	int ssw;
{
	return (ssw & 0x60) == 0x20 ? 1 : (ssw & 0x60) == 0x40 ? 2 : 4;
}

/* a 68040 write-back: a constant page's to ge_access, the rest stored */
static int
cp_put(arg, a, sz, v)
	char *arg;
	unsigned long a, v;
	int sz;
{
	struct guest_sess *s = (struct guest_sess *)arg;
	struct guest_cpage *cp = cp_find(s, a);
	struct guest_access ga;
	unsigned char b[4];
	int k;

	if (cp) {
		ga.ga_addr = a;
		ga.ga_val = v;
		ga.ga_size = sz;
		ga.ga_write = 1;
		ga.ga_rmw = ga.ga_movep = 0;
		if (!(*s->gs_env->ge_access)(s, (char *)u.u_ar0, cp, &ga))
			return 0;
		if ((a & ~(PG - 1)) != s->gs_sinkva && cp_sink(s, cp, a, (char *)0) < 0)
			return 1;
	}
	for (k = sz - 1; k >= 0; k--, v >>= 8)
		b[k] = v;
	return copyout((caddr_t)b, (caddr_t)a, (u_int)sz) != 0;
}

/* vector 2: a write to a constant page, else the environment's disposition */
int
guest_cpage_fault(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	struct guest_env *e = GENV(gp);
	struct guest_sess *s = guest_env_sess(gp);
	struct guest_cpage *cp;
	struct guest_access ga;
	unsigned long fa, val;
	int ssw, fmt = GR_FV(r) >> 12;

	if (!s || s->gs_state != GS_RUN || guest_bfault(r, &fa, &ssw) != 0 || (ssw & 0x100) ||
	    !(cp = cp_find(s, fa)))
		return e->ge_faultdisp.gd_fn ? (*e->ge_faultdisp.gd_fn)(gp, r, v) : 1;
	if (fmt == 7 && guest_wb040(r, cp_put, (char *)s) > 0) {
		u.u_sigflag |= USTKCLEAR;
		guest_trapret();
		return 0;
	}
	if (fmt != 7 && guest_bfcycle(r, &val)) {
		ga.ga_addr = fa;
		ga.ga_size = cp_size(ssw);
		ga.ga_val = val;
		ga.ga_write = 1;
		ga.ga_rmw = ga.ga_movep = 0;
		if (!(*e->ge_access)(s, r, cp, &ga)) {
			guest_bfdone(r, 0UL);
			guest_trapret();
			return 0;
		}
	}
	/* the access reruns on the copy, traced; T1 may be the guest's own */
	if (cp_sink(s, cp, fa, r) < 0)
		return 1;
	s->gs_sinktrace = GR_SR(r) & 0x8000 ? 3 : 1;
	GR_SR(r) |= 0x8000;
	guest_trapret();
	return 0;
}

/* vector 9: the end of a rerun on the copy, else the environment's disposition */
int
guest_cpage_trace(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	struct guest_env *e = GENV(gp);
	struct guest_sess *s = guest_env_sess(gp);
	int own;

	if (s && s->gs_sinktrace) {
		own = s->gs_sinktrace & 2;
		guest_cpage_unsink(s, r);
		if (!own) {
			guest_trapret();
			return 0;
		}
	}
	return e->ge_tracedisp.gd_fn ? (*e->ge_tracedisp.gd_fn)(gp, r, v) : 1;
}
