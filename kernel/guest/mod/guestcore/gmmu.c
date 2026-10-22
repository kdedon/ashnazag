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
 * access sets U.  Each space keeps its own host tables, so a switch only
 * swaps the window's root entries.  With TC.E clear the window maps
 * identity.
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

extern int as_map(), as_unmap(), as_fault();
extern void hat_unload(), seg_free();
extern int guest_ram_at();
extern paddr_t vtop();
extern unsigned long ds_gen;
__asm__(".weak hat_cm_ram");
extern long hat_cm_ram;
static long *__volatile__ gmu_hat4k = &hat_cm_ram;
__asm__(".weak cputype");
extern long gmu_cputype __asm__("cputype");
static long *__volatile__ gmu_cpu = &gmu_cputype;

unsigned long guest_cflush_all = 0, guest_cflush_scoped = 0;	/* guest cache instructions: whole caches, lines or pages */

#define	HP	4096UL		/* host page, with the 4 KB HAT */
#define	SR_S	0x2000
#define	TC_E	0x8000
#define	TC_P	0x4000		/* 8 KB pages */
#define	NSP	33		/* the supervisor's space and up to 32 users' */
#define	NPOOL	1024		/* table pages: 4 MB */
#define	NHELD	256		/* locked host pages outside the guest's RAM */
#define	TDESC	0x0aUL		/* resident table, U set so the MMU never writes it */
#define	PTSZ	1024		/* pointer table, then its leaves' host addresses */
#define	LFSZ	256
#define	GL	0x800UL		/* PTE reserved bit: the guest's page is global */

/*
 * Each guest address space has its own 040 tables over host frames.
 * The process's root holds the window entries of the space in use;
 * a switch rewrites them and flushes the user ATC.
 */
struct gspace {
	int		inuse;
	unsigned long	key;		/* the guest's root pointer */
	unsigned long	use;		/* last switch to it */
	unsigned long	slot[128];	/* root entries over the window */
	unsigned long	*ptr[128];	/* their pointer tables */
	int		local;		/* holds pages the guest did not mark global */
};

