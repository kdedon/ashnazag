/*
 * dlm_slot.c -- exec-format and hook linkages.
 *
 * execsw is replaced by a larger table; dlm_slot_init copies the stock
 * rows in from __amix_execsw.  A registered magic gets a row whose
 * exec_func is the trampoline of its slot, bound to the slot rather
 * than the row, so rows can move.  The trampoline loads the module on
 * first use and holds it across the call.  An EXF_FIRST slot sits
 * before the rows it shadows and falls through to the next row with
 * the same magic when its module declines (ENOEXEC) or cannot be
 * loaded:
 *
 *	execsw:	[0x150 slot 0, EXF_FIRST] -> auxexec, else
 *		[0x150 coffexec] [elf] [#!] ...
 *
 * Hooks: each entry of a module's hook data sets one hooksw pointer,
 * which the static shims call through.  Removal restores the default.
 *
 * K&R C.
 */

#include "dlm.h"
#ifndef DLM_HOST
#include "sys/exec.h"
#endif

#define	EXEC_STATIC	3
#define	EXEC_RESERVE	8
#define	NEXECSW		(EXEC_STATIC + EXEC_RESERVE)
#define	EXDSZ		12		/* struct mod_exec_data */
#define	HKDSZ		8		/* struct mod_hook_data */

struct execsw	execsw[NEXECSW] = { { 0 } };	/* in .data, as the stock table */
extern struct execsw __amix_execsw[];
extern struct hooksw hooksw[];

struct dlm_xslot {
	char		xs_name[MODMAXNAMELEN];
	short		xs_magic;	/* the row's exec_magic points here */
	short		xs_flags;
	int		(*xs_tramp)();
	struct dlm_mod	*xs_mod;	/* installed module */
	unsigned long	xs_func;	/* its exec function */
};

static int xcall();

#define	XBODY(n) \
	long a[8]; \
	a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; \
	a[4] = a4; a[5] = a5; a[6] = a6; a[7] = a7; \
	return xcall(&dlm_xslot[n], a)
#define	XARGS	a0, a1, a2, a3, a4, a5, a6, a7
#define	XDECL	long a0, a1, a2, a3, a4, a5, a6, a7;

static int x0(), x1(), x2(), x3(), x4(), x5(), x6(), x7();

struct dlm_xslot dlm_xslot[EXEC_RESERVE] = {
	{ "", 0, 0, x0 }, { "", 0, 0, x1 }, { "", 0, 0, x2 }, { "", 0, 0, x3 },
	{ "", 0, 0, x4 }, { "", 0, 0, x5 }, { "", 0, 0, x6 }, { "", 0, 0, x7 },
};

static int x0(XARGS) XDECL { XBODY(0); }
static int x1(XARGS) XDECL { XBODY(1); }
static int x2(XARGS) XDECL { XBODY(2); }
static int x3(XARGS) XDECL { XBODY(3); }
static int x4(XARGS) XDECL { XBODY(4); }
static int x5(XARGS) XDECL { XBODY(5); }
static int x6(XARGS) XDECL { XBODY(6); }
static int x7(XARGS) XDECL { XBODY(7); }

/* at boot, before the first exec, whether or not the DLM comes up */
void
dlm_slot_init()
{
	int i;

	if (nexectype > NEXECSW)
		nexectype = NEXECSW;
	for (i = 0; i < nexectype; i++)
		execsw[i] = __amix_execsw[i];
}

static int
inimg(m, a, n)
	struct dlm_mod *m;
	unsigned long a, n;
{
	return a >= m->m_run && n <= m->m_size && a - m->m_run <= m->m_size - n;
}

static int
ismagic(i, mg)
	int i, mg;
{
	return execsw[i].exec_magic && (*execsw[i].exec_magic & 0xffff) == mg;
}

static int
rowof(f)
	int (*f)();
{
	int i;

	for (i = 0; i < nexectype; i++)
		if (execsw[i].exec_func == f)
			return i;
	return -1;
}

/* the first row after the slot's with the same magic */
static int
xnext(xs, a)
	struct dlm_xslot *xs;
	long *a;
{
	int i, mg = xs->xs_magic & 0xffff;

	for (i = rowof(xs->xs_tramp) + 1; i > 0 && i < nexectype; i++)
		if (ismagic(i, mg))
			return (*execsw[i].exec_func)(a[0], a[1], a[2], a[3],
			    a[4], a[5], a[6], a[7]);
	return ENOEXEC;
}

