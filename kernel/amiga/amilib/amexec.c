/*
 * amexec.c -- the exec.library an AmigaOS library sees inside the kernel.
 *
 * One task (the caller of the module), no scheduler: Forbid/Permit only
 * count, Disable/Enable raise the IPL to 7, semaphores never block (the
 * user of amilib serialises its calls), Wait returns what it was asked
 * for.  Devices complete their requests in BeginIO.  Memory comes from
 * the host kernel; chip memory is never available.
 *
 * What a library asks for and we do not provide is logged once per LVO
 * and returns 0, so a new library version shows its needs on the
 * console instead of crashing.
 */

#include "amilib.h"

#define	EXEC_NLVO	140
#define	AM_SIGFREE	0xffff0000	/* signals 16-31 for AllocSignal */

static void x_lopen(), x_lclose(), x_lnull();

#define	AM_STACK	16384

char *am_sysbase;
int am_intmask;
int am_users;				/* am_init calls not yet undone */
char *am_stkbot, *am_stktop;		/* amglue.s switches to it */

static char *am_task;			/* our one Task */
static char *am_ints;			/* AM_NINT server Lists */
static struct amlib *am_libs[10];
static int am_nlibs;
static int am_dis, am_dissr, am_forb;
static long am_inuse;			/* bytes from AllocMem, for checks */
static unsigned char am_said[EXEC_NLVO];	/* unprovided LVOs logged */
static int am_attn;

/* ------------------------------------------------------------ helpers */

static void
am_zero(p, n)
	char *p;
	long n;
{
	while (n-- > 0)
		*p++ = 0;
}

static void
am_copy(s, d, n)
	char *s, *d;
	long n;
{
	if (d < s || d >= s + n)
		while (n-- > 0)
			*d++ = *s++;
	else
		for (s += n, d += n; n-- > 0; )
			*--d = *--s;
}

static int
am_lower(c)
	int c;
{
	c &= 0xff;
	if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
		c += 0x20;
	return c;
}

int
am_stricmp(a, b, n)
	char *a, *b;
	long n;				/* -1: no limit */
{
	int ca, cb;

	for (; n != 0; n--) {
		ca = am_lower(*a++);
		cb = am_lower(*b++);
		if (ca != cb)
			return ca < cb ? -1 : 1;
		if (ca == 0)
			break;
	}
	return 0;
}

static int
am_strcmp(a, b)
	char *a, *b;
{
	while (*a && *a == *b)
		a++, b++;
	return (*a & 0xff) - (*b & 0xff);
}

static int
cansleep()
{
	return am_dis == 0 && amx_ipl() == 0;
}

/* ------------------------------------------------------------- lists */

void
am_newlist(l)
	char *l;
{
	AP(l, LH_HEAD) = l + LH_TAIL;
	AL(l, LH_TAIL) = 0;
	AP(l, LH_TAILPRED) = l;
}

void
am_addhead(l, n)
	char *l, *n;
{
	char *h = AP(l, LH_HEAD);

	AP(n, LN_SUCC) = h;
	AP(n, LN_PRED) = l;
	AP(h, LN_PRED) = n;
	AP(l, LH_HEAD) = n;
}

void
am_addtail(l, n)
	char *l, *n;
{
	char *t = AP(l, LH_TAILPRED);

	AP(n, LN_SUCC) = l + LH_TAIL;
	AP(n, LN_PRED) = t;
	AP(t, LN_SUCC) = n;
	AP(l, LH_TAILPRED) = n;
}

void
am_remove(n)
	char *n;
{
	char *s = AP(n, LN_SUCC), *p = AP(n, LN_PRED);

	AP(p, LN_SUCC) = s;
	AP(s, LN_PRED) = p;
}

char *
am_remhead(l)
	char *l;
{
	char *h = AP(l, LH_HEAD);

	if (AP(h, LN_SUCC) == 0)
		return 0;
	am_remove(h);
	return h;
}

static char *
am_remtail(l)
	char *l;
{
	char *t = AP(l, LH_TAILPRED);

	if (AP(t, LN_PRED) == 0)
		return 0;
	am_remove(t);
	return t;
}

static void
am_insert(l, n, pred)
	char *l, *n, *pred;
{
	char *s;

	if (pred == 0) {
		am_addhead(l, n);
		return;
	}
	s = AP(pred, LN_SUCC);
	if (s) {
		AP(n, LN_SUCC) = s;
		AP(n, LN_PRED) = pred;
		AP(s, LN_PRED) = n;
		AP(pred, LN_SUCC) = n;
	} else {			/* pred is the list's tail node */
		AP(n, LN_SUCC) = pred;
		AP(n, LN_PRED) = AP(pred, LN_PRED);
		AP(AP(pred, LN_PRED), LN_SUCC) = n;
		AP(pred, LN_PRED) = n;
	}
}

void
am_enqueue(l, n)
	char *l, *n;
{
	char *x;
	int pri = (signed char)AB(n, LN_PRI);

	for (x = AP(l, LH_HEAD); AP(x, LN_SUCC); x = AP(x, LN_SUCC))
		if ((signed char)AB(x, LN_PRI) < pri)
			break;
	AP(n, LN_SUCC) = x;
	AP(n, LN_PRED) = AP(x, LN_PRED);
	AP(AP(x, LN_PRED), LN_SUCC) = n;
	AP(x, LN_PRED) = n;
}

/* FindName: from a list header or from a node, as exec */
char *
am_findname(start, name)
	char *start, *name;
{
	char *n;

	for (n = AP(start, LN_SUCC); n && AP(n, LN_SUCC); n = AP(n, LN_SUCC))
		if (AP(n, LN_NAME) && am_strcmp(AP(n, LN_NAME), name) == 0)
			return n;
	return 0;
}

/* ------------------------------------------------------------ memory */

char *
am_alloc(size, flags)
	unsigned long size, flags;
{
	char *p;

	if (size == 0 || size > 0x10000000)
		return 0;
	if (flags & MEMF_CHIP) {
		amx_log("amilib: AllocMem %d bytes of chip memory refused\n",
		    (long)size, 0L, 0L, 0L);
		return 0;
	}
	size = (size + 7) & ~7;
	p = amx_alloc(size, cansleep());
	if (p == 0)
		return 0;
	if (flags & MEMF_CLEAR)
		am_zero(p, (long)size);
	am_inuse += size;
	return p;
}

