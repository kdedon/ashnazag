/*
 * ammmu.c -- mmu.library for amilib: an adapter onto the kernel's MMU.
 *
 * The real mmu.library takes over the MMU, which AMIX owns, so it cannot
 * run here.  This one owns no tables.  The kernel's mappings stay the
 * authority, and a library's requests reach them through amx_* (the
 * plat_* hooks):
 *
 *   contexts        the default and supervisor ones, and private ones
 *                   (CreateMMUContextA); each keeps the ranges a library
 *                   described on it: properties, destination, window
 *   GetMapping      a MappingNode list: the window range the kernel keeps
 *                   for us (plat_winrange) as the one blank region, the
 *                   rest as mapped
 *   SetPropertiesA  page-aligned, as the real one.  On the default and
 *                   supervisor contexts: accepted inside the mapped boards
 *                   (left as the kernel mapped them) and, as windows,
 *                   inside the window range; refused elsewhere.  On
 *                   private contexts: recorded.
 *   context windows a window shows one of its contexts at a time; what
 *                   that context maps there (remapped to a board: the
 *                   page goes to the board; anything else: no page) is
 *                   applied with amx_remap for the supervisor context's
 *                   windows, the address space the kernel runs in
 *   exception hooks an access fault in the window range builds an
 *                   ExceptionData and runs the active hooks, as the
 *                   library's dispatcher (LVO -396) does
 *   WithoutMMU      the function runs in supervisor state with the MMU
 *                   still on
 *
 * Version: 46 when the kernel has the window hooks (plat_winrange,
 * plat_remap, plat_faulthook), else 43 and no windows, so a library
 * checking the version (openpci) keeps to what works.
 *
 * Offsets up to -360 are MMU_lib.fd 41.1.
 */

#include "amilib.h"

#define	MUTYPE_NONE	0		/* GetMMUType(); callers test nonzero */
#define	MUTYPE_68030	2
#define	MUTYPE_68040	3
#define	MUTYPE_68060	4

/* properties (mmu/context.h) */
#define	MAPP_CACHEINHIBIT	0x00000040
#define	MAPP_BLANK		0x00000800
#define	MAPP_REMAPPED		0x00008000
#define	MAPP_WINDOW		0x80000000

/* SetPropertiesA tags */
#define	MAPTAG_DESTINATION	0x83e00001
#define	MAPTAG_WINDOW		0x83e00006

/* AddContextHookA tags */
#define	MCXH_CONTEXT		0x83e00200
#define	MCXH_TASK		0x83e00201
#define	MCXH_TYPE		0x83e00202
#define	MCXH_CODE		0x83e00203
#define	MCXH_DATA		0x83e00204
#define	MCXH_NAME		0x83e00205
#define	MCXH_PRI		0x83e00206
#define	MCXHTYPE_EXCEPTION	0

/* the hook node: a Node, then data, code, task; ours adds the context */
#define	HK_ACTIVE	2		/* ln_Type of an active hook */
#define	HK_DATA		14
#define	HK_CODE		18
#define	HK_TASK		24
#define	HK_CONTEXT	28
#define	HK_SIZE		32

/* MappingNode: a MinNode, lower, upper (inclusive), data, properties */
#define	MN_LOWER	8
#define	MN_HIGHER	12
#define	MN_DATA		16
#define	MN_PROPS	20
#define	MN_SIZE		28

/* ExceptionData: what openpci's hook and the dispatcher read */
#define	EXD_TASK	0
#define	EXD_CONTEXT	4
#define	EXD_FAULT	16		/* first byte of the access */
#define	EXD_FAULTEND	20		/* its last byte */
#define	EXD_FLAGS	40
#define	EXD_SIZE	52
#define	EXD_MMU		128
#define	EXD_BYTES	136
#define	EXDF_WRITE	2

#define	MW_MAXALT	8

struct mrange {
	struct mrange	*mr_next;
	unsigned long	mr_lo, mr_hi;	/* inclusive */
	unsigned long	mr_props;
	unsigned long	mr_dest;	/* MAPP_REMAPPED: where mr_lo goes */
	struct mwin	*mr_win;	/* MAPP_WINDOW: which window */
};

struct mctx {
	char		mc_node[LN_SIZE];
	struct mctx	*mc_next;
	struct mrange	*mc_ranges;	/* newest first: the newest wins */
	long		mc_locks;
};

