/*
 * gmmu.c -- a guest's 68040 MMU over the host's.
 *
 * The platform names where guest physical memory lies in the process
 * (guest physical p at host pbase + p, for p below plimit) and the
 * window of guest virtual addresses the process can give the guest.
 * When the guest first sets TC.E, one segment replaces the window; its
 * pages fill at fault from the guest's own tables or transparent
 * ranges, in the space (SRP or URP) its virtual S bit selects.  A page
 * maps read-only until a write sets M in the guest's descriptor; any
 * access sets U.  Loaded pages are listed so a space switch or a flush
 * drops just those.  With TC.E clear again the window maps identity.
 *
 * K&R C.
 */

#include "kinc.h"
#include "sys/mman.h"
#include "sys/resource.h"
#include "vm/seg.h"
#include "vm/as.h"
#include "vm/hat.h"
#include "vm/page.h"

/* hat flags of the 68040 port, beyond AMIX's */
#define HAT_NOCPUSH	0x20	/* load: the frame's bytes did not change */
#define HAT_NOPFLUSH	0x40	/* unload: the caller flushes the ATC */

extern int as_map(), as_unmap(), as_fault();
extern void hat_memload(), hat_devload(), hat_unload(), seg_free();
extern page_t *page_numtookpp();
extern int guest_ram_at();
extern void flushmmu();
extern unsigned long ds_gen;
__asm__(".weak hat_cm_ram");
extern long hat_cm_ram;
static long *__volatile__ gmu_hat4k = &hat_cm_ram;
__asm__(".weak cputype");
extern long gmu_cputype __asm__("cputype");
static long *__volatile__ gmu_cpu = &gmu_cputype;

unsigned long guest_cflush_all = 0, guest_cflush_scoped = 0;	/* guest cache instructions: whole caches, lines or pages */

#define	HP	4096UL		/* host page, with the 4 KB HAT */
#define	NLOAD	4096
#define	SR_S	0x2000
#define	TC_E	0x8000
#define	TC_P	0x4000		/* 8 KB pages */

struct guest_mmu {
	struct guest_proc *gp;
	struct seg	*seg;		/* the window, once TC.E was set */
	unsigned long	pbase, plimit, lo, hi;
	unsigned long	tc, srp, urp, tt[4], mmusr;	/* tt: ITT0, ITT1, DTT0, DTT1 */
	int		space;		/* SR_S of the space loaded */
	int		nload;		/* above NLOAD: the list overflowed */
	int		moved;		/* holds frames of a display session, which may move */
	struct guest_mmu *next;		/* of those */
	unsigned long	load[NLOAD];
	unsigned long	nfill, nfault, nswitch, nflush;
};

static struct seg_ops gmu_ops;
static struct guest_mmu *gmu_moved;

/* a long of guest physical memory */
static int
prd(m, a, v)
	struct guest_mmu *m;
	unsigned long a, *v;
{
	return a > m->plimit - 4 || copyin((caddr_t)(m->pbase + a), (caddr_t)v, 4) ? -1 : 0;
}

static void
pwr(m, a, v)
	struct guest_mmu *m;
	unsigned long a, v;
{
	(void)copyout((caddr_t)&v, (caddr_t)(m->pbase + a), 4);
}

/*
 * Translate va for an access rw in space s.  upd sets U, and M for a
 * write, as the table search would.  0, or FC_NOMAP or FC_PROT; *wpp:
 * no write allowed, *mp: may map writable now, *ttp: transparent.
 */