struct guest_mmu {
	struct guest_proc *gp;
	struct seg	*seg;		/* the window, once TC.E was set */
	unsigned long	pbase, plimit, lo, hi;
	unsigned long	tc, srp, urp, tt[4], mmusr;	/* tt: ITT0, ITT1, DTT0, DTT1 */
	int		space;		/* SR_S of the space in use */
	int		cur;		/* space the process root holds; -1: the host's own entries */
	int		nuser;		/* user spaces kept */
	int		first, nslot;	/* the window's root entries */
	int		moved;		/* holds frames of a display session, which may move */
	int		glob;		/* the last walk's page was global */
	struct guest_mmu *next;		/* of those */
	unsigned long	*root, rootpa;
	unsigned long	hsave[128];	/* the host's window entries */
	struct gspace	sp[NSP];
	unsigned long	clock;
	caddr_t		pool[NPOOL];
	int		npool, nheld;
	unsigned long	*freept, *freelf;	/* free tables, linked through their first word */
	unsigned long	held[NHELD];	/* host address | 1 for a write lock */
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
	m->glob = 0;
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
	m->glob = (d & 0x400) != 0;
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

/* pflush (a0) with DFC 1: the user page at va */
static void
atcflush(va)
	unsigned long va;
{
	register unsigned long r __asm__("a0") = va;

	__asm__ __volatile__(".long 0x4e7a0001\n\t.short 0x7201\n\t.long 0x4e7b1001\n\t"
	    ".short 0xf508\n\t.long 0x4e7b0001" : : "a" (r) : "d0", "d1", "memory");
}

/* a zeroed table of sz bytes, its physical address in *pap; 0: none left, or tables were freed while it slept */
static unsigned long *
tnew(m, sz, pap)
	struct guest_mmu *m;
	int sz;
	unsigned long *pap;
{
	unsigned long **fl = sz == PTSZ ? &m->freept : &m->freelf, *t;
	caddr_t pg;
	unsigned long n = m->nflush;
	int i;

	if (!*fl) {
		if (m->npool >= NPOOL)
			return 0;
		if ((pg = (caddr_t)kmem_zalloc(HP, KM_NOSLEEP)) == 0)
			pg = (caddr_t)kmem_zalloc(HP, KM_SLEEP);
		m->pool[m->npool++] = pg;
		for (i = 0; i < HP; i += sz) {
			t = (unsigned long *)(pg + i);
			*t = (unsigned long)*fl;
			*fl = t;
		}
		/* a display move resets every space meanwhile */
		if (n != m->nflush)
			return 0;
	}
	t = *fl;
	*fl = (unsigned long *)*t;
	bzero((caddr_t)t, sz);
	*pap = (unsigned long)vtop((caddr_t)t, (struct proc *)0);
	/* the table walk reads memory */
	hcpush(*pap, 1, 1);
	return t;
}

static void
tfree(m, t, sz)
	struct guest_mmu *m;
	unsigned long *t;
	int sz;
{
	unsigned long **fl = sz == PTSZ ? &m->freept : &m->freelf;

	*t = (unsigned long)*fl;
	*fl = t;
}

/* the process root's window entries from space i, or the host's own for -1 */
static void
setroot(m, i)
	struct guest_mmu *m;
	int i;
{
	unsigned long *s = i < 0 ? m->hsave : m->sp[i].slot, *r = m->root + m->first;
	int k;

	for (k = 0; k < m->nslot; k++)
		if (r[k] != s[k])
			r[k] = s[k];
	hcpush(m->rootpa, 1, 1);
	__asm__ __volatile__(".short 0xf510" : : : "memory");	/* pflushan */
	m->cur = i;
}

static void
spdrop(m, sp)
	struct guest_mmu *m;
	struct gspace *sp;
{
	unsigned long *p;
	int a, b;

	for (a = 0; a < m->nslot; a++) {
		if ((p = sp->ptr[a]) == 0)
			continue;
		for (b = 0; b < 128; b++)
			if (p[b])
				tfree(m, (unsigned long *)p[128 + b], LFSZ);
		tfree(m, p, PTSZ);
		sp->ptr[a] = 0;
		sp->slot[a] = 0;
	}
	sp->inuse = 0;
}

/* every space empty */
static void
reset(m)
	struct guest_mmu *m;
{
	int i;

	for (i = 0; i < NSP; i++)
		if (m->sp[i].inuse)
			spdrop(m, &m->sp[i]);
	if (m->cur >= 0)
		setroot(m, m->cur);
	m->nflush++;
}

/* the PTE of va in sp; 0: none, with mk a new empty one, or 0 when no table is left */
static unsigned long *
pte(m, sp, va, mk, pap)
	struct guest_mmu *m;
	struct gspace *sp;
	unsigned long va, *pap;
	int mk;
{
	int a = (int)(va >> 25) - m->first, b = va >> 18 & 0x7f, c = va >> 12 & 0x3f;
	unsigned long *p, *l, pa;

	if ((p = sp->ptr[a]) == 0) {
		if (!mk || (p = tnew(m, PTSZ, &pa)) == 0)
			return 0;
		sp->ptr[a] = p;
		sp->slot[a] = pa | TDESC;
		if (m->cur == sp - m->sp) {
			m->root[m->first + a] = sp->slot[a];
			hcpush(m->rootpa + (m->first + a) * 4, 1, 0);
		}
	}
	if (!p[b]) {
		if (!mk || (l = tnew(m, LFSZ, &pa)) == 0)
			return 0;
		p[b] = pa | TDESC;
		p[128 + b] = (unsigned long)l;
		hcpush((sp->slot[a] & ~0x1ffUL) + b * 4, 1, 0);
	}
	*pap = (p[b] & ~0xffUL) + c * 4;
	return (unsigned long *)p[128 + b] + c;
}

/* pflushan: the pages the guest did not mark global */
static void
dropnl(m)
	struct guest_mmu *m;
{
	struct gspace *sp;
	unsigned long *p, *l, pa;
	int i, a, b, c, n;

