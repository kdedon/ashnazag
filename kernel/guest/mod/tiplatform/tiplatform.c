/*
 * tiplatform.c -- /dev/tiplatform and the Voyage 200 profile.
 *
 * AMS runs natively in user mode from its own reset vector, with the
 * supervisor state virtual (guestcore).  The CPU is a 68000 to it:
 * exceptions and interrupts push 6-byte frames through the vectors in
 * its RAM at 0, and rte pops them.  The register pages are memory;
 * the routines that need a live answer (key scan, idle halt, Flash
 * program and erase) are patched by the host to jump to service stubs
 * at TI_HOST, whose illegal instruction lands here.  TI_SIG carries the
 * interrupts from the clock tick, held while the guest's IPL masks them.
 *
 *	tick --> pend --> TI_SIG --> 6-byte frame --> AMS handler --> rte
 *	TI_KEYS (display process) --> keys --> key scan service
 *
 * K&R C.
 */

#include "ti.h"
#include "sys/mman.h"
#include "vm/as.h"
#include "guest_rpage.h"
#include "guest_env.h"

extern int nodev();
extern void dlm_cacheflush(), flushmmu();
__asm__(".weak cputype");
extern long ti_cputype __asm__("cputype");	/* 30, 40, 60; absent: 68030 */
static long *__volatile__ cpup = &ti_cputype;
extern struct modwrapper tiplatform_wrapper;

/* a session's calculator; gs_pend bit n: autovector n waits */
struct tictr {
	int		flags, nnote, nexc;
	unsigned long	scanout, modtab;
	unsigned char	keys[10], on;
	unsigned long	walked;		/* tick of the last uncache() */
	int		n20, n1;	/* ticks to the next 20 Hz and 1 Hz interrupt */
	struct ti_info	st;
};
#define	TIC(s)	((struct tictr *)(s)->gs_priv)

static struct guest_env env;
static struct tictr tinone;		/* no session: counts go nowhere */
static int device_flag;
static void uncache();

static struct guest_sess *
sess(gp)
	struct guest_proc *gp;
{
	return guest_env_sess(gp);
}

static struct tictr *
tip(gp)
	struct guest_proc *gp;
{
	struct guest_sess *s = sess(gp);

	return s ? TIC(s) : &tinone;
}

static void
note(t, what, a, pc)
	struct tictr *t;
	char *what;
	long a, pc;
{
	t->st.faults++;
	if (t->nnote++ < 10)
		printf("NOTICE: Voyage %s %x pc %x\n", what, (int)a, (int)pc);
}

static void
ti_note(s, what, a, pc)
	struct guest_sess *s;
	char *what;
	long a, pc;
{
	note(TIC(s), what, a, pc);
}

/*
 * Autovector 1 every tick (the keyboard scan), 5 at 20 Hz (AMS's
 * timers and cursor), 3 at 1 Hz (the clock).
 */
static void
tick(s)
	struct guest_sess *s;
{
	struct tictr *t = TIC(s);

	t->st.ticks++;
	s->gs_pend |= 1 << 1;
	if (--t->n20 <= 0) {
		t->n20 = HZ / 20;
		s->gs_pend |= 1 << 5;
	}
	if (--t->n1 <= 0) {
		t->n1 = HZ;
		s->gs_pend |= 1 << 3;
	}
}

static int
ack(s, r, l)
	struct guest_sess *s;
	char *r;
	int l;
{
	s->gs_pend &= ~(1UL << l);
	if ((TIC(s)->flags & TIF_CENSUS) && s->gs_ints < 8)
		printf("tiplatform: census interrupt %d pc %x sr %x\n", l, (int)GR_PC(r),
		    guest_getsr(s->gs_gp, r));
	return l + 24;
}

/* the calculator's pages each second */
static void
walk(t)
	struct tictr *t;
{
	if (t->st.ticks - t->walked >= HZ) {
		t->walked = t->st.ticks;
		uncache(curproc->p_as, TI_FLASH);
	}
}

/*
 * AMS copies code into RAM and runs it (its Flash routines run at $5B02,
 * built afresh for each command); a 68000 has no caches, so nothing
 * flushes.  On a 68040/060 the copy would sit in the data cache while
 * the instruction cache fetches memory or an older routine.  So RAM is
 * cache-inhibited, serialized: every fetch sees the last store.  Flash
 * stays cached; it changes only in fput(), which pushes the caches.
 * A page refaulted after paging comes back cacheable, so the calculator's
 * pages are walked again each second.
 */