static int
walk(m, va, rw, s, upd, pap, wpp, mp, ttp)
	struct guest_mmu *m;
	unsigned long va, *pap;
	int rw, s, upd, *wpp, *mp, *ttp;
{
	unsigned long a, d, t, n, mask;
	int i, sf, wp = 0, pg8 = (m->tc & TC_P) != 0;

	*ttp = 0;
	if (!(m->tc & TC_E)) {
		*pap = va;
		*wpp = 0;
		*mp = 1;
		return 0;
	}
	for (i = rw == S_EXEC ? 0 : 2; i < (rw == S_EXEC ? 2 : 4); i++) {
		t = m->tt[i];
		mask = t >> 16 & 0xff;
		sf = t >> 13 & 3;
		if (!(t & 0x8000) || ((va >> 24 ^ t >> 24) & ~mask & 0xff) ||
		    (sf == 0 && s) || (sf == 1 && !s))
			continue;
		if (rw == S_WRITE && (t & 4))
			return FC_PROT;
		*pap = va;
		*wpp = (t & 4) != 0;
		*mp = !*wpp;
		*ttp = 1;
		return 0;
	}
	a = ((s ? m->srp : m->urp) & ~0x1ffUL) + (va >> 25 << 2);
	for (i = 0; i < 2; i++) {
		if (prd(m, a, &d) || !(d & 2))
			return FC_NOMAP;
		wp |= d & 4;
		if (upd && !(d & 8))
			pwr(m, a, d | 8);
		a = i == 0 ? (d & ~0x1ffUL) + ((va >> 18 & 0x7f) << 2) :
		    pg8 ? (d & ~0x7fUL) + ((va >> 13 & 0x1f) << 2) : (d & ~0xffUL) + ((va >> 12 & 0x3f) << 2);
	}
	if (prd(m, a, &d))
		return FC_NOMAP;
	if ((d & 3) == 2) {			/* indirect */
		a = d & ~3UL;
		if (prd(m, a, &d) || (d & 3) == 2)
			return FC_NOMAP;
	}
	if ((d & 3) == 0)
		return FC_NOMAP;
	if ((d & 0x80) && !s)
		return FC_PROT;
	wp |= d & 4;
	if (rw == S_WRITE && wp)
		return FC_PROT;
	n = d | 8 | (rw == S_WRITE ? 0x10 : 0);
	if (upd && n != d)
		pwr(m, a, n);
	*pap = (d & (pg8 ? ~0x1fffUL : ~0xfffUL)) | (va & (pg8 ? 0x1fff : 0xfff));
	*wpp = wp != 0;
	*mp = !wp && (n & 0x10);
	return 0;
}

/* the page descriptor of a in the process, or 0 */
static unsigned long *
leaf(as, a)
	struct as *as;
	unsigned long a;
{
	unsigned long *root = *(unsigned long **)((char *)as + 20);
	unsigned long d;

	if (!root || !((d = root[a >> 25]) & 2))
		return 0;
	d = ((unsigned long *)(d & ~0x1ffUL))[a >> 18 & 0x7f];
	if (!(d & 2))
		return 0;
	return (unsigned long *)(d & ~0xffUL) + (a >> 12 & 0x3f);
}

/* the host frame at a in the process, or -1 */
static long
hostpfn(as, a)
	struct as *as;
	unsigned long a;
{
	unsigned long *d = leaf(as, a);

	if (!d || !(*d & 1))
		return -1;
	return (long)((*d & ~(HP - 1)) / HP);
}

static void
unload_all(m)
	struct guest_mmu *m;
{
	int i;

	if (m->seg) {
		if (m->nload > NLOAD)
			hat_unload(m->seg, m->seg->s_base, m->seg->s_size, HAT_NOFLAGS);
		else {
			for (i = 0; i < m->nload; i++)
				hat_unload(m->seg, (addr_t)m->load[i], (u_int)HP, HAT_NOPFLUSH);
			__asm__ __volatile__(".short 0xf518" : : : "memory");	/* pflusha */
		}
	}
	m->nload = 0;
	m->nflush++;
}

static void
setspace(m, s)
	struct guest_mmu *m;
	int s;
{
	if (m->space == s)
		return;
	if (m->nload)
		unload_all(m);
	m->space = s;
	m->nswitch++;
}