void
am_free(p, size)
	char *p;
	unsigned long size;
{
	if (p == 0 || size == 0)
		return;
	size = (size + 7) & ~7;
	amx_free(p, size);
	am_inuse -= size;
}

long
am_lvo(base, lvo, r)
	char *base;
	int lvo;
	unsigned long *r;
{
	r[A6] = (unsigned long)base;
	return am_call(base + lvo, r);
}

/* ------------------------------------------------------- our libraries */

/*
 * One block: jump table (each entry "jmp stub"), the base and its
 * positive part, then the stubs ("jsr am_entry").  SetFunction may
 * replace a jump table entry; the old function it returns is the stub,
 * which still reaches our handler.
 */
char *
am_addlib(al)
	struct amlib *al;
{
	long neg = ((long)al->al_nlvo * 6 + 3) & ~3;
	long pos = (al->al_size + 3) & ~3;
	long fns = (long)al->al_nlvo * sizeof (void (*)());
	struct amfn *f;
	char *m, *b;
	int i;

	al->al_memsize = neg + pos + (long)al->al_nlvo * 6 + fns;
	m = am_alloc(al->al_memsize, MEMF_PUBLIC | MEMF_CLEAR);
	if (m == 0)
		return 0;
	al->al_mem = m;
	al->al_base = b = m + neg;
	al->al_stub = b + pos;
	al->al_fn = (void (**)())(al->al_stub + (long)al->al_nlvo * 6);
	for (i = 0; i < al->al_nlvo; i++) {
		AW(b, -6 * (i + 1)) = 0x4ef9;			/* jmp */
		AP(b, -6 * (i + 1) + 2) = al->al_stub + 6 * i;
		AW(al->al_stub, 6 * i) = 0x4eb9;		/* jsr */
		AP(al->al_stub, 6 * i + 2) = (char *)am_entry;
	}
	al->al_fn[0] = x_lopen;
	al->al_fn[1] = x_lclose;
	al->al_fn[2] = x_lnull;		/* Expunge: we stay */
	al->al_fn[3] = x_lnull;
	for (f = al->al_tab; f->af_fn; f++)
		if (f->af_lvo < 0 && -f->af_lvo / 6 <= al->al_nlvo)
			al->al_fn[-f->af_lvo / 6 - 1] = f->af_fn;
	AB(b, LN_TYPE) = al->al_type;
	AP(b, LN_NAME) = al->al_name;
	AW(b, LIB_NEGSIZE) = neg;
	AW(b, LIB_POSSIZE) = pos;
	AW(b, LIB_VERSION) = al->al_version;
	AP(b, LIB_IDSTRING) = al->al_name;
	amx_cacheflush();
	if (am_nlibs < sizeof am_libs / sizeof am_libs[0])
		am_libs[am_nlibs++] = al;
	if (am_sysbase && al->al_type == NT_LIBRARY)
		am_addtail(am_sysbase + EB_LIBLIST, b);
	else if (am_sysbase && al->al_type == NT_DEVICE)
		am_addtail(am_sysbase + EB_DEVICELIST, b);
	return b;
}

void
am_dellib(al)
	struct amlib *al;
{
	int i;

	if (al->al_mem == 0)
		return;
	for (i = 0; i < am_nlibs; i++)
		if (am_libs[i] == al) {
			am_libs[i] = am_libs[--am_nlibs];
			break;
		}
	am_free(al->al_mem, al->al_memsize);
	al->al_mem = al->al_base = al->al_stub = 0;
}

void
am_dispatch(r)
	unsigned long *r;
{
	char *s = (char *)r[R_STUB] - 6;
	struct amlib *al;
	int i, k;

	for (i = 0; i < am_nlibs; i++) {
		al = am_libs[i];
		if (s >= al->al_stub && s < al->al_stub + 6 * al->al_nlvo) {
			k = (s - al->al_stub) / 6;
			if (al->al_fn[k]) {
				al->al_fn[k](r);
				return;
			}
			if (al != am_libs[0] || k >= EXEC_NLVO || !am_said[k]) {
				amx_log("amilib: %s LVO -%d not provided\n",
				    (long)al->al_name, (long)(6 * (k + 1)), 0L, 0L);
				if (al == am_libs[0] && k < EXEC_NLVO)
					am_said[k] = 1;
			}
			r[D0] = 0;
			return;
		}
	}
	amx_log("amilib: call from an unknown stub %x\n", (long)s, 0L, 0L, 0L);
	r[D0] = 0;
}

/* Open, Close, Expunge and the reserved vector of our libraries */
static void
x_lopen(r)
	unsigned long *r;
{
	char *b = (char *)r[A6];

	AW(b, LIB_OPENCNT)++;
	r[D0] = (unsigned long)b;
}

static void
x_lclose(r)
	unsigned long *r;
{
	char *b = (char *)r[A6];

	if (AW(b, LIB_OPENCNT))
		AW(b, LIB_OPENCNT)--;
	r[D0] = 0;
}

static void
x_lnull(r)
	unsigned long *r;
{
	r[D0] = 0;
}

/* --------------------------------------------------------- exec calls */

static void
x_supervisor(r)
	unsigned long *r;
{
	(void)am_super(r);
}

static void
x_disable(r)
	unsigned long *r;
{
	int s = amx_spl7();

	if (am_dis++ == 0)
		am_dissr = s;
	AB(am_sysbase, EB_IDNESTCNT)++;
}

static void
x_enable(r)
	unsigned long *r;
{
	if (am_dis == 0)
		return;
	AB(am_sysbase, EB_IDNESTCNT)--;
	if (--am_dis == 0)
		amx_splx(am_dissr);
}

static void
x_forbid(r)
	unsigned long *r;
{
	am_forb++;
	AB(am_sysbase, EB_TDNESTCNT)++;
}

static void
x_permit(r)
	unsigned long *r;
{
	if (am_forb) {
		am_forb--;
		AB(am_sysbase, EB_TDNESTCNT)--;
	}
}

/* SetSR: report the SR, change nothing */
static void
x_setsr(r)
	unsigned long *r;
{
	if (r[D1] & 0xffff)
		amx_log("amilib: SetSR %x/%x ignored\n", (long)r[D0], (long)r[D1],
		    0L, 0L);
	r[D0] = 0x2000 | (amx_ipl() << 8);
}

/* SuperState: already in supervisor state, which returns 0 */
static void
x_superstate(r)
	unsigned long *r;
{
	r[D0] = 0;
}

