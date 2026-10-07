/*
 * ammmu.c -- mmu.library for amilib: an adapter onto the kernel's MMU.
 *
 * The real mmu.library takes over the MMU, which AMIX owns, so it cannot
 * run here.  This one owns no tables: the kernel's mappings (plat_iomap
 * for the boards) stay the authority, and these calls keep the books a
 * library expects around them.
 *
 *   contexts        two, the default and the supervisor one; locks count
 *   GetMapping      an empty MinList the caller hands back to
 *                   SetPropertyList or ReleaseMapping
 *   SetPropertiesA  accepted for any range; the kernel's cache mode for
 *                   it stays as mapped (bridge windows: inhibited,
 *                   serialized).  Requests that would map a range the
 *                   kernel has not mapped are refused.
 *   RebuildTreesA   nothing to rebuild: success
 *   WithoutMMU      the function runs in supervisor state with the MMU
 *                   still on (the kernel's addresses need it); callers
 *                   use it, as openpci does, only for a supervisor call
 *   GetPageSize     the kernel's page size
 *
 * We report version 43: the context windows of V46 (openpci's virtual
 * window for the A1200 Mediators) are not provided, and a library
 * checking the version falls back as it would with an older mmu.library.
 * LVO offsets: MMU_lib.fd 41.1.
 */

#include "amilib.h"

#define	MMU_VERSION	43

/* mmu/mmubase.h: GetMMUType(); callers test for nonzero */
#define	MUTYPE_NONE	0
#define	MUTYPE_68030	2
#define	MUTYPE_68040	3
#define	MUTYPE_68060	4

#define	CTX_SIZE	32		/* ours: a Node, a lock count */
#define	CTX_LOCKS	16

static char *mmu_ctx[2];		/* default, supervisor */
static int mmu_listlocks;

static int
mmu_isctx(c)
	char *c;
{
	return c && (c == mmu_ctx[0] || c == mmu_ctx[1]);
}

static void
m_getmmutype(r)
	unsigned long *r;
{
	int a = AW(am_sysbase, EB_ATTNFLAGS);

	r[D0] = (a & AFF_68060) ? MUTYPE_68060 : (a & AFF_68040) ? MUTYPE_68040 :
	    (a & AFF_68030) ? MUTYPE_68030 : MUTYPE_NONE;
}

static void
m_getpagesize(r)
	unsigned long *r;
{
	r[D0] = amx_pagesize();
}

static void
m_defaultcontext(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)mmu_ctx[0];
}

static void
m_supercontext(r)
	unsigned long *r;
{
	r[D0] = mmu_isctx((char *)r[A0]) ? (unsigned long)mmu_ctx[1] : 0;
}

static void
m_lockcontext(r)
	unsigned long *r;
{
	char *c = (char *)r[A0];

	if (mmu_isctx(c))
		AL(c, CTX_LOCKS)++;
	r[D0] = 1;
}

static void
m_unlockcontext(r)
	unsigned long *r;
{
	char *c = (char *)r[A0];

	if (mmu_isctx(c) && AL(c, CTX_LOCKS))
		AL(c, CTX_LOCKS)--;
}

static void
m_lockcontextlist(r)
	unsigned long *r;
{
	mmu_listlocks++;
	r[D0] = 1;
}

static void
m_unlockcontextlist(r)
	unsigned long *r;
{
	if (mmu_listlocks)
		mmu_listlocks--;
}

/* GetMapping(ctx): a mapping description to hand back later */
static void
m_getmapping(r)
	unsigned long *r;
{
	char *l = 0;

	if (mmu_isctx((char *)r[A0]) &&
	    (l = am_alloc(12L, MEMF_PUBLIC | MEMF_CLEAR)) != 0) {
		AP(l, 0) = l + 4;		/* an empty MinList */
		AP(l, 8) = l;
	}
	r[D0] = (unsigned long)l;
}