struct mwin {
	struct mwin	*mw_next;
	struct mctx	*mw_home;
	struct mctx	*mw_cur;	/* shown now; 0: the home's own */
	struct mctx	*mw_alt[MW_MAXALT];
	int		mw_nalt;
};

struct amlib am_mmu;

static struct mctx *mmu_default, *mmu_super, *mmu_ctxs;
static struct mwin *mmu_wins;
static char *mmu_hooks;			/* exec List of hook nodes */
static int mmu_listlocks;
static int mmu_windows;			/* the kernel has the window hooks */
static unsigned long mmu_winlo, mmu_winhi;

/* ---------------------------------------------------------- helpers */

static struct mctx *
mctx_new(name)
	char *name;
{
	struct mctx *c;

	c = (struct mctx *)am_alloc((unsigned long)sizeof *c,
	    MEMF_PUBLIC | MEMF_CLEAR);
	if (c) {
		AB(c->mc_node, LN_TYPE) = NT_MEMORY;
		AP(c->mc_node, LN_NAME) = name;
		c->mc_next = mmu_ctxs;
		mmu_ctxs = c;
	}
	return c;
}

static int
mmu_isctx(p)
	char *p;
{
	struct mctx *c;

	for (c = mmu_ctxs; c; c = c->mc_next)
		if ((char *)c == p)
			return 1;
	return 0;
}

static int
mmu_iswin(p)
	char *p;
{
	struct mwin *w;

	for (w = mmu_wins; w; w = w->mw_next)
		if ((char *)w == p)
			return 1;
	return 0;
}

/* c's ranges: all of them (w 0), or those bound to window w */
static void
mrange_free(c, w)
	struct mctx *c;
	struct mwin *w;
{
	struct mrange **pp = &c->mc_ranges, *r;

	while ((r = *pp) != 0)
		if (w == 0 || r->mr_win == w) {
			*pp = r->mr_next;
			am_free((char *)r, (unsigned long)sizeof *r);
		} else
			pp = &r->mr_next;
}

/* the newest range of c covering va */
static struct mrange *
mrange_at(c, va)
	struct mctx *c;
	unsigned long va;
{
	struct mrange *r;

	for (r = c ? c->mc_ranges : 0; r; r = r->mr_next)
		if (va >= r->mr_lo && va <= r->mr_hi)
			return r;
	return 0;
}

/* the lowest start of a range of c in (va, end], or end + 1 */
static unsigned long
mrange_next(c, va, end)
	struct mctx *c;
	unsigned long va, end;
{
	struct mrange *r;
	unsigned long n = end + 1;

	for (r = c ? c->mc_ranges : 0; r; r = r->mr_next)
		if (r->mr_lo > va && r->mr_lo <= end && r->mr_lo < n)
			n = r->mr_lo;
	return n;
}

/* a destination, as the library gave it, to a physical address: board
 * addresses are kernel addresses here */
static unsigned long
mmu_destpa(d)
	unsigned long d;
{
	unsigned long pa = am_boardpa((char *)d);

	return pa != ~0UL ? pa : d;
}

/*
 * Show in window range [lo, hi] what the window's current context maps
 * there: remapped pages to their destination, everything else no page.
 */
static void
mmu_applyrange(w, lo, hi)
	struct mwin *w;
	unsigned long lo, hi;
{
	struct mrange *r;
	unsigned long p = lo, end;

	for (;;) {
		r = mrange_at(w->mw_cur, p);
		if (r && (r->mr_props & MAPP_REMAPPED)) {
			end = r->mr_hi < hi ? r->mr_hi : hi;
			(void)amx_remap((char *)p, mmu_destpa(r->mr_dest +
			    (p - r->mr_lo)), end - p + 1, AMX_MAP_IO);
		} else {
			end = mrange_next(w->mw_cur, p, hi) - 1;
			(void)amx_remap((char *)p, 0L, end - p + 1,
			    AMX_MAP_INVALID);
		}
		if (end >= hi)
			break;
		p = end + 1;
	}
}

/* the kernel runs in the supervisor context: its windows are the ones
 * that reach the MMU */
static void
mmu_apply(w)
	struct mwin *w;
{
	struct mrange *r;
	int s;

	if (!mmu_windows || w->mw_home != mmu_super)
		return;
	s = amx_spl7();
	for (r = mmu_super->mc_ranges; r; r = r->mr_next)
		if (r->mr_win == w)
			mmu_applyrange(w, r->mr_lo, r->mr_hi);
	amx_splx(s);
}