static int
xcall(xs, a)
	struct dlm_xslot *xs;
	long *a;
{
	struct dlm_guard g;
	struct { struct dlm_mod *m; } h;	/* in memory: read after a longjmp */
	label_t save;
	struct dlm_mod *m;
	int e;

	bzero((caddr_t)&g, sizeof g);
	bzero((caddr_t)&h, sizeof h);
	bcopy((caddr_t)&u.u_qsav, (caddr_t)&save, sizeof (label_t));
	if (DLM_SETJMP(&u.u_qsav)) {
		bcopy((caddr_t)&save, (caddr_t)&u.u_qsav, sizeof (label_t));
		dlm_abort(&g);
		if (h.m)
			dlm_rele(h.m);
		DLM_LONGJMP(&u.u_qsav);
	}
	e = 0;
	if (xs->xs_mod == 0 &&
	    ((e = dlm_load(xs->xs_name, DL_SYS, 1, &g, &m)) != 0 || xs->xs_mod == 0))
		e = ENOEXEC;
	if (e == 0) {
		h.m = m = xs->xs_mod;
		dlm_hold(m);
		e = DLM_CALL8(m, xs->xs_func, a);
		h.m = 0;
		dlm_rele(m);
	}
	bcopy((caddr_t)&save, (caddr_t)&u.u_qsav, sizeof (label_t));
	if (e == ENOEXEC && (xs->xs_flags & EXF_FIRST))
		e = xnext(xs, a);
	return e;
}

/*
 * modadm(MOD_TY_EXEC): a slot for (name, magic).  EXF_FIRST puts its
 * row before the first row with that magic; otherwise it is appended,
 * which a static row of the same magic would shadow (EEXIST).
 */
int
dlm_xreg(name, er)
	char *name;
	struct mod_execreg *er;
{
	struct dlm_xslot *xs, *fr = 0;
	int (*core)();
	int i, k, mg = er->er_magic & 0xffff;

	for (k = 0; k < EXEC_RESERVE; k++) {
		xs = &dlm_xslot[k];
		if (xs->xs_name[0] == 0) {
			if (fr == 0)
				fr = xs;
		} else if ((xs->xs_magic & 0xffff) == mg)
			return strcmp(xs->xs_name, name) == 0 ? 0 : EEXIST;
	}
	for (i = 0; i < nexectype && !ismagic(i, mg); i++)
		;
	if (i < nexectype && !(er->er_flags & EXF_FIRST))
		return EEXIST;
	if (fr == 0 || nexectype >= NEXECSW)
		return ECONFIG;
	core = i < nexectype ? execsw[i].exec_core : (int (*)())nodev;
	for (k = nexectype; k > i; k--)
		execsw[k] = execsw[k - 1];
	strcpy(fr->xs_name, name);
	fr->xs_magic = er->er_magic;
	fr->xs_flags = er->er_flags & EXF_FIRST;
	execsw[i].exec_magic = &fr->xs_magic;
	execsw[i].exec_func = fr->xs_tramp;
	execsw[i].exec_core = core;
	nexectype++;
	return 0;
}

/* ---- exec linkage ---- */

static struct dlm_xslot *
xfind(m, mg)
	struct dlm_mod *m;
	int mg;
{
	int k;

	for (k = 0; k < EXEC_RESERVE; k++)
		if ((dlm_xslot[k].xs_magic & 0xffff) == mg &&
		    strcmp(dlm_xslot[k].xs_name, m->m_name) == 0)
			return &dlm_xslot[k];
	return 0;
}