/* one host page of the window */
static faultcode_t
fill(seg, m, va, rw, reload, lock)
	struct seg *seg;
	struct guest_mmu *m;
	unsigned long va;
	enum seg_rw rw;
	int reload, lock;
{
	struct as *as = seg->s_as;
	unsigned long pa, h;
	enum seg_rw hrw = S_WRITE;
	page_t *pp;
	unsigned long *d, gen = ds_gen;
	long pf;
	int e, wp, mod, tt, prot, moved = 0;

	setspace(m, (int)(m->gp->gp_vsr & SR_S));
	if ((e = walk(m, va, (int)rw, m->space != 0, 1, &pa, &wp, &mod, &tt)) != 0) {
		m->nfault++;
		return e;
	}
	if (pa >= m->plimit)
		return FC_NOMAP;
	h = m->pbase + (pa & ~(HP - 1));
	if ((pf = guest_ram_at(as, (addr_t)h)) >= 0) {
		hrw = S_OTHER;
	} else if (as_fault(as, (addr_t)h, (u_int)HP, F_SOFTLOCK, S_WRITE)) {
		hrw = S_READ;
		if (!as_fault(as, (addr_t)h, (u_int)HP, F_SOFTLOCK, S_READ)) {
			if (rw == S_WRITE) {
				(void)as_fault(as, (addr_t)h, (u_int)HP, F_SOFTUNLOCK, S_READ);
				return FC_PROT;
			}
			mod = 0;
		} else if (lock || as_fault(as, (addr_t)h, (u_int)HP, F_INVAL, S_WRITE) || (pf = hostpfn(as, h)) < 0)
			return FC_NOMAP;
		else
			/* a display session's pages refuse locking; it tells when they move */
			hrw = S_OTHER, moved = 1;
	}
	if (hrw != S_OTHER && (pf = hostpfn(as, h)) < 0) {
		(void)as_fault(as, (addr_t)h, (u_int)HP, F_SOFTUNLOCK, hrw);
		return FC_NOMAP;
	}
	if (reload)
		hat_unload(seg, (addr_t)va, (u_int)HP, HAT_NOFLAGS);
	prot = PROT_READ | PROT_EXEC | PROT_USER | (mod ? PROT_WRITE : 0);
	/* the guest keeps its own caches coherent */
	if ((pp = page_numtookpp((u_int)pf)) != 0)
		hat_memload(seg, (addr_t)va, pp, prot, HAT_NOCPUSH);
	else
		hat_devload(seg, (addr_t)va, (u_int)pf, prot, HAT_NOCPUSH);
	if (hrw != S_OTHER)
		(void)as_fault(as, (addr_t)h, (u_int)HP, F_SOFTUNLOCK, hrw);
	if (moved) {
		/* the session's caching (write-through VRAM) */
		if ((d = leaf(as, va)) != 0 && (*d & 1) && leaf(as, h)) {
			*d = (*d & ~0x60UL) | (*leaf(as, h) & 0x60);
			flushmmu((caddr_t)va, 1);
		}
		if (!m->moved) {
			m->moved = 1;
			m->next = gmu_moved;
			gmu_moved = m;
		}
		/* moved while loading: the access faults again */
		if (gen != ds_gen)
			hat_unload(seg, (addr_t)va, (u_int)HP, HAT_NOFLAGS);
	}
	if (m->nload < NLOAD)
		m->load[m->nload] = va;
	if (m->nload <= NLOAD)
		m->nload++;
	m->nfill++;
	return 0;
}

static faultcode_t
gmu_fault(seg, addr, len, type, rw)
	struct seg *seg;
	addr_t addr;
	u_int len;
	enum fault_type type;
	enum seg_rw rw;
{
	struct guest_mmu *m = (struct guest_mmu *)seg->s_data;
	unsigned long va;
	faultcode_t e;

	if (type == F_SOFTUNLOCK)
		return 0;
	if (!m)
		return FC_NOMAP;
	for (va = (unsigned long)addr & ~(HP - 1); va < (unsigned long)addr + len; va += HP)
		if ((e = fill(seg, m, va, rw, type == F_PROT, type == F_SOFTLOCK)) != 0)
			return e;
	return 0;
}

/* a child gets an empty window */
static int
gmu_dup(seg, nseg)
	struct seg *seg, *nseg;
{
	nseg->s_ops = &gmu_ops;
	nseg->s_data = 0;
	/* the caller adds the size back */
	nseg->s_as->a_size -= nseg->s_size;
	return 0;
}

static void
gmu_free(seg)
	struct seg *seg;
{
	struct guest_mmu *m = (struct guest_mmu *)seg->s_data;

	if (m) {
		m->seg = 0;
		m->nload = 0;
	}
}