static void
uncache(as, hi)
	struct as *as;
	unsigned long hi;
{
	long *cpu = cpup;
	unsigned long *root, a, b, *leaf, va;
	int n = 0;

	if (!cpu || *cpu < 40 || !(root = *(unsigned long **)((char *)as + 20)))
		return;
	for (va = 0; va < hi; va += 0x1000) {
		if (!((a = root[va >> 25]) & 2) ||
		    !((b = ((unsigned long *)(a & 0xfffffe00UL))[(va >> 18) & 0x7f]) & 2))
			continue;
		leaf = (unsigned long *)(b & 0xffffff00UL) + ((va >> 12) & 0x3f);
		if ((*leaf & 1) && (*leaf & 0x60) != 0x40) {
			*leaf = (*leaf & ~0x60UL) | 0x40;
			flushmmu((addr_t)va, 1);
			n++;
		}
	}
	if (n)
		dlm_cacheflush();	/* lines the pages had go to memory */
}

/* ---- services ---- */

static int
gword(a)
	unsigned long a;
{
	unsigned char b[2];

	return copyin((caddr_t)a, (caddr_t)b, 2) ? 0 : G16(b);
}

static int
pword(a, v)
	unsigned long a;
	int v;
{
	unsigned char b[2];

	P16(b, v);
	return copyout((caddr_t)b, (caddr_t)a, 2);
}

/*
 * AMS's key scan: the one key down as row * 8 + column + 1 (ON $48,
 * arrows $50 + their bits), row 0's modifiers in the high byte; 0 and
 * d0.w clear for none or two keys.  Only modifiers: AMS's own table.
 */
static void
scan(t, r)
	struct tictr *t;
	char *r;
{
	int row, b, bits, code = 0, found = 0;

	if (t->on)
		code = 0x48, found = 1;
	else {
		for (bits = 0, row = 0; row < 10; row++)
			bits |= t->keys[row];
		if (!bits)
			goto none;
		for (row = 9; row > 0; row--) {
			if (!(bits = t->keys[row]))
				continue;
			if (found)
				goto none;
			for (b = 7; !(bits & 1 << b); b--)
				;
			if (bits & ~(1 << b))
				goto none;
			code = row * 8 + b + 1;
			found = 1;
		}
	}
	if ((bits = t->keys[0]) != 0) {
		if (bits >> 4) {
			if (found)
				goto none;
			code = 0x50 + (bits >> 4);
			found = 1;
		}
		code = found ? (bits & 15) << 8 | (code & 0xff) :
		    gword(t->modtab + 2 * (bits & 15));
	}
	pword(t->scanout, code);
	GR_D(r, 0) |= 0xffff;
	return;
none:
	pword(t->scanout, 0);
	GR_D(r, 0) &= ~0xffffL;
}

/* a store: AMS's words land as written; a freed block is ones, AMS's empty */
static void
prog(t, r)
	struct tictr *t;
	char *r;
{
	unsigned long src = GR_A(r, 2), dst = GR_A(r, 3), n = GR_D(r, 4) & 0xffff;

	for (; n > 0; n--, src += 2, dst += 2)
		if (dst >= TI_FLASH && dst < TI_FLASH + TI_FLASH_SIZE)
			pword(TI_ALIAS + (dst - TI_FLASH), gword(src));
	GR_A(r, 2) = src;
	GR_A(r, 3) = dst;
	GR_D(r, 4) &= ~0xffffL;
	t->st.flash++;
	dlm_cacheflush();
}

static void
sprog(t, r)
	struct tictr *t;
	char *r;
{
	unsigned long a[3];

	if (copyin((caddr_t)GR_USP(r) + 4, (caddr_t)a, sizeof a))
		return;
	if (a[1] < TI_FLASH || a[1] >= TI_FLASH + TI_FLASH_SIZE || a[2] > TI_FLASH_SIZE ||
	    a[1] + a[2] > TI_FLASH + TI_FLASH_SIZE)
		return;
	for (; (long)a[2] > 0; a[2] -= 2, a[0] += 2, a[1] += 2)
		pword(TI_ALIAS + (a[1] - TI_FLASH), gword(a[0]));
	t->st.flash++;
	dlm_cacheflush();
}

static void
eblock(t, a)
	struct tictr *t;
	unsigned long a;
{
	static unsigned char ones[256];
	unsigned long i;

	if (!ones[0])
		for (i = 0; i < sizeof ones; i++)
			ones[i] = 0xff;
	a = TI_ALIAS + ((a - TI_FLASH) & ~0xffffUL);
	for (i = 0; i < 0x10000; i += sizeof ones)
		(void)copyout((caddr_t)ones, (caddr_t)(a + i), sizeof ones);
	t->st.flash++;
}

static int
inflash(a)
	unsigned long a;
{
	return a >= TI_FLASH && a < TI_FLASH + TI_FLASH_SIZE;
}