/* ------------------------------------------------- the simple calls */

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
	r[D0] = (unsigned long)mmu_default;
}

static void
m_supercontext(r)
	unsigned long *r;
{
	r[D0] = mmu_isctx((char *)r[A0]) ? (unsigned long)mmu_super : 0;
}

static void
m_lockcontext(r)
	unsigned long *r;
{
	if (mmu_isctx((char *)r[A0]))
		((struct mctx *)r[A0])->mc_locks++;
	r[D0] = 1;
}

static void
m_unlockcontext(r)
	unsigned long *r;
{
	struct mctx *c = (struct mctx *)r[A0];

	if (mmu_isctx((char *)c) && c->mc_locks)
		c->mc_locks--;
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

static void
m_createcontext(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)mctx_new("private");
}

static void
m_deletecontext(r)
	unsigned long *r;
{
	struct mctx *c = (struct mctx *)r[A0], **pp;
	struct mwin *w;
	int i;

	if (!mmu_isctx((char *)c) || c == mmu_default || c == mmu_super)
		return;
	for (w = mmu_wins; w; w = w->mw_next) {
		for (i = 0; i < w->mw_nalt; i++)
			if (w->mw_alt[i] == c)
				w->mw_alt[i] = 0;
		if (w->mw_cur == c) {
			w->mw_cur = 0;
			mmu_apply(w);
		}
	}
	mrange_free(c, (struct mwin *)0);
	for (pp = &mmu_ctxs; *pp; pp = &(*pp)->mc_next)
		if (*pp == c) {
			*pp = c->mc_next;
			break;
		}
	am_free((char *)c, (unsigned long)sizeof *c);
}

/* ------------------------------------------------------- mappings */

