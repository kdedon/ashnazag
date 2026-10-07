/*
 * hmain.c -- the harness program run.py runs on the emulated 68040.
 *
 * h_mode picks the test; results go to h_res[] (run.py checks them)
 * and lines to the console.  The library image, when there is one,
 * is placed by run.py at h_lib / h_liblen.
 */

#include "amilib.h"
#include "sys/opci.h"

extern int opci_init(), opci_fini();
extern long h_delay_us;
extern int h_attached;

int h_mode;
char *h_lib;
unsigned long h_liblen;
char *h_hunk[8];		/* mode 2: load files to try */
unsigned long h_hunklen[8];
long h_res[64];
int h_nres;

#define	RES(v)	(h_res[h_nres++] = (long)(v))

/* ------------------------------------------------- mode 0, 1: opci */

static int intcount;

static int
myisr(arg)
	char *arg;
{
	intcount++;
	return 0;
}

static void
t_opci()
{
	struct opci_dev *d, *n;
	struct opci_intr oi;
	int e, i;

	e = opci_init(h_lib, h_liblen);
	RES(e);					/* 0 */
	if (e) {
		RES(am_meminuse());		/* 1 */
		return;
	}
	RES(opci_bus());			/* 1 */
	d = opci_find((struct opci_dev *)0, OPCI_ANY, OPCI_ANY);
	RES(d != 0);				/* 2 */
	if (d) {
		RES(opci_cfgread(d, OPCI_CFG_VENDOR, 2));	/* 3 */
		RES(opci_cfgread(d, 2, 2));			/* 4 */
		RES(opci_cfgread(d, 0, 4));			/* 5 */
		RES(opci_attr(d, OPCI_VENDOR));			/* 6 */
		RES(opci_attr(d, OPCI_DEVICE));			/* 7 */
		RES(opci_attr(d, OPCI_BARADDR(0)));		/* 8 */
		RES(opci_attr(d, OPCI_BARSIZE(0)));		/* 9 */
		RES(opci_cfgread(d, OPCI_CFG_BAR0, 4));		/* 10 */
		RES(opci_cfgread(d, OPCI_CFG_COMMAND, 2));	/* 11 */
		RES(opci_obtain(d));				/* 12 */
		RES(opci_obtain(d));				/* 13: busy */
		opci_release(d);
		RES(opci_obtain(d));				/* 14 */
		opci_release(d);
		n = opci_find((struct opci_dev *)0, 0x10ec, 0x8139);
		RES(n == d);					/* 15 */
		n = opci_find((struct opci_dev *)0, 0x1234, OPCI_ANY);
		RES(n == 0);					/* 16 */
		n = opci_find(d, OPCI_ANY, OPCI_ANY);
		RES(n != 0);					/* 17: second card */
		RES(n ? opci_attr(n, OPCI_VENDOR) : 0);		/* 18 */
		opci_cfgwrite(d, OPCI_CFG_INTLINE, 1, 0x5a);
		RES(opci_cfgread(d, OPCI_CFG_INTLINE, 1));	/* 19 */
		oi.oi_fn = myisr;
		oi.oi_arg = 0;
		e = opci_intr(d, &oi);
		RES(e);						/* 20 */
		RES(h_attached);				/* 21 */
		RES(am_intmask);				/* 22 */
		for (i = 0; i < AM_NINT; i++)
			if (am_intmask & (1 << i))
				(void)am_intrun(i);
		RES(intcount);					/* 23 */
		if (e == 0)
			opci_unintr(&oi);
		for (i = 0; i < AM_NINT; i++)
			if (am_intmask & (1 << i))
				(void)am_intrun(i);
		RES(intcount);					/* 24: not again */
	} else
		h_nres = 25;
	RES(opci_fini());				/* 25 */
	RES(am_meminuse());				/* 26 */
	RES(h_delay_us);				/* 27 */
}

/* ------------------------------------------------ mode 2: LoadSeg */