static void
erase(t, r)
	struct tictr *t;
	char *r;
{
	unsigned long a;

	if (copyin((caddr_t)GR_USP(r) + 4, (caddr_t)&a, 4) || !inflash(a))
		return;
	eblock(t, a);
	GR_D(r, 0) = (GR_D(r, 0) & ~0xffffL) | 0x8080;
	dlm_cacheflush();
}

static void
move(t, r)
	struct tictr *t;
	char *r;
{
	static unsigned char b[256];
	unsigned long src, dst = GR_A(r, 1) & ~0xffffUL, i;

	if (copyin((caddr_t)GR_USP(r) + 4, (caddr_t)&src, 4) || !inflash(src) || !inflash(dst))
		return;
	src &= ~0xffffUL;
	eblock(t, dst);
	for (i = 0; i < 0x10000; i += sizeof b)
		if (copyin((caddr_t)(src + i), (caddr_t)b, sizeof b) == 0)
			(void)copyout((caddr_t)b, (caddr_t)(TI_ALIAS + (dst - TI_FLASH) + i), sizeof b);
	eblock(t, src);
	dlm_cacheflush();
}

/* ---- dispositions ---- */

/* an exception through the guest's own vector */
static int
exc(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	struct tictr *t = tip(gp);

	if ((t->flags & TIF_CENSUS) && t->nexc++ < 40)
		printf("tiplatform: census vector %d pc %x\n", v, (int)GR_PC(r));
	if (v == 3 || v == 4)
		note(t, v == 3 ? "address error" : "illegal instruction", (long)v, GR_PC(r));
	if (guest_reflect(gp, r, GR_PC(r), v << 2, (char *)0, 2, -1) < 0) {
		note(t, "exception without a stack", (long)v, GR_PC(r));
		psignal(curproc, SIGSEGV);
	}
	if (GR_FV(r) >> 12)
		u.u_sigflag |= USTKCLEAR;
	guest_trapret();
	return 0;
}

/* a Flash store, or an access past Flash nothing is mapped at: AMS reached a routine nobody patched */
static int
hwfault(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned long fa;
	int ssw;

	if (guest_bfault(r, &fa, &ssw) < 0 || fa < TI_FLASH || TI_MAPPED(fa) ||
	    (fa < TI_FLASH + TI_FLASH_SIZE && (ssw & 0x100)))
		return 1;
	tip(gp)->st.io++;
	note(tip(gp), "hardware access", (long)fa, GR_PC(r));
	return guest_berr(gp, r, fa, ssw);
}

/* a stub at TI_HOST: the service; guestcore steps to its rts */
static int
service(s, r, n)
	struct guest_sess *s;
	char *r;
	int n;
{
	struct tictr *t = TIC(s);
	unsigned long buf;

	switch (n) {
	case TS_SCAN:
		scan(t, r);
		break;
	case TS_MODS:
		GR_D(r, 0) = gword(t->modtab + 2 * (t->keys[0] & 15));
		break;
	case TS_ROWS:
		if (copyin((caddr_t)GR_USP(r) + 4, (caddr_t)&buf, 4) == 0)
			(void)copyout((caddr_t)t->keys, (caddr_t)buf, 10);
		GR_D(r, 0) |= 0xffff;
		break;
	case TS_HALT:		/* off until one of autovectors 1-5, or ON */
		s->gs_halt = (GR_D(r, 0) & 0x1f) << 1 | 1 << 6;
		break;
	case TS_PROG:
		prog(t, r);
		break;
	case TS_ERASE:
		erase(t, r);
		break;
	case TS_WRITE:
		sprog(t, r);
		break;
	case TS_MOVE:
		move(t, r);
		break;
	case TS_DARK:
	case TS_LIT:
		t->st.dark = n == TS_DARK;
		break;
	case TS_SIZE:
		GR_D(r, 0) = TI_FLASH_SIZE;
		GR_D(r, 1) = GR_A(r, 0) = TI_FLASH + TI_FLASH_SIZE;
		break;
	default:
		return -1;
	}
	walk(t);
	return 0;
}

/* vector 8: what a 68000 in the guest's mode would do */
static int
priv(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	int e;

	if ((tip(gp)->flags & TIF_CENSUS) && tip(gp)->nexc++ < 60)
		printf("tiplatform: census priv %x pc %x sp %x vsr %x\n",
		    (int)(fuword((caddr_t)GR_PC(r)) >> 16 & 0xffff), (int)GR_PC(r),
		    (int)GR_USP(r), guest_getsr(gp, r));
	walk(tip(gp));
	if ((e = guest_privtrap(gp, r, v)) <= 1)
		return e;
	return exc(gp, r, e);
}