static int
gmu_unmap(seg, addr, len)
	struct seg *seg;
	addr_t addr;
	u_int len;
{
	if (addr != seg->s_base || len != seg->s_size)
		return EINVAL;
	/* the caller takes len off the size, which never counted it */
	seg->s_as->a_size += len;
	hat_unload(seg, addr, len, HAT_NOFLAGS);
	seg_free(seg);
	return 0;
}

static int
gmu_none() { return 0; }

static int
gmu_refuse() { return EACCES; }

static int
gmu_nokluster() { return -1; }

static int
gmu_getprot(seg, addr, len, protv)
	struct seg *seg;
	addr_t addr;
	u_int len, *protv;
{
	u_int i;

	/* one entry per page; a length of 0 asks for one */
	for (i = 0; i == 0 || i < (len + HP - 1) / HP; i++)
		protv[i] = PROT_ALL;
	return 0;
}

static int
gmu_gettype() { return MAP_SHARED; }

static int
gmu_getvp() { return -1; }

static int
gmu_incore(seg, addr, len, vec)
	struct seg *seg;
	addr_t addr;
	u_int len;
	char *vec;
{
	u_int i;

	for (i = 0; i < len; i += HP)
		*vec++ = 1;
	return (int)len;
}

static int
gmu_create(seg, arg)
	struct seg *seg;
	caddr_t arg;
{
	seg->s_ops = &gmu_ops;
	seg->s_data = (_VOID *)arg;
	((struct guest_mmu *)arg)->seg = seg;
	return 0;
}

static void
gmu_opsinit()
{
	gmu_ops.dup = gmu_dup;
	gmu_ops.unmap = gmu_unmap;
	gmu_ops.free = gmu_free;
	gmu_ops.fault = gmu_fault;
	gmu_ops.faulta = (faultcode_t (*)())gmu_none;
	gmu_ops.unload = (void (*)())gmu_none;
	gmu_ops.setprot = gmu_refuse;
	gmu_ops.checkprot = gmu_none;
	gmu_ops.kluster = gmu_nokluster;
	gmu_ops.swapout = (u_int (*)())gmu_none;
	gmu_ops.sync = gmu_none;
	gmu_ops.incore = gmu_incore;
	gmu_ops.lockop = gmu_none;
	gmu_ops.getprot = gmu_getprot;
	gmu_ops.getoffset = (off_t (*)())gmu_none;
	gmu_ops.gettype = gmu_gettype;
	gmu_ops.getvp = gmu_getvp;
}

/*
 * The window replaces whatever the process had there.  The trap fast
 * paths enter the guest's handlers without a space switch: turned off.
 */
static int
window(m)
	struct guest_mmu *m;
{
	struct as *as = m->gp->gp_proc->p_as;
	rlim_t lim;
	int e;

	if (!gmu_ops.fault)
		gmu_opsinit();
	if ((e = as_unmap(as, (addr_t)m->lo, (u_int)(m->hi - m->lo))) != 0)
		return e;
	/* the window is address space, not memory: outside the process's limit and size */
	lim = u.u_rlimit[RLIMIT_VMEM].rlim_cur;
	u.u_rlimit[RLIMIT_VMEM].rlim_cur = ~(rlim_t)0;
	e = as_map(as, (addr_t)m->lo, (u_int)(m->hi - m->lo), gmu_create, (caddr_t)m);
	u.u_rlimit[RLIMIT_VMEM].rlim_cur = lim;
	if (e)
		return e;
	as->a_size -= m->hi - m->lo;
	m->gp->gp_flags &= ~(GPF_FTRAP | GPF_FTRAP13);
	/* a core file of the window would read the whole guest space */
	u.u_rlimit[RLIMIT_CORE].rlim_cur = 0;
	m->space = m->gp->gp_vsr & SR_S;
	return 0;
}