	for (i = 0; i < NSP; i++) {
		sp = &m->sp[i];
		if (!sp->inuse || !sp->local)
			continue;
		for (a = 0, n = 0; a < m->nslot; a++)
			if ((p = sp->ptr[a]) != 0)
				for (b = 0; b < 128; b++)
					if (p[b])
						for (l = (unsigned long *)p[128 + b], pa = p[b] & ~0xffUL, c = 0; c < 64; c++)
							if (l[c] && !(l[c] & GL)) {
								l[c] = 0;
								hcpush(pa + c * 4, 1, 0);
								n++;
							}
		sp->local = 0;
		if (n && m->cur == i)
			__asm__ __volatile__(".short 0xf510" : : : "memory");	/* pflushan */
	}
	m->nflush++;
}

/* drop va's page from every space */
static void
pdrop(m, va)
	struct guest_mmu *m;
	unsigned long va;
{
	unsigned long *d, pa;
	int i;

	for (i = 0; i < NSP; i++)
		if (m->sp[i].inuse && (d = pte(m, &m->sp[i], va, 0, &pa)) != 0 && *d) {
			*d = 0;
			hcpush(pa, 1, 0);
			if (m->cur == i)
				atcflush(va);
		}
}

/* the space for vSR.S s, its entries in the process root */
static void
setspace(m, s)
	struct guest_mmu *m;
	int s;
{
	struct gspace *sp;
	int i, j;

	if (m->cur >= 0 && m->space == s && (s || m->sp[m->cur].key == m->urp) && m->sp[m->cur].inuse)
		return;
	if (s)
		i = 0;
	else {
		for (i = 1, j = 1; i <= m->nuser; i++) {
			if (m->sp[i].inuse && m->sp[i].key == m->urp)
				break;
			if (!m->sp[i].inuse || (m->sp[j].inuse && m->sp[i].use < m->sp[j].use))
				j = i;
		}
		if (i > m->nuser) {
			i = j;
			if (m->sp[i].inuse) {
				spdrop(m, &m->sp[i]);
				if (m->cur == i)
					m->cur = -2;	/* the root names freed tables */
			}
		}
	}
	sp = &m->sp[i];
	sp->inuse = 1;
	sp->key = s ? m->srp : m->urp;
	sp->use = ++m->clock;
	if (m->space != s)
		m->nswitch++;
	m->space = s;
	if (m->cur != i)
		setroot(m, i);
}

static void
unhold(m, as)
	struct guest_mmu *m;
	struct as *as;
{
	while (m->nheld > 0) {
		m->nheld--;
		(void)as_fault(as, (addr_t)(m->held[m->nheld] & ~1UL), (u_int)HP, F_SOFTUNLOCK,
		    m->held[m->nheld] & 1 ? S_WRITE : S_READ);
	}
}

/* lock a host page outside the guest's RAM while tables may map it: 1 writable, 0 read-only, -1 not lockable */
static int
hold(m, as, h)
	struct guest_mmu *m;
	struct as *as;
	unsigned long h;
{
	int i, wr;

	for (i = 0; i < m->nheld; i++)
		if ((m->held[i] & ~1UL) == h)
			return (int)(m->held[i] & 1);
	if (m->nheld == NHELD) {
		reset(m);
		unhold(m, as);
	}
	if (!as_fault(as, (addr_t)h, (u_int)HP, F_SOFTLOCK, S_WRITE))
		wr = 1;
	else if (!as_fault(as, (addr_t)h, (u_int)HP, F_SOFTLOCK, S_READ))
		wr = 0;
	else
		return -1;
	m->held[m->nheld++] = h | wr;
	return wr;
}

/* the host's entries back, every table freed */
static void
release(m)
	struct guest_mmu *m;
{
	struct as *as = m->gp->gp_proc->p_as;
	int i;