static void
m_releasemapping(r)
	unsigned long *r;
{
	if (r[A1])
		am_free((char *)r[A1], 12L);
}

static void
m_setpropertylist(r)
	unsigned long *r;
{
	r[D0] = mmu_isctx((char *)r[A0]) && r[A1] ? 1 : 0;
}

/* SetPropertiesA(ctx, flags d1, mask d2, lower a1, size d0, tags a2) */
static void
m_setproperties(r)
	unsigned long *r;
{
	char *lo = (char *)r[A1];
	unsigned long size = r[D0];

	if (!mmu_isctx((char *)r[A0]) || lo + size < lo) {
		r[D0] = 0;
		return;
	}
	/* the boards are the only ranges a library may describe */
	if (size && !am_inboard(lo) && !am_inboard(lo + size - 1)) {
		amx_log("amilib: mmu SetProperties %x+%x %x outside the boards\n",
		    (long)lo, (long)size, (long)r[D1], 0L);
		r[D0] = 0;
		return;
	}
	r[D0] = 1;
}

/* GetPropertiesA(ctx, lower a1, tags a2): nothing beyond the kernel's */
static void
m_getproperties(r)
	unsigned long *r;
{
	r[D0] = 0;
}

static void
m_rebuildtrees(r)
	unsigned long *r;
{
	unsigned long *c = (unsigned long *)r[A0];

	for (; c && *c; c++)
		if (!mmu_isctx((char *)*c)) {
			r[D0] = 0;
			return;
		}
	r[D0] = 1;
}

static void
m_rebuildtree(r)
	unsigned long *r;
{
	r[D0] = mmu_isctx((char *)r[A0]) ? 1 : 0;
}

/* WithoutMMU(userfunc a5): entered by jsr, as the real one does */
static void
m_withoutmmu(r)
	unsigned long *r;
{
	unsigned long c[16];
	int i;

	for (i = 0; i < 15; i++)
		c[i] = r[i];
	c[A6] = (unsigned long)am_sysbase;
	(void)am_call((char *)r[A5], c);
	r[D0] = c[D0];
}

static struct amfn am_mmutab[] = {
	{ -36, m_getmapping },
	{ -42, m_releasemapping },
	{ -48, m_getpagesize },
	{ -54, m_getmmutype },
	{ -72, m_lockcontext },
	{ -78, m_unlockcontext },
	{ -84, m_setproperties },
	{ -90, m_getproperties },
	{ -96, m_rebuildtree },
	{ -144, m_supercontext },
	{ -150, m_defaultcontext },
	{ -204, m_lockcontext },	/* AttemptLockMMUContext */
	{ -210, m_lockcontextlist },
	{ -216, m_unlockcontextlist },
	{ -222, m_lockcontextlist },	/* AttemptLockContextList */
	{ -228, m_setpropertylist },
	{ -270, m_withoutmmu },
	{ -360, m_rebuildtrees },
	{ 0, 0 }
};

struct amlib am_mmu = {
	"mmu.library", NT_LIBRARY, MMU_VERSION, 80, 64, am_mmutab
};

int
am_mmuinit()
{
	int i;

	for (i = 0; i < 2; i++) {
		mmu_ctx[i] = am_alloc((unsigned long)CTX_SIZE,
		    MEMF_PUBLIC | MEMF_CLEAR);
		if (mmu_ctx[i] == 0)
			return -1;
		AB(mmu_ctx[i], LN_TYPE) = NT_MEMORY;
		AP(mmu_ctx[i], LN_NAME) = i ? "supervisor" : "default";
	}
	mmu_listlocks = 0;
	return am_addlib(&am_mmu) ? 0 : -1;
}

void
am_mmufini()
{
	int i;

	am_dellib(&am_mmu);
	for (i = 0; i < 2; i++)
		if (mmu_ctx[i]) {
			am_free(mmu_ctx[i], (unsigned long)CTX_SIZE);
			mmu_ctx[i] = 0;
		}
}