/* the platform's layout: 0, or EINVAL where this host cannot shadow */
int
guest_mmu_init(gp, pbase, plimit, lo, hi)
	struct guest_proc *gp;
	unsigned long pbase, plimit, lo, hi;
{
	struct guest_mmu *m;

	if (gp->gp_mmu)
		return EBUSY;
	if (!gmu_hat4k || ((pbase | plimit | lo | hi) & (HP - 1)) || hi <= lo ||
	    pbase + plimit < pbase || (pbase < hi && pbase + plimit > lo))
		return EINVAL;
	/* the shadow fast-path check in the gates reads gp_mmu at a fixed offset */
	if ((char *)&gp->gp_mmu - (char *)gp != 68)
		return EINVAL;
	m = (struct guest_mmu *)kmem_zalloc(sizeof *m, KM_SLEEP);
	m->gp = gp;
	m->pbase = pbase;
	m->plimit = plimit;
	m->lo = lo;
	m->hi = hi;
	gp->gp_mmu = m;
	return 0;
}

void
guest_mmu_free(gp)
	struct guest_proc *gp;
{
	struct guest_mmu *m = gp->gp_mmu, **mp;

	if (!m)
		return;
	if (m->seg)
		m->seg->s_data = 0;
	for (mp = &gmu_moved; *mp; mp = &(*mp)->next)
		if (*mp == m) {
			*mp = m->next;
			break;
		}
	gp->gp_mmu = 0;
	kmem_free((_VOID *)m, sizeof *m);
}

/* a display session's pages moved: drop what was loaded from them */
int
guest_mmu_dsmoved()
{
	struct guest_mmu *m;

	for (m = gmu_moved; m; m = m->next)
		unload_all(m);
	return 0;
}

/* a change of the virtual S bit */
void
guest_mmu_space(gp, s)
	struct guest_proc *gp;
	int s;
{
	if (gp->gp_mmu && gp->gp_mmu->seg)
		setspace(gp->gp_mmu, s & SR_S);
}

/* movec of an MMU register: 0, or -1 for any other register */
int
guest_mmu_creg(gp, c, vp, wr)
	struct guest_proc *gp;
	int c, wr;
	long *vp;
{
	struct guest_mmu *m = gp->gp_mmu;
	unsigned long *cp, old;
	int e;

	switch (c) {
	case 0x003: cp = &m->tc; break;
	case 0x004: case 0x005: case 0x006: case 0x007: cp = &m->tt[c - 4]; break;
	case 0x805: cp = &m->mmusr; break;
	case 0x806: cp = &m->urp; break;
	case 0x807: cp = &m->srp; break;
	default: return -1;
	}
	if (!wr) {
		*vp = *cp;
		return 0;
	}
	old = *cp;
	*cp = c == 0x003 ? *vp & (TC_E | TC_P) : *vp;
	if (c == 0x003 && (*cp & TC_E) && !m->seg && (e = window(m)) != 0) {
		printf("NOTICE: guest pid %d: no MMU window (%d)\n", (int)curproc->p_pid, e);
		*cp = old;
		return 0;
	}
	/* the URP is not in use while the guest runs movec */
	if (old != *cp && (c != 0x806 || m->space == 0) && c != 0x805)
		unload_all(m);
	return 0;
}

/* pflush (An) / pflushn (An) / pflusha / pflushan, ptestr / ptestw (An) */
int
guest_mmu_insn(gp, op, a)
	struct guest_proc *gp;
	int op;
	unsigned long a;
{
	struct guest_mmu *m = gp->gp_mmu;
	unsigned long pa;
	int wp, mod, tt, s = (gp->gp_vdfc & 4) != 0;

	if ((op & 0xffe0) == 0xf500) {
		if ((op & 0x10) || !m->seg)
			unload_all(m);
		else if (a >= m->lo && a < m->hi)
			hat_unload(m->seg, (addr_t)(a & (m->tc & TC_P ? ~0x1fffUL : ~(HP - 1))),
			    (u_int)(m->tc & TC_P ? 2 * HP : HP), HAT_NOFLAGS);
		return 0;
	}
	if ((op & 0xffd8) == 0xf548) {
		if (walk(m, a, op & 0x20 ? S_READ : S_WRITE, s, 0, &pa, &wp, &mod, &tt))
			m->mmusr = 0;
		else
			m->mmusr = (pa & ~0xfffUL) | (tt ? 2 : 0) | (wp ? 4 : 0) | (mod ? 0x10 : 0) | 1;
		return 0;
	}
	return -1;
}