static void
x_cause(r)
	unsigned long *r;
{
	char *is = (char *)r[A1];
	unsigned long c[16];

	am_zero((char *)c, (long)sizeof c);
	c[A1] = AL(is, IS_DATA);
	c[A5] = AL(is, IS_CODE);
	c[A6] = (unsigned long)am_sysbase;
	(void)am_call((char *)AL(is, IS_CODE), c);
}

static void
x_addintserver(r)
	unsigned long *r;
{
	unsigned long n = r[D0];
	char *l;
	int s, first;

	r[D0] = 0;
	if (n >= AM_NINT)
		return;
	l = am_ints + n * LH_SIZE;
	s = amx_spl7();
	first = AP(AP(l, LH_HEAD), LN_SUCC) == 0;
	am_enqueue(l, (char *)r[A1]);
	am_intmask |= 1 << n;
	amx_splx(s);
	if (first && amx_intattach((int)n) != 0)
		amx_log("amilib: no host route for interrupt chain %d\n", (long)n,
		    0L, 0L, 0L);
}

static void
x_remintserver(r)
	unsigned long *r;
{
	unsigned long n = r[D0];
	char *l;
	int s, last;

	r[D0] = 0;
	if (n >= AM_NINT)
		return;
	l = am_ints + n * LH_SIZE;
	s = amx_spl7();
	am_remove((char *)r[A1]);
	last = AP(AP(l, LH_HEAD), LN_SUCC) == 0;
	if (last)
		am_intmask &= ~(1 << n);
	amx_splx(s);
	if (last)
		amx_intdetach((int)n);
}

/* a server chain, from the host's interrupt path: 1 if a server took it */
int
am_intrun(n)
	int n;
{
	unsigned long c[16];
	char *is;

	if (n < 0 || n >= AM_NINT || !am_ints)
		return 0;
	for (is = AP(am_ints + n * LH_SIZE, LH_HEAD); AP(is, LN_SUCC);
	    is = AP(is, LN_SUCC)) {
		c[A1] = AL(is, IS_DATA);
		c[A5] = AL(is, IS_CODE);
		c[A6] = (unsigned long)am_sysbase;
		c[D0] = 0;
		if (am_call((char *)AL(is, IS_CODE), c) != 0)
			return 1;
	}
	return 0;
}

static void
x_allocmem(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_alloc(r[D0], r[D1]);
}

static void
x_freemem(r)
	unsigned long *r;
{
	am_free((char *)r[A1], r[D0]);
}

static void
x_allocvec(r)
	unsigned long *r;
{
	char *p = am_alloc(r[D0] + 8, r[D1]);

	if (p) {
		AL(p, 0) = r[D0] + 8;
		p += 8;
	}
	r[D0] = (unsigned long)p;
}

static void
x_freevec(r)
	unsigned long *r;
{
	char *p = (char *)r[A1];

	if (p)
		am_free(p - 8, AL(p - 8, 0));
}

static void
x_availmem(r)
	unsigned long *r;
{
	r[D0] = (r[D1] & MEMF_CHIP) ? 0 : 0x400000;
}

static void
x_typeofmem(r)
	unsigned long *r;
{
	r[D0] = MEMF_PUBLIC | MEMF_FAST;
}

static void
x_allocabs(r)
	unsigned long *r;
{
	amx_log("amilib: AllocAbs %d at %x refused\n", (long)r[D0], (long)r[A1],
	    0L, 0L);
	r[D0] = 0;
}

static void
x_copymem(r)
	unsigned long *r;
{
	am_copy((char *)r[A0], (char *)r[A1], (long)r[D0]);
}

/* pools: a List of AllocVec'd puddles of one allocation each */
static void
x_createpool(r)
	unsigned long *r;
{
	char *l = am_alloc((unsigned long)LH_SIZE + 4, MEMF_PUBLIC | MEMF_CLEAR);

	if (l) {
		am_newlist(l);
		AL(l, LH_SIZE) = r[D0] & ~MEMF_CLEAR;
	}
	r[D0] = (unsigned long)l;
}

static void
x_allocpooled(r)
	unsigned long *r;
{
	char *l = (char *)r[A0], *p;

	p = l ? am_alloc(r[D0] + 16, AL(l, LH_SIZE) | MEMF_CLEAR) : 0;
	if (p) {
		AL(p, 8) = r[D0] + 16;
		am_addtail(l, p);
		p += 16;
	}
	r[D0] = (unsigned long)p;
}

static void
x_freepooled(r)
	unsigned long *r;
{
	char *p = (char *)r[A1];

	if (p) {
		p -= 16;
		am_remove(p);
		am_free(p, AL(p, 8));
	}
}

static void
x_deletepool(r)
	unsigned long *r;
{
	char *l = (char *)r[A0], *p;

	if (l == 0)
		return;
	while ((p = am_remhead(l)) != 0)
		am_free(p, AL(p, 8));
	am_free(l, (unsigned long)LH_SIZE + 4);
}

static void
x_insert(r)
	unsigned long *r;
{
	am_insert((char *)r[A0], (char *)r[A1], (char *)r[A2]);
}

static void
x_addhead(r)
	unsigned long *r;
{
	am_addhead((char *)r[A0], (char *)r[A1]);
}

static void
x_addtail(r)
	unsigned long *r;
{
	am_addtail((char *)r[A0], (char *)r[A1]);
}

static void
x_remove(r)
	unsigned long *r;
{
	am_remove((char *)r[A1]);
}

static void
x_remhead(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_remhead((char *)r[A0]);
}

static void
x_remtail(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_remtail((char *)r[A0]);
}

static void
x_enqueue(r)
	unsigned long *r;
{
	am_enqueue((char *)r[A0], (char *)r[A1]);
}

static void
x_findname(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_findname((char *)r[A0], (char *)r[A1]);
}

/* ------------------------------------------------- task, signals, ports */

static void
x_findtask(r)
	unsigned long *r;
{
	char *n = (char *)r[A1];

	r[D0] = (n == 0 || am_strcmp(n, AP(am_task, LN_NAME)) == 0) ?
	    (unsigned long)am_task : 0;
}

static long
am_allocsignal(n)
	long n;
{
	unsigned long a = AL(am_task, TC_SIGALLOC);

	if (n < 0) {
		for (n = 31; n >= 16; n--)
			if (!(a & (1L << n)))
				break;
		if (n < 16)
			return -1;
	} else if (n > 31 || (a & (1L << n)))
		return -1;
	AL(am_task, TC_SIGALLOC) = a | (1L << n);
	AL(am_task, TC_SIGRECVD) &= ~(1L << n);
	return n;
}

