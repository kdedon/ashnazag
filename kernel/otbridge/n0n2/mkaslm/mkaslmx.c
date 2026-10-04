/*
 * mkaslmx.c -- mkaslm -x: load each library as the Shared Library Manager
 * does (A5 world, jump table of JMPs, both relocation lists), run its
 * entry with a stand-in context and call what it exports.  Included by
 * mkaslm.c on 68k hosts; build with -fcall-used-d2, since library code
 * may use d2 as scratch.  Pointer results are read from d0 (as long).
 */

#include <signal.h>
#include <setjmp.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>

static jmp_buf xjb;
static int xn, xcbi, xbad;
static char *xstage;
static long xcb[8], xrec[8], xdesc[8], xarg[8];
static long xv[32];			/* the context object's virtual table */
static long *xobj = xv;
static long xctx[8];

static void
xfault(sig)
	int sig;
{
	longjmp(xjb, sig);
}

#define	XS(n)	static long xs##n() { xbad = 4 * n; return 0; }
XS(0) XS(1) XS(2) XS(3) XS(4) XS(5) XS(6) XS(7) XS(8) XS(9) XS(10) XS(11)
XS(12) XS(13) XS(14) XS(15) XS(16) XS(17) XS(18) XS(21) XS(23) XS(24)
XS(25) XS(26) XS(27) XS(28) XS(29) XS(30) XS(31)

static long
x76(obj, n)
	long obj, n;
{
	xn = n;
	return 1000;
}

static long
x80(obj, cur, cb, rec)
	long obj, cur, cb, rec;
{
	int i = cur - 1000;

	if (i >= 0 && i < 8) {
		xcb[i] = cb;
		xrec[i] = rec;
	}
	return cur + 1;
}

static long
x88(obj, a, desc, z)
	long obj, a, desc, z;
{
	if (xcbi >= 0 && xcbi < 8) {
		xdesc[xcbi] = desc;
		xarg[xcbi] = a;
	}
	return 0;
}

static void
xvinit()
{
	static long (*f[32])() = {
		xs0, xs1, xs2, xs3, xs4, xs5, xs6, xs7, xs8, xs9, xs10, xs11,
		xs12, xs13, xs14, xs15, xs16, xs17, xs18, x76, x80, xs21, x88,
		xs23, xs24, xs25, xs26, xs27, xs28, xs29, xs30, xs31 };
	int i;

	for (i = 0; i < 32; i++)
		xv[i] = (long)f[i];
	memset(xctx, 0, sizeof xctx);
	xctx[5] = (long)&xobj;		/* context word 20: the object */
}

static int xfail;

static void
xcheck(ok, what, v)
	int ok;
	char *what;
	long v;
{
	if (!ok) {
		printf("  FAIL %s (%#lx)\n", what, v);
		xfail++;
	}
}

/* call the test library's functions by name */
static void
xcalltest(names, fns, n)
	long names, fns;
	int n;
{
	int i;
	long (*f)(), r;
	char *nm;

	for (i = 0; i < n; i++) {
		nm = *(char **)(names + 4 * i);
		f = *(long (**)())(fns + 4 * i);
		if (strcmp(nm, "AUXTestAdd") == 0) {
			r = (*f)(3L, 4L);
			xcheck(r == 7, "AUXTestAdd(3, 4) == 7", r);
		} else if (strcmp(nm, "AUXTestMsg") == 0) {
			r = (*f)(0L);
			xcheck(strcmp((char *)r, "hello from Main") == 0, "AUXTestMsg(0)", r);
			r = (*f)(1L);
			xcheck(strcmp((char *)r, "second") == 0, "AUXTestMsg(1)", r);
		} else if (strcmp(nm, "AUXTestCount") == 0) {
			r = (*f)();
			xcheck(r == 6, "AUXTestCount() == 6", r);
			r = (*f)();
			xcheck(r == 12, "AUXTestCount() == 12", r);
		}
	}
}

static void
runone(lb)
	struct lib *lb;
{
	char *w, *sp[16];
	long a5, r, off, i, n, nf, *d;
	int k, sg, sig;
	unsigned char *e;
	struct seg *s;
	long (*entry)();