/* cpushl or cpushp of host physical a; c: 1 data, 2 instruction, 3 both caches */
static void
hcpush(a, c, page)
	unsigned long a;
	int c, page;
{
	register unsigned long r __asm__("a0") = a;

	switch (c | page << 2) {
	case 1: __asm__ __volatile__(".short 0xf468" : : "a" (r) : "memory"); break;
	case 2: __asm__ __volatile__(".short 0xf4a8" : : "a" (r) : "memory"); break;
	case 3: __asm__ __volatile__(".short 0xf4e8" : : "a" (r) : "memory"); break;
	case 5: __asm__ __volatile__(".short 0xf470" : : "a" (r) : "memory"); break;
	case 6: __asm__ __volatile__(".short 0xf4b0" : : "a" (r) : "memory"); break;
	case 7: __asm__ __volatile__(".short 0xf4f0" : : "a" (r) : "memory"); break;
	}
}

/*
 * cinv or cpush of the line or page at guest physical a, done as a push
 * of the same caches over the host frames: an invalidate would drop
 * host data.  -1: the whole caches.
 */
int
guest_cache_insn(gp, op, a)
	struct guest_proc *gp;
	int op;
	unsigned long a;
{
	struct guest_mmu *m = gp->gp_mmu;
	struct as *as = gp->gp_proc->p_as;
	unsigned long *d, h, n;
	int c = op >> 6 & 3, sc = op >> 3 & 3;

	if (!gmu_hat4k || !gmu_cpu || *gmu_cpu < 40 || (sc != 1 && sc != 2))
		return -1;
	if (c == 0)
		return 0;
	n = sc == 1 ? 16 : m && (m->tc & TC_P) ? 2 * HP : HP;
	a &= ~(n - 1);
	if (m && a > m->plimit - n)
		return 0;
	h = m ? m->pbase + a : a;
	do {
		if ((d = leaf(as, h)) != 0 && (*d & 1))
			hcpush((*d & ~(HP - 1)) | (h & (HP - 1)), c, sc == 2);
		h += HP;
	} while (n > HP && (n -= HP) >= HP);
	if ((c & 2) && *gmu_cpu >= 60)
		/* CACR.CABC: the branch cache */
		__asm__ __volatile__(".long 0x4e7a0002\n\t.long 0x00800040\n\t.short 0\n\t.long 0x4e7b0002" : : : "d0");
	guest_cflush_scoped++;
	return 0;
}

/* the guest frame of sz bytes at va in the user space, in one host page, or 0: not mapped for that access */
caddr_t
guest_mmu_user(gp, va, sz, wr)
	struct guest_proc *gp;
	unsigned long va;
	int sz, wr;
{
	struct guest_mmu *m = gp->gp_mmu;
	unsigned long pa;
	int wp, mod, tt;

	if (!m || !m->seg || ((va ^ (va + sz - 1)) & ~(HP - 1)) ||
	    walk(m, va, wr ? S_WRITE : S_READ, 0, 1, &pa, &wp, &mod, &tt) || pa > m->plimit - sz)
		return 0;
	return (caddr_t)(m->pbase + pa);
}

/* guest physical address of va in the current space, for platform modules; -1: none */
int
guest_mmu_xlate(gp, va, rw, pap)
	struct guest_proc *gp;
	unsigned long va, *pap;
	int rw;
{
	struct guest_mmu *m = gp->gp_mmu;
	int wp, mod, tt;

	if (!m || !(m->tc & TC_E)) {
		*pap = va;
		return 0;
	}
	return walk(m, va, rw, (gp->gp_vsr & SR_S) != 0, 0, pap, &wp, &mod, &tt) ? -1 : 0;
}

/* fills, faults with no translation, space switches, flushes, privileged instructions */
void
guest_mmu_stats(gp, v)
	struct guest_proc *gp;
	unsigned long *v;
{
	struct guest_mmu *m = gp->gp_mmu;
	extern int guest_npriv;

	v[0] = m ? m->nfill : 0;
	v[1] = m ? m->nfault : 0;
	v[2] = m ? m->nswitch : 0;
	v[3] = m ? m->nflush : 0;
	v[4] = guest_npriv;
}