	if (m->root && as && *(unsigned long **)((char *)as + 20) == m->root) {
		if (m->cur >= 0)
			setroot(m, -1);
		unhold(m, as);
	}
	for (i = 0; i < NSP; i++)
		if (m->sp[i].inuse)
			spdrop(m, &m->sp[i]);
	while (m->npool > 0)
		kmem_free((_VOID *)m->pool[--m->npool], HP);
	m->freept = m->freelf = 0;
	m->nheld = 0;
	m->root = 0;
	m->cur = -1;
}

/* one host page of the window */
static faultcode_t
fill(seg, m, va, rw, lock)
	struct seg *seg;
	struct guest_mmu *m;
	unsigned long va;
	enum seg_rw rw;
	int lock;
{
	struct as *as = seg->s_as;
	unsigned long pa, h, *d, *hd, dpa, cm, gen = ds_gen;
	long pf;
	int e, wp, mod, tt, moved = 0, wl;

	setspace(m, (int)(m->gp->gp_vsr & SR_S));
	if ((e = walk(m, va, (int)rw, m->space != 0, 1, &pa, &wp, &mod, &tt)) != 0) {
		m->nfault++;
		return e;
	}
	if (pa >= m->plimit)
		return FC_NOMAP;
	h = m->pbase + (pa & ~(HP - 1));
	if ((pf = guest_ram_at(as, (addr_t)h)) < 0) {
		if ((wl = hold(m, as, h)) == 0) {
			if (rw == S_WRITE)
				return FC_PROT;
			mod = 0;
		} else if (wl < 0) {
			if (lock || as_fault(as, (addr_t)h, (u_int)HP, F_INVAL, S_WRITE))
				return FC_NOMAP;
			/* a display session's pages refuse locking; it tells when they move */
			moved = 1;
		}
		if ((pf = hostpfn(as, h)) < 0)
			return FC_NOMAP;
	}
	/* hold() may have emptied every space */
	setspace(m, m->space);
	/* the host's caching of the frame: write-through VRAM, copyback RAM */
	cm = (hd = leaf(as, h)) != 0 && (*hd & 1) ? *hd & 0x60 : *gmu_hat4k & 0x60;
	if ((d = pte(m, &m->sp[m->cur], va, 1, &dpa)) == 0) {
		reset(m);
		setspace(m, m->space);
		if ((d = pte(m, &m->sp[m->cur], va, 1, &dpa)) == 0)
			return FC_NOMAP;
	}
	*d = (unsigned long)pf * HP | cm | 8 | (mod ? 0x10 : 4) | (m->glob ? GL : 0) | 1;
	if (!m->glob)
		m->sp[m->cur].local = 1;
	hcpush(dpa, 1, 0);
	atcflush(va);
	if (moved) {
		if (!m->moved) {
			m->moved = 1;
			m->next = gmu_moved;
			gmu_moved = m;
		}
		/* moved while loading: the access faults again */
		if (gen != ds_gen) {
			*d = 0;
			hcpush(dpa, 1, 0);
			atcflush(va);
		}
	}
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
		if ((e = fill(seg, m, va, rw, type == F_SOFTLOCK)) != 0)
			return e;
	return 0;
}

/* a child gets an empty window; the parent's tree goes back to the host's for the copy */
static int
gmu_dup(seg, nseg)
	struct seg *seg, *nseg;
{
	struct guest_mmu *m = (struct guest_mmu *)seg->s_data;