static int
enter(s, arg)
	struct guest_sess *s;
	caddr_t arg;
{
	struct tictr *t = TIC(s);
	struct ti_enter te;

	if (copyin(arg, (caddr_t)&te, sizeof te))
		return EFAULT;
	t->st.lcd = TI_LCD;
	t->n20 = HZ / 20;
	t->n1 = HZ;
	t->flags = te.flags;
	t->scanout = te.scanout;
	t->modtab = te.modtab;
	uncache(curproc->p_as, TI_FLASH);
	dlm_cacheflush();
	return 0;
}

static int
keys(s, arg)
	struct guest_sess *s;
	caddr_t arg;
{
	struct tictr *t = TIC(s);
	struct ti_keys k;
	int i, x, down = 0;

	if (copyin(arg, (caddr_t)&k, sizeof k))
		return EFAULT;
	x = guest_splhi();
	for (i = 0; i < 10; i++) {
		down |= k.rows[i] & ~t->keys[i];
		t->keys[i] = k.rows[i];
	}
	if (down)
		s->gs_pend |= 1 << 2;
	if (k.on && !t->on)
		s->gs_pend |= 1 << 6;
	t->on = k.on;
	guest_env_kick(s);
	guest_splx(x);
	return 0;
}

static int
control(e, s, command, argument, mode, result)
	struct guest_env *e;
	struct guest_sess *s;
	int command, mode;
	caddr_t argument;
	int *result;
{
	struct tictr *t = TIC(s);

	switch (command) {
	case TI_KEYS:
		return s->gs_state == GS_RUN ? keys(s, argument) : ENXIO;
	case TI_INFO:
		if (s->gs_state != GS_RUN)
			return ENXIO;
		if (curproc != s->gs_proc)	/* the display process reads the LCD */
			uncache(curproc->p_as, (t->st.lcd + 3840 + 0xfff) & ~0xfffUL);
		t->st.ints = s->gs_ints;
		return copyout((caddr_t)&t->st, argument, sizeof t->st) ? EFAULT : 0;
	}
	return EINVAL;
}

static int
load()
{
	struct guest_disp *d;
	int v;

	bzero((caddr_t)&env, sizeof env);
	env.ge_name = "voyage200";
	env.ge_wrapper = &tiplatform_wrapper;
	env.ge_major = TI_MAJOR;
	env.ge_entercmd = TI_ENTER;
	env.ge_ssize = sizeof (struct tictr);
	env.ge_gpflags = GPF_FRAME68K | GPF_PRIV;
	env.ge_nsess = 1;
	env.ge_sig = TI_SIG;
	env.ge_holdfmt = 1 << 7 | 1 << 9 | 1 << 0xa | 1 << 0xb;
	env.ge_hostva = TI_HOST;
	env.ge_hostn = TS_N;
	env.ge_hoststep = 8;
	env.ge_hostvec = 4;
	env.ge_hostlen = 2;
	env.ge_enter = enter;
	env.ge_ack = ack;
	env.ge_tick = tick;
	env.ge_hostcall = service;
	env.ge_ioctl = control;
	env.ge_note = ti_note;
	for (v = 2; v < 48; v++) {
		if (v > 11 && v < 32)
			continue;
		d = &env.ge_prof.gpf_disp[v];
		d->gd_kind = GD_EMULATE;
		d->gd_flags = GDF_USER;
		d->gd_fn = v == 2 ? hwfault : v == 8 ? priv : exc;
	}
	return guest_env_add(&env);
}

static int
unload()
{
	return guest_env_del(&env);
}

static int
open_device(dev, flags, kind, credentials)
	dev_t *dev;
	int flags, kind;
	struct cred *credentials;
{
	return guest_env_open(&env, dev, flags, kind, credentials);
}

static int
close_device(dev, flags, kind, credentials)
	dev_t dev;
	int flags, kind;
	struct cred *credentials;
{
	return guest_env_close(&env, dev, flags, kind, credentials);
}

static int
ioctl_device(dev, command, argument, mode, credentials, result)
	dev_t dev;
	int command, mode;
	caddr_t argument;
	struct cred *credentials;
	int *result;
{
	return guest_env_ioctl(&env, dev, command, argument, mode, credentials, result);
}

struct mod_drv_data tiplatform_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	{ open_device, close_device, nodev, nodev, ioctl_device, nodev, nodev,
		nodev, nodev, nodev, 0, 0, &device_flag }, TI_MAJOR, 1 }
};
MOD_DRV_WRAPPER(tiplatform, load, unload, 0, "Voyage 200 native services");