	printf("%s:\n", lb->l.name);
	if (lb->jt.below > 0x1000000UL || lb->jt.above > 0x100000UL) {
		printf("  FAIL A5 world too large\n");
		xfail++;
		return;
	}
	w = xalloc((long)(lb->jt.below + lb->jt.above + 8));
	a5 = ((long)w + lb->jt.below + 3) & ~3L;
	for (k = 1; k < lb->nseg; k++) {
		sp[k] = xalloc(lb->seg[k]->len);
		memcpy(sp[k], lb->seg[k]->data, (size_t)lb->seg[k]->len);
	}
	memcpy((char *)a5 + lb->jt.off, lb->jt.e, (size_t)lb->jt.len);
	for (k = 0; k < lb->jt.n; k++) {
		if ((sg = jtfar(&lb->jt, k, &off)) < 1 || sg >= lb->nseg)
			continue;
		e = (unsigned char *)a5 + lb->jt.off + 8 * k;
		wr16(e + 2, 0x4ef9);
		wr32(e + 4, (unsigned long)(sp[sg] + off));
	}
	for (k = 1; k < lb->nseg; k++) {
		s = &lb->s[k];
		for (i = 0; i < s->a5.n; i++) {
			e = (unsigned char *)sp[k] + s->a5.off[i];
			wr32(e, rd32(e) + a5);
		}
		for (i = 0; i < s->sr.n; i++) {
			e = (unsigned char *)sp[k] + s->sr.off[i];
			wr32(e, rd32(e) + (long)sp[k] + FARHDR);
		}
	}
	entry = (long (*)())(a5 + lb->jt.off + 16 + 2);
	xvinit();
	xn = xbad = 0;
	xcbi = -1;
	signal(SIGSEGV, xfault);
	signal(SIGBUS, xfault);
	signal(SIGILL, xfault);
	signal(SIGEMT, xfault);
	signal(SIGSYS, xfault);
	if ((sig = setjmp(xjb)) != 0) {
		printf("  FAIL signal %d inside the library, %s\n", sig, xstage);
		xfail++;
		return;
	}
	xstage = "entry 0";
	r = (*entry)(0L, a5, 0L);
	xcheck(r == 0, "entry 0 (initialise) returns 0", r);
	xstage = "entry 1";
	r = (*entry)(1L, (long)xctx, 0L);
	xcheck(r == 0, "entry 1 (context) returns 0", r);
	xctx[3] = 0x55;
	xstage = "entry 2";
	r = (*entry)(2L, 0L, 0L);
	xcheck(r == a5 + lb->jt.off + 8 * 3 + 2, "entry 2 returns Main's first entry", r);
	xcheck(xctx[3] == 0, "entry 2 clears context word 12", xctx[3]);
	xstage = "entry 3";
	r = (*entry)(3L, 0L, 0L);
	xcheck(r == 1000, "entry 3 returns the first cursor", r);
	xcheck(xn == lb->l.nc, "entry 3 announces every set", (long)xn);
	for (k = 0; k < xn && k < 8; k++) {
		xcbi = k;
		xstage = "an export callback";
		r = (*(long (*)())xcb[k])(0x1234L, 0L);
		xcheck(xarg[k] == 0x1234 && xdesc[k] != 0, "callback passes a descriptor", xarg[k]);
		xcheck(strcmp((char *)xrec[k] + 16, lb->l.c[k].name) == 0,
		    "record names the libr class", xrec[k]);
		d = (long *)xdesc[k];
		if (d == 0)
			continue;
		for (nf = 0; d[1 + nf]; nf++)
			;
		printf("  %s %04x: %ld functions", (char *)xrec[k] + 16,
		    rd16((unsigned char *)xrec[k] + 12), nf);
		if (d[0]) {
			for (n = 0; ((long *)d[0])[n]; n++)
				printf(" %s", ((char **)d[0])[n]);
			xcheck(n == nf, "a name per function", n);
			xstage = "an exported function";
			xcalltest(d[0], (long)(d + 1), (int)nf);
		}
		printf("\n");
	}
	xcheck(xbad == 0, "only the known virtual functions are called", (long)xbad);
}

static int
runlibs(fk, only)
	struct fork *fk;
	char *only;
{
	int i, z;
	struct lib lb;
	char *why;

	if (loadcheck(fk) != 0)
		die("the layout check failed; not running", "");
	/* low memory reads (ExpandMem) as zeros, where the system allows it */
	if ((z = open("/dev/zero", O_RDWR)) >= 0) {
		if (mmap((caddr_t)0, 4096, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_FIXED, z, 0L) == (caddr_t)-1)
			printf("(page 0 not mapped)\n");
		close(z);
	}
	for (i = 0; i < fk->n; i++) {
		if (memcmp(fk->r[i].type, "libr", 4) != 0)
			continue;
		if (getlib(fk, &fk->r[i], &lb, &why) < 0) {
			printf("FAIL libr %d: %s\n", fk->r[i].id, why);
			xfail++;
			continue;
		}
		if (only == 0 || strcmp(only, lb.l.name) == 0)
			runone(&lb);
	}
	printf("%s: -x\n", xfail ? "FAIL" : "PASS");
	return xfail != 0;
}