	if (m && m->cur >= 0)
		setroot(m, -1);
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
		release(m);
		m->seg = 0;
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
	if (seg->s_data)
		release((struct guest_mmu *)seg->s_data);
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
	m->root = *(unsigned long **)((char *)as + 20);
	m->rootpa = (unsigned long)vtop((caddr_t)m->root, (struct proc *)0);
	bcopy((caddr_t)(m->root + m->first), (caddr_t)m->hsave, m->nslot * 4);
	m->cur = -1;
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
	if (!gmu_hat4k || ((pbase | plimit) & (HP - 1)) || ((lo | hi) & 0x1ffffff) || hi <= lo ||
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
	m->first = (int)(lo >> 25);
	m->nslot = (int)((hi - lo) >> 25);
	m->cur = -1;
	m->nuser = 1;
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
	if (m->seg) {
		release(m);
		m->seg->s_data = 0;
	}
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
		if (m->seg)
			reset(m);
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
	/* a new URP takes effect at the next switch to user mode */
	if (old != *cp && m->seg && (c != 0x806 || m->space == 0) && c != 0x805)
		reset(m);
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
		if (!m->seg)
			return 0;
		if ((op & 0x18) == 0x18)
			reset(m);
		else if (op & 0x10)
			dropnl(m);
		else if (a >= m->lo && a < m->hi) {
			pdrop(m, a);
			if (m->tc & TC_P)
				pdrop(m, a ^ HP);
		}
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

/* drop [s, e) from space i */
static void
rdrop(m, i, s, e)
	struct guest_mmu *m;
	int i;
	unsigned long s, e;
{
	struct gspace *sp = &m->sp[i];
	unsigned long *p, *l, pa, va;
	int n = 0;

	if (s < m->lo)
		s = m->lo;
	if (e > m->hi || e < s)
		e = m->hi;
	for (va = s & ~(HP - 1); va < e; ) {
		if ((p = sp->ptr[(va >> 25) - m->first]) == 0) {
			va = (va | 0x1ffffffUL) + 1;
			continue;
		}
		if (!p[va >> 18 & 0x7f]) {
			va = (va | 0x3ffffUL) + 1;
			continue;
		}
		l = (unsigned long *)p[128 + (va >> 18 & 0x7f)] + (va >> 12 & 0x3f);
		pa = (p[va >> 18 & 0x7f] & ~0xffUL) + (va >> 12 & 0x3f) * 4;
		if (*l) {
			*l = 0;
			hcpush(pa, 1, 0);
			if (m->cur == i && ++n <= 16)
				atcflush(va);
		}
		if (va + HP < va)
			break;
		va += HP;
	}
	if (n > 16)
		__asm__ __volatile__(".short 0xf510" : : : "memory");	/* pflushan */
}

/*
 * The guest's pmap, for a kernel whose own invalidation is replaced: 1 it
 * says what changes from now on, 2 drop [s, e) in the space of root key
 * (0: the supervisor's), 3 the space of key goes.  With pa, the caches
 * are pushed over the e - s bytes of guest physical memory there, as the
 * flushes it replaces did for a new mapping's code.
 */
int
guest_mmu_svc(gp, op, key, s, e, pa)
	struct guest_proc *gp;
	int op;
	unsigned long key, s, e, pa;
{
	struct guest_mmu *m = gp->gp_mmu;
	unsigned long o;
	int i;

	if (!m)
		return ENXIO;
	if (op == 1) {
		m->nuser = NSP - 1;
		return 0;
	}
	if (pa)
		for (o = 0; o < e - s && o < 0x10000; o += HP)
			(void)guest_cache_insn(gp, 0xf4f0, pa + o);	/* cpushp bc */
	if (!m->seg)
		return 0;
	for (i = 0; i < NSP; i++) {
		if (!m->sp[i].inuse || (key ? i == 0 || m->sp[i].key != key : i != 0))
			continue;
		if (op == 2)
			rdrop(m, i, s, e);
		else if (op == 3 && i) {
			spdrop(m, &m->sp[i]);
			if (m->cur == i)
				setroot(m, i);
		}
	}
	return 0;
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