static void
t_hunks()
{
	unsigned long seg;
	int i, e;
	char *s;

	for (i = 0; i < 8 && h_hunk[i]; i++) {
		seg = am_loadseg(h_hunk[i], h_hunklen[i], &e);
		RES(e);
		RES(seg << 2);
		if (seg) {
			s = (char *)(seg << 2);
			RES(AL(s, 4));		/* first long of hunk 0 */
			RES(AL(s, 8));
			RES(AL(s, 0) << 2);	/* hunk 1 */
			am_unloadseg(seg);
		} else {
			RES(0);
			RES(0);
			RES(0);
		}
	}
	RES(am_meminuse());
}

/* ------------------------------------- mode 3: exec and utility calls */

static char fmtbuf[64];

static void
t_exec()
{
	unsigned long r[16], tags[8], args[4];
	char *u, *l, *n1, *n2;
	int i;

	RES(am_init(AFF_68010 | AFF_68020));		/* 0 */
	for (i = 0; i < 16; i++)
		r[i] = 0;
	/* RawDoFmt into a buffer (PutChProc 0) */
	args[0] = (unsigned long)"lib";
	args[1] = 42L << 16 | 0xbeef;		/* two WORD arguments */
	args[2] = 0xfffffffe;
	args[3] = (unsigned long)"ab";
	r[A0] = (unsigned long)"%s %d %x %ld|%5s|";
	r[A1] = (unsigned long)args;
	r[A2] = 0;
	r[A3] = (unsigned long)fmtbuf;
	(void)am_lvo(am_sysbase, -522, r);
	RES(fmtbuf);					/* 1 */
	/* utility */
	u = am_openlib("utility.library", 37L);
	RES(u != 0);					/* 2 */
	tags[0] = 0x80000001;
	tags[1] = 11;
	tags[2] = TAG_IGNORE;
	tags[3] = 0;
	tags[4] = 0x80000002;
	tags[5] = 22;
	tags[6] = TAG_DONE;
	for (i = 0; i < 16; i++)
		r[i] = 0;
	r[D0] = 0x80000002;
	r[D1] = 99;
	r[A0] = (unsigned long)tags;
	RES(am_lvo(u, -36, r));				/* 3: GetTagData */
	r[D0] = 0x80000003;
	r[D1] = 99;
	r[A0] = (unsigned long)tags;
	RES(am_lvo(u, -36, r));				/* 4 */
	r[A0] = (unsigned long)"PCI";
	r[A1] = (unsigned long)"pci";
	RES(am_lvo(u, -162, r));			/* 5: Stricmp */
	r[D0] = 0x12345678;
	r[D1] = 0x9abcdef0;
	(void)am_lvo(u, -204, r);			/* UMult64 */
	RES(r[D0]);					/* 6 */
	RES(r[D1]);					/* 7 */
	am_closelib(u);
	/* lists: Enqueue by priority, FindName */
	l = am_alloc(14L, MEMF_CLEAR);
	n1 = am_alloc(14L, MEMF_CLEAR);
	n2 = am_alloc(14L, MEMF_CLEAR);
	am_newlist(l);
	AB(n1, LN_PRI) = 0;
	AP(n1, LN_NAME) = "low";
	AB(n2, LN_PRI) = 5;
	AP(n2, LN_NAME) = "high";
	am_enqueue(l, n1);
	am_enqueue(l, n2);
	RES(AP(l, LH_HEAD) == n2);			/* 8 */
	RES(am_findname(l, "low") == n1);		/* 9 */
	am_free(l, 14L);
	am_free(n1, 14L);
	am_free(n2, 14L);
	/* missing library and device */
	RES(am_openlib("dos.library", 0L) == 0);	/* 10 */
	am_fini();
	RES(am_meminuse());				/* 11 */
}

int
h_main()
{
	switch (h_mode) {
	case 0:
	case 1:
		t_opci();
		break;
	case 2:
		t_hunks();
		break;
	case 3:
		t_exec();
		break;
	}
	return h_nres;
}