/* GetMapping(ctx): the window range as the one blank region */
static void
m_getmapping(r)
	unsigned long *r;
{
	unsigned long lo[3], hi[3];
	int n = 0, i;
	char *p, *l, *nd;

	if (!mmu_isctx((char *)r[A0])) {
		r[D0] = 0;
		return;
	}
	if (mmu_windows) {
		lo[0] = 0;
		hi[0] = mmu_winlo - 1;
		lo[1] = mmu_winlo;
		hi[1] = mmu_winhi;
		n = 2;
		if (mmu_winhi != 0xffffffff) {
			lo[2] = mmu_winhi + 1;
			hi[2] = 0xffffffff;
			n = 3;
		}
	} else {
		lo[0] = 0;
		hi[0] = 0xffffffff;
		n = 1;
	}
	p = am_alloc(4L + 12 + (long)n * MN_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
	if (p == 0) {
		r[D0] = 0;
		return;
	}
	AL(p, 0) = 4 + 12 + n * MN_SIZE;
	l = p + 4;
	AP(l, 0) = l + 4;			/* empty MinList */
	AP(l, 8) = l;
	for (i = 0; i < n; i++) {
		nd = l + 12 + i * MN_SIZE;
		AL(nd, MN_LOWER) = lo[i];
		AL(nd, MN_HIGHER) = hi[i];
		AL(nd, MN_PROPS) = (mmu_windows && i == 1) ? MAPP_BLANK : 0;
		am_addtail(l, nd);
	}
	r[D0] = (unsigned long)l;
}

static void
m_releasemapping(r)
	unsigned long *r;
{
	char *l = (char *)r[A1];

	if (l)
		am_free(l - 4, AL(l - 4, 0));
}

static void
m_setpropertylist(r)
	unsigned long *r;
{
	r[D0] = mmu_isctx((char *)r[A0]) && r[A1] ? 1 : 0;
}

static int
mmu_inwin(lo, hi)
	unsigned long lo, hi;
{
	return mmu_windows && lo >= mmu_winlo && hi <= mmu_winhi;
}

/* SetPropertiesA(ctx a0, flags d1, mask d2, lower a1, size d0, tags a2) */
static void
m_setproperties(r)
	unsigned long *r;
{
	struct mctx *c = (struct mctx *)r[A0];
	unsigned long lo = r[A1], size = r[D0], ps = amx_pagesize();
	unsigned long props = r[D1] & r[D2], hi;
	unsigned char *tags = (unsigned char *)r[A2], *t;
	struct mrange *mr;
	struct mwin *w = 0;
	unsigned long dest = 0;

	r[D0] = 0;
	if (c == 0)
		c = mmu_default;
	if (!mmu_isctx((char *)c) || size == 0 || ((lo | size) & (ps - 1)))
		return;
	hi = lo + size - 1;
	if (hi < lo)
		return;
	if (props & MAPP_WINDOW) {
		t = am_tagfind(MAPTAG_WINDOW, tags);
		if (t == 0 || !mmu_iswin((char *)AL(t, 4)))
			return;
		w = (struct mwin *)AL(t, 4);
	} else if (props & MAPP_REMAPPED) {
		t = am_tagfind(MAPTAG_DESTINATION, tags);
		if (t == 0 || (AL(t, 4) & (ps - 1)))
			return;
		dest = AL(t, 4);
	}
	if ((c == mmu_default || c == mmu_super) && w == 0) {
		/* the boards stay as the kernel mapped them */
		if (am_inboard((char *)lo) || am_inboard((char *)hi) ||
		    mmu_inwin(lo, hi)) {
			r[D0] = 1;
			return;
		}
		amx_log("amilib: mmu SetProperties %x+%x %x outside the boards\n",
		    (long)lo, (long)size, (long)props, 0L);
		return;
	}
	if (w && !mmu_inwin(lo, hi)) {
		amx_log("amilib: mmu window %x+%x outside the window range\n",
		    (long)lo, (long)size, 0L, 0L);
		return;
	}
	mr = (struct mrange *)am_alloc((unsigned long)sizeof *mr,
	    MEMF_PUBLIC | MEMF_CLEAR);
	if (mr == 0)
		return;
	mr->mr_lo = lo;
	mr->mr_hi = hi;
	mr->mr_props = props;
	mr->mr_dest = dest;
	mr->mr_win = w;
	mr->mr_next = c->mc_ranges;
	c->mc_ranges = mr;
	r[D0] = 1;
}

static void
m_getproperties(r)
	unsigned long *r;
{
	struct mctx *c = (struct mctx *)r[A0];
	struct mrange *mr;

	mr = mmu_isctx((char *)c) ? mrange_at(c, r[A1]) : 0;
	r[D0] = mr ? mr->mr_props : 0;
}

/* RebuildTree(s): what windows show is brought to the MMU */
static void
m_rebuildtrees(r)
	unsigned long *r;
{
	unsigned long *c = (unsigned long *)r[A0];
	struct mwin *w;

	for (; c && *c; c++)
		if (!mmu_isctx((char *)*c)) {
			r[D0] = 0;
			return;
		}
	for (w = mmu_wins; w; w = w->mw_next)
		mmu_apply(w);
	r[D0] = 1;
}

static void
m_rebuildtree(r)
	unsigned long *r;
{
	unsigned long a[2];

	a[0] = r[A0];
	a[1] = 0;
	r[A0] = (unsigned long)a;
	m_rebuildtrees(r);
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

/* ------------------------------------------------- context windows */

/* -426 CreateContextWindow(home a0, contexts a1: 0-terminated) */
static void
m_createwindow(r)
	unsigned long *r;
{
	struct mctx *home = (struct mctx *)r[A0];
	unsigned long *alt = (unsigned long *)r[A1];
	struct mwin *w;
	int n;

	r[D0] = 0;
	if (!mmu_windows || !mmu_isctx((char *)home) || alt == 0)
		return;
	for (n = 0; alt[n]; n++)
		if (n == MW_MAXALT || !mmu_isctx((char *)alt[n]))
			return;
	w = (struct mwin *)am_alloc((unsigned long)sizeof *w,
	    MEMF_PUBLIC | MEMF_CLEAR);
	if (w == 0)
		return;
	w->mw_home = home;
	for (n = 0; alt[n]; n++)
		w->mw_alt[n] = (struct mctx *)alt[n];
	w->mw_nalt = n;
	w->mw_next = mmu_wins;
	mmu_wins = w;
	r[D0] = (unsigned long)w;
}

/* -432 DeleteContextWindow(window a1): its ranges show no page again */
static void
m_deletewindow(r)
	unsigned long *r;
{
	struct mwin *w = (struct mwin *)r[A1], **pp;

	if (w == 0 || !mmu_iswin((char *)w))
		return;
	w->mw_cur = 0;
	mmu_apply(w);
	mrange_free(mmu_default, w);
	mrange_free(mmu_super, w);
	for (pp = &mmu_wins; *pp; pp = &(*pp)->mw_next)
		if (*pp == w) {
			*pp = w->mw_next;
			break;
		}
	am_free((char *)w, (unsigned long)sizeof *w);
}

/* -438 BuildContextWindow(window a0), -450 LayoutContextWindow(window
 * a0): the real ones build and lay out descriptor tables; here the
 * window is applied as it stands */
static void
m_buildwindow(r)
	unsigned long *r;
{
	struct mwin *w = (struct mwin *)r[A0];

	if (w == 0 || !mmu_iswin((char *)w)) {
		r[D0] = 0;
		return;
	}
	mmu_apply(w);
	r[D0] = 1;
}

/* -444 SetContextWindow(home a0, context a1 or 0, window d1): what the
 * window shows; called from exception hooks, so at any IPL */
static void
m_setwindow(r)
	unsigned long *r;
{
	struct mwin *w = (struct mwin *)r[D1];
	struct mctx *c = (struct mctx *)r[A1];
	int i, s;

	r[D0] = 0;
	if (w == 0 || !mmu_iswin((char *)w))
		return;
	if (c) {
		for (i = 0; i < w->mw_nalt; i++)
			if (w->mw_alt[i] == c)
				break;
		if (i == w->mw_nalt)
			return;
	}
	s = amx_spl7();
	if (w->mw_cur != c) {
		w->mw_cur = c;
		mmu_apply(w);
	}
	amx_splx(s);
	r[D0] = 1;
}

/* ------------------------------------------------- exception hooks */

static void
m_addcontexthook(r)
	unsigned long *r;
{
	unsigned char *tags = (unsigned char *)r[A0], *t;
	char *h;
	int s;

	r[D0] = 0;
	t = am_tagfind(MCXH_TYPE, tags);
	if (t && AL(t, 4) != MCXHTYPE_EXCEPTION) {
		amx_log("amilib: mmu context hook type %d not provided\n",
		    (long)AL(t, 4), 0L, 0L, 0L);
		return;
	}
	if ((t = am_tagfind(MCXH_CODE, tags)) == 0 || AL(t, 4) == 0)
		return;
	h = am_alloc((unsigned long)HK_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
	if (h == 0)
		return;
	AL(h, HK_CODE) = AL(t, 4);
	if ((t = am_tagfind(MCXH_DATA, tags)) != 0)
		AL(h, HK_DATA) = AL(t, 4);
	if ((t = am_tagfind(MCXH_NAME, tags)) != 0)
		AL(h, LN_NAME) = AL(t, 4);
	if ((t = am_tagfind(MCXH_PRI, tags)) != 0)
		AB(h, LN_PRI) = AL(t, 4);
	if ((t = am_tagfind(MCXH_TASK, tags)) != 0)
		AL(h, HK_TASK) = AL(t, 4);
	if ((t = am_tagfind(MCXH_CONTEXT, tags)) != 0)
		AL(h, HK_CONTEXT) = AL(t, 4);
	s = amx_spl7();
	am_enqueue(mmu_hooks, h);
	amx_splx(s);
	r[D0] = (unsigned long)h;
}

static void
m_remcontexthook(r)
	unsigned long *r;
{
	char *h = (char *)r[A1];
	int s;

	if (h == 0)
		return;
	s = amx_spl7();
	am_remove(h);
	amx_splx(s);
	am_free(h, (unsigned long)HK_SIZE);
}

static void
m_activate(r)
	unsigned long *r;
{
	if (r[A1])
		AB((char *)r[A1], LN_TYPE) = HK_ACTIVE;
}

static void
m_deactivate(r)
	unsigned long *r;
{
	if (r[A1])
		AB((char *)r[A1], LN_TYPE) = 0;
}

/*
 * From the kernel's fault path (plat_faulthook): a supervisor access to
 * [va, va+len) in the window range.  0 when a hook repaired it.
 */
static int
mmu_fault(va, len, write)
	unsigned long va, len;
	int write;
{
	unsigned long e[EXD_BYTES / 4], c[16];
	char *x = (char *)e, *h, *task = AP(am_sysbase, EB_THISTASK);
	int i;

	for (i = 0; i < EXD_BYTES / 4; i++)
		e[i] = 0;
	AP(x, EXD_TASK) = task;
	AP(x, EXD_CONTEXT) = (char *)mmu_super;
	AL(x, EXD_FAULT) = va;
	AL(x, EXD_FAULTEND) = va + (len ? len : 1) - 1;
	AL(x, EXD_FLAGS) = write ? EXDF_WRITE : 0;
	AB(x, EXD_SIZE) = len;
	AP(x, EXD_MMU) = am_mmu.al_base;
	for (h = AP(mmu_hooks, LH_HEAD); AP(h, LN_SUCC); h = AP(h, LN_SUCC)) {
		if (AB(h, LN_TYPE) != HK_ACTIVE)
			continue;
		if (AL(h, HK_CONTEXT) && AP(h, HK_CONTEXT) != (char *)mmu_super)
			continue;
		if (AL(h, HK_TASK) && AP(h, HK_TASK) != task)
			continue;
		for (i = 0; i < 15; i++)
			c[i] = 0;
		c[A0] = (unsigned long)x;
		c[A1] = c[A4] = AL(h, HK_DATA);
		c[A6] = (unsigned long)am_mmu.al_base;
		if (am_call(AP(h, HK_CODE), c) == 0)
			return 0;
	}
	return 1;
}

/* ---------------------------------------------------------- the library */

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
	{ -114, m_createcontext },
	{ -120, m_deletecontext },
	{ -144, m_supercontext },
	{ -150, m_defaultcontext },
	{ -168, m_addcontexthook },
	{ -174, m_remcontexthook },
	{ -192, m_activate },
	{ -198, m_deactivate },
	{ -204, m_lockcontext },	/* AttemptLockMMUContext */
	{ -210, m_lockcontextlist },
	{ -216, m_unlockcontextlist },
	{ -222, m_lockcontextlist },	/* AttemptLockContextList */
	{ -228, m_setpropertylist },
	{ -270, m_withoutmmu },
	{ -360, m_rebuildtrees },
	{ -426, m_createwindow },
	{ -432, m_deletewindow },
	{ -438, m_buildwindow },
	{ -444, m_setwindow },
	{ -450, m_buildwindow },	/* LayoutContextWindow */
	{ 0, 0 }
};

struct amlib am_mmu = {
	"mmu.library", NT_LIBRARY, 43, 80, 64, am_mmutab
};

/* the window range the kernel offers, if it is one openpci can use */
static int
mmu_winok(lo, hi)
	unsigned long lo, hi;
{
	unsigned long ps = amx_pagesize();

	return lo < hi && !(lo & (ps - 1)) && !((hi + 1) & (ps - 1)) &&
	    lo >= 0x01000000 && hi < 0x80000000 &&
	    (lo & 0xe0000000) == (hi & 0xe0000000);
}

int
am_mmuinit()
{
	unsigned long lo, hi;

	mmu_ctxs = 0;
	mmu_wins = 0;
	mmu_listlocks = 0;
	mmu_windows = 0;
	if ((mmu_default = mctx_new("default")) == 0 ||
	    (mmu_super = mctx_new("supervisor")) == 0)
		return -1;
	mmu_hooks = am_alloc((unsigned long)LH_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
	if (mmu_hooks == 0)
		return -1;
	am_newlist(mmu_hooks);
	if (amx_winrange(&lo, &hi) == 0) {
		if (mmu_winok(lo, hi) && amx_faulthook(lo, hi, mmu_fault) == 0) {
			mmu_winlo = lo;
			mmu_winhi = hi;
			mmu_windows = 1;
		} else
			amx_log("amilib: window range %x-%x not usable\n",
			    (long)lo, (long)hi, 0L, 0L);
	}
	am_mmu.al_version = mmu_windows ? 46 : 43;
	return am_addlib(&am_mmu) ? 0 : -1;
}

void
am_mmufini()
{
	struct mwin *w;
	struct mctx *c;
	char *h;

	am_dellib(&am_mmu);
	if (mmu_windows) {
		(void)amx_remap((char *)mmu_winlo, 0L, mmu_winhi - mmu_winlo + 1,
		    AMX_MAP_INVALID);
		amx_unfaulthook(mmu_winlo, mmu_winhi);
		mmu_windows = 0;
	}
	while ((w = mmu_wins) != 0) {
		mmu_wins = w->mw_next;
		am_free((char *)w, (unsigned long)sizeof *w);
	}
	while ((c = mmu_ctxs) != 0) {
		mmu_ctxs = c->mc_next;
		mrange_free(c, (struct mwin *)0);
		am_free((char *)c, (unsigned long)sizeof *c);
	}
	if (mmu_hooks) {
		while ((h = am_remhead(mmu_hooks)) != 0)
			am_free(h, (unsigned long)HK_SIZE);
		am_free(mmu_hooks, (unsigned long)LH_SIZE);
		mmu_hooks = 0;
	}
	mmu_default = mmu_super = 0;
}