/* td: run address of the mod_type_data; its mtd_pdata is the array */
static int
exec_install(m, td)
	struct dlm_mod *m;
	unsigned long td;
{
	struct dlm_xslot *xs;
	unsigned long d, d0, f;

	if (!inimg(m, td, 8L))
		return ERELOC;
	d0 = G32(DLM_RP(m, td) + 4);
	for (d = d0;; d += EXDSZ) {
		if (!inimg(m, d, (unsigned long)EXDSZ))
			return ERELOC;
		if ((f = G32(DLM_RP(m, d) + 4)) == 0)
			break;
		if (!inimg(m, f, 2L))
			return ERELOC;
		xs = xfind(m, (int)G16(DLM_RP(m, d)));
		if (xs == 0 || (xs->xs_mod && xs->xs_mod != m))
			return EINVAL;
	}
	if (d == d0)
		return EINVAL;
	for (d = d0; (f = G32(DLM_RP(m, d) + 4)) != 0; d += EXDSZ) {
		xs = xfind(m, (int)G16(DLM_RP(m, d)));
		xs->xs_mod = m;
		xs->xs_func = f;
	}
	return 0;
}

static int
exec_remove(m, td)
	struct dlm_mod *m;
	unsigned long td;
{
	int k;

	for (k = 0; k < EXEC_RESERVE; k++)
		if (dlm_xslot[k].xs_mod == m) {
			dlm_xslot[k].xs_mod = 0;
			dlm_xslot[k].xs_func = 0;
		}
	return 0;
}

static void
exec_info(m, td, st)
	struct dlm_mod *m;
	unsigned long td;
	struct modspecific_stat *st;
{
	int k;

	st->mss_type = MOD_TY_EXEC;
	st->mss_p0[0] = st->mss_p0[1] = st->mss_p1[0] = st->mss_p1[1] = -1;
	for (k = 0; k < EXEC_RESERVE; k++)
		if (dlm_xslot[k].xs_mod == m) {
			st->mss_p0[0] = dlm_xslot[k].xs_magic & 0xffff;
			st->mss_p0[1] = rowof(dlm_xslot[k].xs_tramp);
			break;
		}
}

struct mod_operations mod_execops = { exec_install, exec_remove, exec_info };

/* ---- hook linkage ---- */

/* the hooksw row named by the module string at run address a */
static struct hooksw *
hkfind(m, a)
	struct dlm_mod *m;
	unsigned long a;
{
	struct hooksw *h;
	char buf[32];
	int i;

	for (i = 0; i < sizeof buf - 1 && inimg(m, a + i, 1L); i++)
		if ((buf[i] = *DLM_RP(m, a + i)) == 0)
			break;
	if (i == 0 || i == sizeof buf - 1 || buf[i] != 0)
		return 0;
	for (h = hooksw; h->hk_name; h++)
		if (strcmp(h->hk_name, buf) == 0)
			return h;
	return 0;
}

static int
hook_install(m, td)
	struct dlm_mod *m;
	unsigned long td;
{
	struct hooksw *h;
	unsigned long d, d0, n, f;

	if (!inimg(m, td, 8L))
		return ERELOC;
	d0 = G32(DLM_RP(m, td) + 4);
	for (d = d0;; d += HKDSZ) {
		if (!inimg(m, d, (unsigned long)HKDSZ))
			return ERELOC;
		if ((n = G32(DLM_RP(m, d))) == 0)
			break;
		f = G32(DLM_RP(m, d) + 4);
		if (!inimg(m, f, 2L))
			return ERELOC;
		if ((h = hkfind(m, n)) == 0)
			return EINVAL;
		if (h->hk_owner && h->hk_owner != (char *)m)
			return EEXIST;
	}
	for (d = d0; (n = G32(DLM_RP(m, d))) != 0; d += HKDSZ) {
		h = hkfind(m, n);
		h->hk_owner = (char *)m;
		*h->hk_ptr = (char *)G32(DLM_RP(m, d) + 4);
	}
	return 0;
}

static int
hook_remove(m, td)
	struct dlm_mod *m;
	unsigned long td;
{
	struct hooksw *h;

	for (h = hooksw; h->hk_name; h++)
		if (h->hk_owner == (char *)m) {
			*h->hk_ptr = h->hk_dflt;
			h->hk_owner = 0;
		}
	return 0;
}

static void
hook_info(m, td, st)
	struct dlm_mod *m;
	unsigned long td;
	struct modspecific_stat *st;
{
	st->mss_type = MOD_TY_MISC;
	st->mss_p0[0] = st->mss_p0[1] = st->mss_p1[0] = st->mss_p1[1] = -1;
}

struct mod_operations mod_hookops = { hook_install, hook_remove, hook_info };