static void
x_allocsignal(r)
	unsigned long *r;
{
	r[D0] = am_allocsignal((long)(signed char)r[D0]);
}

static void
x_freesignal(r)
	unsigned long *r;
{
	long n = (long)r[D0];

	if (n >= 16 && n <= 31)
		AL(am_task, TC_SIGALLOC) &= ~(1L << n);
}

static void
x_setsignal(r)
	unsigned long *r;
{
	unsigned long o = AL(am_task, TC_SIGRECVD);

	AL(am_task, TC_SIGRECVD) = (o & ~r[D1]) | (r[D0] & r[D1]);
	r[D0] = o;
}

static void
x_signal(r)
	unsigned long *r;
{
	if ((char *)r[A1] == am_task)
		AL(am_task, TC_SIGRECVD) |= r[D0];
}

/* Wait: nothing else runs to send a signal; never block */
static void
x_wait(r)
	unsigned long *r;
{
	unsigned long m = r[D0], g = AL(am_task, TC_SIGRECVD) & m;

	if (g == 0) {
		amx_log("amilib: Wait %x with no signal pending\n", (long)m, 0L,
		    0L, 0L);
		g = m;
	}
	AL(am_task, TC_SIGRECVD) &= ~g;
	r[D0] = g;
}

static void
x_createmsgport(r)
	unsigned long *r;
{
	char *p = am_alloc((unsigned long)MP_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
	long s;

	if (p) {
		if ((s = am_allocsignal(-1L)) < 0) {
			am_free(p, (unsigned long)MP_SIZE);
			p = 0;
		} else {
			AB(p, LN_TYPE) = NT_MSGPORT;
			AB(p, MP_SIGBIT) = s;
			AP(p, MP_SIGTASK) = am_task;
			am_newlist(p + MP_MSGLIST);
		}
	}
	r[D0] = (unsigned long)p;
}

static void
x_deletemsgport(r)
	unsigned long *r;
{
	char *p = (char *)r[A0];

	if (p) {
		AL(am_task, TC_SIGALLOC) &= ~(1L << AB(p, MP_SIGBIT));
		am_free(p, (unsigned long)MP_SIZE);
	}
}

static void
am_putmsg(p, m)
	char *p, *m;
{
	am_addtail(p + MP_MSGLIST, m);
	if (AP(p, MP_SIGTASK) == am_task && AB(p, MP_FLAGS) == 0)
		AL(am_task, TC_SIGRECVD) |= 1L << AB(p, MP_SIGBIT);
}

static void
x_putmsg(r)
	unsigned long *r;
{
	AB((char *)r[A1], LN_TYPE) = NT_MESSAGE;
	am_putmsg((char *)r[A0], (char *)r[A1]);
}

static void
x_getmsg(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_remhead((char *)r[A0] + MP_MSGLIST);
}

static void
am_replymsg(m)
	char *m;
{
	char *p = AP(m, MN_REPLYPORT);

	if (p) {
		AB(m, LN_TYPE) = NT_REPLYMSG;
		am_putmsg(p, m);
	} else
		AB(m, LN_TYPE) = 6;		/* NT_FREEMSG */
}

static void
x_replymsg(r)
	unsigned long *r;
{
	am_replymsg((char *)r[A1]);
}

static void
x_waitport(r)
	unsigned long *r;
{
	char *h = AP((char *)r[A0] + MP_MSGLIST, LH_HEAD);

	if (AP(h, LN_SUCC) == 0) {
		amx_log("amilib: WaitPort on an empty port\n", 0L, 0L, 0L, 0L);
		h = 0;
	}
	r[D0] = (unsigned long)h;
}

static void
x_addport(r)
	unsigned long *r;
{
	char *p = (char *)r[A1];

	AB(p, LN_TYPE) = NT_MSGPORT;
	am_newlist(p + MP_MSGLIST);
	am_enqueue(am_sysbase + EB_PORTLIST, p);
}

static void
x_findport(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_findname(am_sysbase + EB_PORTLIST,
	    (char *)r[A1]);
}

/* -------------------------------------------------------- semaphores */

/* as exec: the node's name and priority stay */
static void
am_initsem(s)
	char *s;
{
	AB(s, LN_TYPE) = NT_SIGNALSEM;
	AW(s, SS_NESTCOUNT) = 0;
	AP(s, SS_WAITQUEUE) = s + SS_WAITQUEUE + 4;
	AL(s, SS_WAITQUEUE + 4) = 0;
	AP(s, SS_WAITQUEUE + 8) = s + SS_WAITQUEUE;
	AL(s, SS_OWNER) = 0;
	AW(s, SS_QUEUECOUNT) = 0xffff;
}

static void
x_initsemaphore(r)
	unsigned long *r;
{
	am_initsem((char *)r[A0]);
}

/* the one task takes every semaphore at once; nesting only counts */
static void
x_obtainsemaphore(r)
	unsigned long *r;
{
	char *s = (char *)r[A0];

	AW(s, SS_NESTCOUNT)++;
	AW(s, SS_QUEUECOUNT)++;
	AP(s, SS_OWNER) = am_task;
	r[D0] = 1;
}

static void
x_releasesemaphore(r)
	unsigned long *r;
{
	char *s = (char *)r[A0];

	if (AW(s, SS_NESTCOUNT) == 0) {
		amx_log("amilib: ReleaseSemaphore %x not held\n", (long)s, 0L,
		    0L, 0L);
		return;
	}
	AW(s, SS_QUEUECOUNT)--;
	if (--AW(s, SS_NESTCOUNT) == 0)
		AP(s, SS_OWNER) = 0;
}

static void
x_addsemaphore(r)
	unsigned long *r;
{
	char *s = (char *)r[A1];

	am_initsem(s);
	am_enqueue(am_sysbase + EB_SEMLIST, s);
}

static void
x_findsemaphore(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_findname(am_sysbase + EB_SEMLIST,
	    (char *)r[A1]);
}

/* -------------------------------------------- libraries and residents */

static void
am_makefunctions(t, fa, disp)
	char *t, *fa, *disp;
{
	int i;
	char *f;

	for (i = 0; ; i++) {
		if (disp) {
			if (AW(fa, 2 * i) == 0xffff)
				break;
			f = disp + (short)AW(fa, 2 * i);
		} else {
			if (AL(fa, 4 * i) == 0xffffffff)
				break;
			f = AP(fa, 4 * i);
		}
		AW(t, -6 * (i + 1)) = 0x4ef9;
		AP(t, -6 * (i + 1) + 2) = f;
	}
}

static int
am_nfuncs(fa)
	char *fa;
{
	int n = 0;

	if (AW(fa, 0) == 0xffff) {
		for (fa += 2; AW(fa, 2 * n) != 0xffff; n++)
			;
	} else
		while (AL(fa, 4 * n) != 0xffffffff)
			n++;
	return n;
}

static void
x_makefunctions(r)
	unsigned long *r;
{
	char *t = (char *)r[A0], *fa = (char *)r[A1], *disp = (char *)r[A2];
	int n = 0;

	am_makefunctions(t, fa, disp);
	if (disp)
		while (AW(fa, 2 * n) != 0xffff)
			n++;
	else
		while (AL(fa, 4 * n) != 0xffffffff)
			n++;
	amx_cacheflush();
	r[D0] = 6 * n;
}

/* InitStruct, as exec: commands ddssnnnn, d = 0 copy, 1 repeat,
 * 2 byte offset, 3 24-bit offset; s = 0 long, 1 word, 2 byte */
static void
am_initstruct(it, mem, size)
	unsigned char *it;
	char *mem;
	unsigned long size;
{
	char *dst = mem;
	int spec, sz, cnt, w;
	unsigned long v;

	if (size)
		am_zero(mem, (long)size);
	while (*it) {
		spec = *it >> 6;
		sz = (*it >> 4) & 3;
		cnt = (*it & 15) + 1;
		it++;
		if (spec == 2)
			dst = mem + *it++;
		else if (spec == 3) {
			dst = mem + ((it[0] << 16) | (it[1] << 8) | it[2]);
			it += 3;
		}
		w = sz == 0 ? 4 : sz == 1 ? 2 : 1;
		if (sz == 3)
			return;
		if (w > 1 && ((unsigned long)it & 1))
			it++;
		if (spec == 1) {
			v = w == 4 ? *(unsigned long *)it :
			    w == 2 ? *(unsigned short *)it : *it;
			it += w;
			while (cnt--) {
				if (w == 4)
					*(unsigned long *)dst = v;
				else if (w == 2)
					*(unsigned short *)dst = v;
				else
					*dst = v;
				dst += w;
			}
		} else
			while (cnt--) {
				am_copy((char *)it, dst, (long)w);
				it += w;
				dst += w;
			}
		if ((unsigned long)it & 1)
			it++;
	}
}

static void
x_initstruct(r)
	unsigned long *r;
{
	am_initstruct((unsigned char *)r[A1], (char *)r[A2], r[D0] & 0xffff);
}

static char *
am_makelibrary(vec, st, init, dsize, seg)
	char *vec, *st, *init;
	unsigned long dsize, seg;
{
	int n = am_nfuncs(vec);
	long neg = ((long)n * 6 + 3) & ~3;
	char *m, *b;
	unsigned long c[16];

	m = am_alloc(neg + dsize, MEMF_PUBLIC | MEMF_CLEAR);
	if (m == 0)
		return 0;
	b = m + neg;
	if (AW(vec, 0) == 0xffff)
		am_makefunctions(b, vec + 2, vec);
	else
		am_makefunctions(b, vec, (char *)0);
	amx_cacheflush();
	AW(b, LIB_NEGSIZE) = neg;
	AW(b, LIB_POSSIZE) = dsize;
	if (st)
		am_initstruct((unsigned char *)st, b, 0L);
	if (init) {
		am_zero((char *)c, (long)sizeof c);
		c[D0] = (unsigned long)b;
		c[A0] = seg;
		c[A6] = (unsigned long)am_sysbase;
		b = (char *)am_call(init, c);
	}
	return b;
}

static void
x_makelibrary(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_makelibrary((char *)r[A0], (char *)r[A1],
	    (char *)r[A2], r[D0], r[D1]);
}

static void
x_addlibrary(r)
	unsigned long *r;
{
	char *b = (char *)r[A1];

	AB(b, LIB_FLAGS) |= 2;			/* LIBF_CHANGED */
	am_enqueue(am_sysbase + EB_LIBLIST, b);
}

static void
x_adddevice(r)
	unsigned long *r;
{
	am_enqueue(am_sysbase + EB_DEVICELIST, (char *)r[A1]);
}

static void
x_addresource(r)
	unsigned long *r;
{
	am_enqueue(am_sysbase + EB_RESOURCELIST, (char *)r[A1]);
}

/* RemLibrary, RemDevice: the library's own Expunge does the removing */
static void
x_remlibrary(r)
	unsigned long *r;
{
	unsigned long c[16];

	am_zero((char *)c, (long)sizeof c);
	r[D0] = am_lvo((char *)r[A1], LVO_EXPUNGE, c);
}

static void
x_openresource(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_findname(am_sysbase + EB_RESOURCELIST,
	    (char *)r[A1]);
}

char *
am_openlib(name, version)
	char *name;
	unsigned long version;
{
	char *b = am_findname(am_sysbase + EB_LIBLIST, name);
	unsigned long c[16];

	if (b == 0 || AW(b, LIB_VERSION) < version)
		return 0;
	am_zero((char *)c, (long)sizeof c);
	c[D0] = version;
	return (char *)am_lvo(b, LVO_OPEN, c);
}

void
am_closelib(b)
	char *b;
{
	unsigned long c[16];

	if (b == 0)
		return;
	am_zero((char *)c, (long)sizeof c);
	(void)am_lvo(b, LVO_CLOSE, c);
}

static void
x_openlibrary(r)
	unsigned long *r;
{
	char *n = (char *)r[A1];

	r[D0] = (unsigned long)am_openlib(n, r[D0]);
	if (r[D0] == 0)
		amx_log("amilib: OpenLibrary %s: not here\n", (long)n, 0L, 0L, 0L);
}

static void
x_oldopenlibrary(r)
	unsigned long *r;
{
	r[D0] = 0;
	x_openlibrary(r);
}

static void
x_closelibrary(r)
	unsigned long *r;
{
	am_closelib((char *)r[A1]);
}

static void
x_setfunction(r)
	unsigned long *r;
{
	char *b = (char *)r[A1], *v = b + (short)r[A0];
	unsigned long old;
	int s = amx_spl7();

	old = AW(v, 0) == 0x4ef9 ? AL(v, 2) : (unsigned long)v;
	AW(v, 0) = 0x4ef9;
	AL(v, 2) = r[D0];
	AB(b, LIB_FLAGS) |= 2;
	amx_cacheflush();
	amx_splx(s);
	r[D0] = old;
}

char *
am_initres(res, seg)
	char *res;
	unsigned long seg;
{
	char *t, *b;
	unsigned long c[16];

	if (AB(res, RT_FLAGS) & RTF_AUTOINIT) {
		t = AP(res, RT_INIT);
		b = am_makelibrary(AP(t, 4), AP(t, 8), AP(t, 12), AL(t, 0), seg);
		if (b == 0)
			return 0;
		switch (AB(res, RT_TYPE)) {
		case NT_LIBRARY:
			AB(b, LIB_FLAGS) |= 2;
			am_enqueue(am_sysbase + EB_LIBLIST, b);
			break;
		case NT_DEVICE:
			am_enqueue(am_sysbase + EB_DEVICELIST, b);
			break;
		case NT_RESOURCE:
			am_enqueue(am_sysbase + EB_RESOURCELIST, b);
			break;
		}
		return b;
	}
	am_zero((char *)c, (long)sizeof c);
	c[A0] = seg;
	c[A6] = (unsigned long)am_sysbase;
	return (char *)am_call(AP(res, RT_INIT), c);
}

static void
x_initresident(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_initres((char *)r[A1], r[D1]);
}

static void
x_findresident(r)
	unsigned long *r;
{
	r[D0] = 0;
}

/* the first Resident in a segment list (BPTR) */
char *
am_findres(seg)
	unsigned long seg;
{
	char *s, *p, *e;

	for (; seg; seg = AL(s, 0)) {
		s = (char *)(seg << 2);
		e = s + AL(s, -4) - 4;
		for (p = s + 4; p + RT_SIZE <= e; p += 2)
			if (AW(p, 0) == RTC_MATCHWORD && AP(p, RT_MATCHTAG) == p)
				return p;
	}
	return 0;
}

/* ---------------------------------------------------------- devices */

static void
x_opendevice(r)
	unsigned long *r;
{
	char *n = (char *)r[A0], *io = (char *)r[A1];
	char *d = am_findname(am_sysbase + EB_DEVICELIST, n);
	unsigned long c[16];

	if (d == 0) {
		amx_log("amilib: OpenDevice %s: not here\n", (long)n, 0L, 0L, 0L);
		AB(io, IO_ERROR) = 0xff;	/* IOERR_OPENFAIL */
		r[D0] = 0xff;
		return;
	}
	am_zero((char *)c, (long)sizeof c);
	c[D0] = r[D0];
	c[D1] = r[D1];
	c[A1] = (unsigned long)io;
	AP(io, IO_DEVICE) = d;
	AB(io, IO_ERROR) = 0;
	(void)am_lvo(d, LVO_OPEN, c);
	r[D0] = (unsigned long)(long)(signed char)AB(io, IO_ERROR);
	if (AB(io, IO_ERROR))
		AL(io, IO_DEVICE) = 0xffffffff;
}

static void
x_closedevice(r)
	unsigned long *r;
{
	char *io = (char *)r[A1];
	unsigned long c[16];

	if (AL(io, IO_DEVICE) == 0 || AL(io, IO_DEVICE) == 0xffffffff)
		return;
	am_zero((char *)c, (long)sizeof c);
	c[A1] = (unsigned long)io;
	(void)am_lvo(AP(io, IO_DEVICE), LVO_CLOSE, c);
	AL(io, IO_DEVICE) = 0xffffffff;
}

static void
am_beginio(io)
	char *io;
{
	unsigned long c[16];

	am_zero((char *)c, (long)sizeof c);
	c[A1] = (unsigned long)io;
	AB(io, LN_TYPE) = NT_MESSAGE;
	(void)am_lvo(AP(io, IO_DEVICE), -30, c);	/* BeginIO */
}

static void
x_doio(r)
	unsigned long *r;
{
	char *io = (char *)r[A1];

	AB(io, IO_FLAGS) |= IOF_QUICK;
	am_beginio(io);
	r[D0] = (unsigned long)(long)(signed char)AB(io, IO_ERROR);
}

static void
x_sendio(r)
	unsigned long *r;
{
	char *io = (char *)r[A1];

	AB(io, IO_FLAGS) &= ~IOF_QUICK;
	am_beginio(io);
}

static void
x_checkio(r)
	unsigned long *r;
{
	char *io = (char *)r[A1];

	r[D0] = (AB(io, IO_FLAGS) & IOF_QUICK) ||
	    AB(io, LN_TYPE) != NT_MESSAGE ? (unsigned long)io : 0;
}

static void
x_waitio(r)
	unsigned long *r;
{
	char *io = (char *)r[A1];

	if (!(AB(io, IO_FLAGS) & IOF_QUICK) && AB(io, LN_TYPE) == NT_REPLYMSG)
		am_remove(io);
	r[D0] = (unsigned long)(long)(signed char)AB(io, IO_ERROR);
}

static void
x_abortio(r)
	unsigned long *r;
{
	r[D0] = 0;
}

/* a device's BeginIO finishes here: quick requests return, others are
 * replied as exec would */
void
am_iodone(io)
	char *io;
{
	if (AB(io, IO_FLAGS) & IOF_QUICK)
		AB(io, LN_TYPE) = NT_REPLYMSG;
	else
		am_replymsg(io);
}

static void
x_createiorequest(r)
	unsigned long *r;
{
	char *io = 0;

	if (r[A0] && r[D0] >= IO_SIZE &&
	    (io = am_alloc(r[D0], MEMF_PUBLIC | MEMF_CLEAR)) != 0) {
		AB(io, LN_TYPE) = NT_REPLYMSG;
		AP(io, MN_REPLYPORT) = (char *)r[A0];
		AW(io, MN_LENGTH) = r[D0];
	}
	r[D0] = (unsigned long)io;
}

static void
x_deleteiorequest(r)
	unsigned long *r;
{
	char *io = (char *)r[A0];

	if (io)
		am_free(io, (unsigned long)AW(io, MN_LENGTH));
}

/* ----------------------------------------------------------- caches */

static void
x_cacheclear(r)
	unsigned long *r;
{
	amx_cacheflush();
}

static void
x_cachecontrol(r)
	unsigned long *r;
{
	if (r[D1])
		amx_log("amilib: CacheControl %x/%x ignored\n", (long)r[D0],
		    (long)r[D1], 0L, 0L);
	r[D0] = 0x0101;			/* CACRF_EnableI | CACRF_EnableD */
}

/* CachePreDMA: the physical address of a kernel buffer, one page at most */
static void
x_cachepredma(r)
	unsigned long *r;
{
	char *a = (char *)r[A0];
	unsigned long *len = (unsigned long *)r[A1];
	unsigned long room = 4096 - ((unsigned long)a & 4095);

	amx_cacheflush();
	if (len && *len > room)
		*len = room;
	r[D0] = amx_vtop(a);
}

/* --------------------------------------------------------- RawDoFmt */

static void
am_putch(r, ch)
	unsigned long *r;
	int ch;
{
	unsigned long c[16];
	char *p;

	if (r[A2] == 0) {			/* V45: store at a3 */
		p = (char *)r[A3];
		*p = ch;
		r[A3]++;
		return;
	}
	am_copy((char *)r, (char *)c, (long)(15 * 4));
	c[D0] = ch & 0xff;
	c[A6] = (unsigned long)am_sysbase;
	(void)am_call((char *)r[A2], c);
	r[A3] = c[A3];
}

static void
x_rawdofmt(r)
	unsigned long *r;
{
	unsigned char *f = (unsigned char *)r[A0];
	char *data = (char *)r[A1], *s, buf[12];
	int left, zero, width, limit, islong, n, i, neg;
	unsigned long v, base;

	for (; *f; f++) {
		if (*f != '%') {
			am_putch(r, *f);
			continue;
		}
		f++;
		left = zero = 0;
		width = 0;
		limit = -1;
		if (*f == '-')
			left = 1, f++;
		if (*f == '0')
			zero = 1, f++;
		while (*f >= '0' && *f <= '9')
			width = width * 10 + *f++ - '0';
		if (*f == '.')
			for (limit = 0, f++; *f >= '0' && *f <= '9'; f++)
				limit = limit * 10 + *f - '0';
		islong = 0;
		if (*f == 'l')
			islong = 1, f++;
		if (*f == 0)
			break;
		s = buf;
		n = 0;
		switch (*f) {
		case 'd': case 'D': case 'u': case 'U': case 'x': case 'X':
			if (islong) {
				v = AL(data, 0);
				data += 4;
			} else {
				v = AW(data, 0);
				if (*f == 'd' || *f == 'D')
					v = (unsigned long)(long)(short)v;
				data += 2;
			}
			neg = (*f == 'd' || *f == 'D') && (long)v < 0;
			if (neg)
				v = -v;
			base = (*f == 'x' || *f == 'X') ? 16 : 10;
			i = sizeof buf;
			do {
				buf[--i] = (*f == 'X' ? "0123456789ABCDEF" :
				    "0123456789abcdef")[v % base];
				v /= base;
			} while (v);
			if (neg)
				buf[--i] = '-';
			s = buf + i;
			n = sizeof buf - i;
			break;
		case 's':
			s = AP(data, 0);
			data += 4;
			for (n = 0; s && s[n]; n++)
				;
			break;
		case 'b':			/* BSTR */
			s = (char *)(AL(data, 0) << 2);
			data += 4;
			n = s ? *(unsigned char *)s++ : 0;
			break;
		case 'c':
			buf[0] = islong ? AL(data, 0) : AW(data, 0);
			data += islong ? 4 : 2;
			n = 1;
			break;
		default:
			buf[0] = *f;
			n = 1;
			break;
		}
		if (limit >= 0 && n > limit)
			n = limit;
		if (!left)
			for (i = n; i < width; i++)
				am_putch(r, zero ? '0' : ' ');
		for (i = 0; i < n; i++)
			am_putch(r, s[i]);
		if (left)
			for (i = n; i < width; i++)
				am_putch(r, ' ');
	}
	am_putch(r, 0);
	r[D0] = (unsigned long)data;
}

/* ------------------------------------------------------------ misc */

static void
x_alert(r)
	unsigned long *r;
{
	amx_log("amilib: Alert %x\n", (long)r[D7], 0L, 0L, 0L);
}

static void
x_getcc(r)
	unsigned long *r;
{
	r[D0] = 0;
}

static void
x_ok(r)
	unsigned long *r;
{
	r[D0] = 1;
}

static void
x_zero(r)
	unsigned long *r;
{
	r[D0] = 0;
}

static struct amfn am_exectab[] = {
	{ -30, x_supervisor },
	{ -72, x_zero },		/* InitCode */
	{ -78, x_initstruct },
	{ -84, x_makelibrary },
	{ -90, x_makefunctions },
	{ -96, x_findresident },
	{ -102, x_initresident },
	{ -108, x_alert },
	{ -114, x_zero },		/* Debug */
	{ -120, x_disable },
	{ -126, x_enable },
	{ -132, x_forbid },
	{ -138, x_permit },
	{ -144, x_setsr },
	{ -150, x_superstate },
	{ -156, x_zero },		/* UserState */
	{ -168, x_addintserver },
	{ -174, x_remintserver },
	{ -180, x_cause },
	{ -198, x_allocmem },
	{ -204, x_allocabs },
	{ -210, x_freemem },
	{ -216, x_availmem },
	{ -234, x_insert },
	{ -240, x_addhead },
	{ -246, x_addtail },
	{ -252, x_remove },
	{ -258, x_remhead },
	{ -264, x_remtail },
	{ -270, x_enqueue },
	{ -276, x_findname },
	{ -294, x_findtask },
	{ -306, x_setsignal },
	{ -318, x_wait },
	{ -324, x_signal },
	{ -330, x_allocsignal },
	{ -336, x_freesignal },
	{ -354, x_addport },
	{ -360, x_remove },		/* RemPort */
	{ -366, x_putmsg },
	{ -372, x_getmsg },
	{ -378, x_replymsg },
	{ -384, x_waitport },
	{ -390, x_findport },
	{ -396, x_addlibrary },
	{ -402, x_remlibrary },
	{ -408, x_oldopenlibrary },
	{ -414, x_closelibrary },
	{ -420, x_setfunction },
	{ -426, x_zero },		/* SumLibrary */
	{ -432, x_adddevice },
	{ -438, x_remlibrary },		/* RemDevice */
	{ -444, x_opendevice },
	{ -450, x_closedevice },
	{ -456, x_doio },
	{ -462, x_sendio },
	{ -468, x_checkio },
	{ -474, x_waitio },
	{ -480, x_abortio },
	{ -486, x_addresource },
	{ -492, x_remove },		/* RemResource */
	{ -498, x_openresource },
	{ -522, x_rawdofmt },
	{ -528, x_getcc },
	{ -534, x_typeofmem },
	{ -552, x_openlibrary },
	{ -558, x_initsemaphore },
	{ -564, x_obtainsemaphore },
	{ -570, x_releasesemaphore },
	{ -576, x_obtainsemaphore },	/* AttemptSemaphore */
	{ -594, x_findsemaphore },
	{ -600, x_addsemaphore },
	{ -606, x_remove },		/* RemSemaphore (a1) */
	{ -624, x_copymem },
	{ -630, x_copymem },		/* CopyMemQuick */
	{ -636, x_cacheclear },		/* CacheClearU */
	{ -642, x_cacheclear },		/* CacheClearE */
	{ -648, x_cachecontrol },
	{ -654, x_createiorequest },
	{ -660, x_deleteiorequest },
	{ -666, x_createmsgport },
	{ -672, x_deletemsgport },
	{ -678, x_obtainsemaphore },	/* ObtainSemaphoreShared */
	{ -684, x_allocvec },
	{ -690, x_freevec },
	{ -696, x_createpool },
	{ -702, x_deletepool },
	{ -708, x_allocpooled },
	{ -714, x_freepooled },
	{ -720, x_obtainsemaphore },	/* AttemptSemaphoreShared */
	{ -762, x_cachepredma },
	{ -768, x_cacheclear },		/* CachePostDMA */
	{ -774, x_ok },			/* AddMemHandler */
	{ -780, x_zero },		/* RemMemHandler */
	{ 0, 0 }
};

static struct amlib am_exec = {
	"exec.library", NT_LIBRARY, 40, EXEC_NLVO, EB_SIZE, am_exectab
};

/* ------------------------------------------------------ setup, teardown */

int
am_init(attn)
	int attn;
{
	char *b;
	int i;

	if (am_users++)
		return 0;
	am_attn = attn;
	am_nlibs = 0;
	am_dis = am_forb = 0;
	am_intmask = 0;
	for (i = 0; i < EXEC_NLVO; i++)
		am_said[i] = 0;
	if ((b = am_addlib(&am_exec)) == 0) {
		am_users = 0;
		return -1;
	}
	am_sysbase = b;
	AW(b, LIB_REVISION) = 0;
	AW(b, 34) = 40;				/* SoftVer */
	AB(b, EB_IDNESTCNT) = 0xff;
	AB(b, EB_TDNESTCNT) = 0xff;
	AW(b, EB_ATTNFLAGS) = attn;
	AB(b, EB_VBLANKFREQ) = 50;
	AB(b, EB_POWERFREQ) = 50;
	AL(b, EB_ECLOCKFREQ) = 709379;
	am_newlist(b + EB_MEMLIST);
	am_newlist(b + EB_RESOURCELIST);
	am_newlist(b + EB_DEVICELIST);
	am_newlist(b + EB_INTRLIST);
	am_newlist(b + EB_LIBLIST);
	am_newlist(b + EB_PORTLIST);
	am_newlist(b + EB_TASKREADY);
	am_newlist(b + EB_TASKWAIT);
	am_newlist(b + EB_SEMLIST);
	AP(b, EB_MEMHANDLERS) = b + EB_MEMHANDLERS + 4;	/* MinList */
	AP(b, EB_MEMHANDLERS + 8) = b + EB_MEMHANDLERS;
	am_addtail(b + EB_LIBLIST, b);

	am_stkbot = am_alloc((unsigned long)AM_STACK, MEMF_PUBLIC);
	am_task = am_alloc((unsigned long)PR_SIZE + 8, MEMF_PUBLIC | MEMF_CLEAR);
	am_ints = am_alloc((unsigned long)AM_NINT * LH_SIZE, MEMF_PUBLIC);
	if (am_task == 0 || am_ints == 0 || am_stkbot == 0) {
		am_users = 1;
		am_fini();
		return -1;
	}
	/* a Process: dos and its callers use pr_Result2, pr_CurrentDir and
	 * pr_WindowPtr; the rest stays zero */
	AB(am_task, LN_TYPE) = NT_PROCESS;
	AP(am_task, LN_NAME) = am_task + PR_SIZE;
	am_copy("amilib", am_task + PR_SIZE, 7L);
	am_newlist(am_task + 92 + MP_MSGLIST);		/* pr_MsgPort */
	AB(am_task, 92 + LN_TYPE) = NT_MSGPORT;
	AL(am_task, TC_SIGALLOC) = ~AM_SIGFREE;
	AP(b, EB_THISTASK) = am_task;
	am_stktop = am_stkbot + AM_STACK;	/* from here on, calls use it */
	AP(am_task, 58) = am_stkbot;		/* tc_SPLower */
	AP(am_task, 62) = am_stktop;		/* tc_SPUpper */
	for (i = 0; i < AM_NINT; i++)
		am_newlist(am_ints + i * LH_SIZE);

	if (am_addlib(&am_utility) == 0 || am_addlib(&am_timer) == 0 ||
	    am_addlib(&am_intuition) == 0 || am_addlib(&am_dos) == 0 ||
	    am_mmuinit() != 0 || am_expinit() != 0) {
		am_users = 1;
		am_fini();
		return -1;
	}
	return 0;
}

/* everything am_init made, when its last user is done; each user has
 * stopped its libraries first */
void
am_fini()
{
	int i;

	if (am_users == 0 || --am_users)
		return;
	for (i = 0; i < AM_NINT; i++)
		if (am_intmask & (1 << i))
			amx_intdetach(i);
	am_intmask = 0;
	am_expfini();
	am_mmufini();
	am_dellib(&am_dos);
	am_dellib(&am_intuition);
	am_dellib(&am_timer);
	am_dellib(&am_utility);
	if (am_ints)
		am_free(am_ints, (unsigned long)AM_NINT * LH_SIZE);
	if (am_task)
		am_free(am_task, (unsigned long)PR_SIZE + 8);
	am_ints = am_task = 0;
	if (am_stkbot)
		am_free(am_stkbot, (unsigned long)AM_STACK);
	am_stkbot = am_stktop = 0;
	am_dellib(&am_exec);
	am_sysbase = 0;
	if (am_dis)
		amx_splx(am_dissr);
	am_dis = 0;
}

long
am_meminuse()
{
	return am_inuse;
}
