/*
 * auxsys.c -- A/UX system calls: the trap #0 / trap #15 entry, the
 * call table and the A/UX profile.
 *
 *	trap #0:  d0 = number (0: indirect, number at usp+4), arguments
 *	          on the user stack as for the AMIX system call path
 *	trap #15: d0 = number, arguments in a0, d1, a1, d2, a2, d3;
 *	          150 is the BSD signal return
 *	result:   carry clear, d0/d1 = values; carry set, d0 = A/UX errno
 *
 * Every A/UX number goes through auxcalls[]; numbers above 63 mean
 * other things on AMIX, so nothing is passed through by number.
 *
 * K&R C.
 */

#include "auxcore.h"
#include "sys/sysinfo.h"

extern int nosys();
extern int preempt(), issig();
extern void psig();
extern char runrun;

int aux_trace = 0;	/* 1: every call, 2: results, 4: open paths, 8: failures, name changes */
void (*aux_macdetach)() = 0;
int (*aux_slotmgr)() = 0;
void (*aux_uitick)() = 0;

/*
 * Trace ring: "pid text value" lines, readable through /dev/kmem
 * (aux_tbuf, aux_tpos = bytes ever written).
 */
char aux_tbuf[AUX_TBUF] = { 0 };
long aux_tpos = 0;

static void
tput(s)
	char *s;
{
	while (*s)
		aux_tbuf[aux_tpos++ % AUX_TBUF] = *s++;
}

static void
tdec(v)
	long v;
{
	char b[12];
	int n = sizeof b - 1;
	u_long w = v < 0 ? -v : v;

	b[n] = 0;
	do
		b[--n] = '0' + w % 10;
	while ((w /= 10) != 0);
	if (v < 0)
		b[--n] = '-';
	tput(b + n);
}

void
aux_tlog(pid, s, v)
	int pid;
	char *s;
	long v;
{
	int sr;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (sr) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (sr | 0x700) : "memory");
	tdec((long)pid);
	tput(" ");
	tput(s);
	tput(" ");
	tdec(v);
	tput("\n");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (sr) : "memory");
}

static struct auxent *auxtab[AUX_NCALL];
static unsigned char aux_warned[AUX_NCALL / 8];

/* call the AMIX handler n with the argument array */
int
aux_amix(n, a, rv)
	int n;
	long *a;
	rval_t *rv;
{
	u.u_syscall = n;
	return (*sysent[n].sy_call)(a, rv);
}

/*
 * The Mac File Manager rescans directories after names (1) or
 * attributes (2) changed; 4 and 8 when a process outside the Mac
 * environment made the change.
 */
static int
fmgrnote(ap, num, a)
	struct aux_proc *ap;
	int num;
	long *a;
{
	u_long b;

	switch (num) {
	case 5:
		if (!(a[1] & 0x100))		/* O_CREAT */
			return 0;
		/* fall through */
	case 8: case 9: case 10: case 14: case 108: case 109: case 112: case 123:
		b = 1;
		break;
	case 15: case 16: case 30: case 143: case 144: case 145: case 164: case 165:
		b = 2;
		break;
	default:
		return 0;
	}
	aux_fmgrflag |= ap->ap_mac & APM_TASK ? b : b << 2;
	return 1;
}

/* trace ring: "sys NNN" and the first argument, a path or hex; the result */
static void
tracecall(num, a, v)
	int num;
	long *a, v;
{
	char t[72];
	u_int i;

	bcopy("sys ", t, 4);
	t[4] = '0' + num / 100;
	t[5] = '0' + num / 10 % 10;
	t[6] = '0' + num % 10;
	t[7] = ' ';
	if (copyinstr((caddr_t)a[0], t + 8, sizeof t - 8, &i) ||
	    i < 2 || t[8] < ' ') {
		for (i = 0; i < 8; i++)
			t[8 + i] = "0123456789abcdef"[a[0] >> (28 - 4 * i) & 15];
		t[16] = 0;
	}
	aux_tlog((int)u.u_procp->p_pid, t, v);
}

/*
 * The entry, from the trap #0 / #15 gates of an A/UX process.  Mirrors
 * the stock system call path, ending with its preemption and signal
 * checks; the gate then leaves through the stock trap return.
 */
int
aux_systrap(gp, r, vec)
	struct guest_proc *gp;
	char *r;
	int vec;
{
	struct aux_proc *ap = AUXP(gp);
	struct proc *p = u.u_procp;
	struct auxent *ae;
	rval_t rv;
	long *a = (long *)u.u_arg;
	long usp;
	int num, e, held = 0;

	u.u_ar0 = (struct pcb *)r;
	sysinfo.syscall++;
	u.u_error = 0;
	u.u_syscall = 0;
	GR_SR(r) &= ~1;
	num = GR_D(r, 0) & 0xff;
	usp = GR_USP(r) + 4;
	if (vec == 47 && num == 150) {
		aux_sigcleanup(ap, r);
		goto out;
	}
	if (vec == 32 && num == 0) {
		num = fuword((caddr_t)usp) & 0xff;
		usp += 4;
	}
	ae = auxtab[num];
	if (ae == 0 || (ae->ae_flags & AE_NOSYS)) {
		if (aux_trace & 3)
			printf("aux %d: nosys %d\n", (int)p->p_pid, num);
		psignal(p, SIGSYS);
		goto out;
	}
	if (vec == 47) {
		a[0] = GR_A(r, 0);
		a[1] = GR_D(r, 1);
		a[2] = GR_A(r, 1);
		a[3] = GR_D(r, 2);
		a[4] = GR_A(r, 2);
		a[5] = GR_D(r, 3);
	} else if (ae->ae_narg && copyin((caddr_t)usp, (caddr_t)a, ae->ae_narg * 4)) {
		e = EFAULT;
		goto err;
	}
	u.u_ap = (int *)a;
	rv.r_val1 = 0;
	rv.r_val2 = GR_D(r, 1);
	if (aux_trace & 3)
		printf("aux %d: %d(%x, %x, %x)\n", (int)p->p_pid, num, (int)a[0],
		    (int)a[1], (int)a[2]);
	if (ae->ae_flags & AE_TODO) {
		if (!(aux_warned[num >> 3] & (1 << (num & 7)))) {
			aux_warned[num >> 3] |= 1 << (num & 7);
			printf("aux: call %d not supported yet\n", num);
		}
		e = EINVAL;
		goto err;
	}
	if (num == 11 || num == 59) {
		/* the new image may be native: keep the module until done */
		mod_hold(&auxcore_wrapper);
		held = 1;
	}
	if ((ae->ae_flags & AE_SETJMP) && setjmp(&u.u_qsav)) {
		e = u.u_error & 0xff;
		if (e == 0)
			e = EINTR;
	} else if (ae->ae_fn)
		e = (*ae->ae_fn)(ap, a, &rv, r);
	else
		e = aux_amix(ae->ae_amix, a, &rv);
	if (aux_trace & 2)
		printf("aux %d: %d -> %d %x %x\n", (int)p->p_pid, num, e, rv.r_val1,
		    rv.r_val2);
	if (e == EINTR || e == ERESTART) {
		if ((ap->ap_compat & COMPAT_BSDSIGNALS) && p->p_cursig &&
		    (u.u_sigrestart & 1L << (p->p_cursig - 1)) &&
		    ((ap->ap_compat & COMPAT_SYSCALLS) || aux_restartable(num))) {
			GR_PC(r) -= 2;
			goto out;
		}
		e = EINTR;
	}
	if (e) {
err:
		if (aux_trace & 8)
			tracecall(num, a, (long)-e);
		if (e == EFBIG)
			psignal(p, SIGXFSZ);
		GR_D(r, 0) = aux_errno_out(e, ap);
		GR_SR(r) |= 1;
	} else {
		GR_D(r, 0) = rv.r_val1;
		GR_D(r, 1) = rv.r_val2;
		if (fmgrnote(ap, num, a) && (aux_trace & 8))
			tracecall(num, a, 0L);
	}
out:
	if (runrun)
		preempt();
	if (p->p_cursig || p->p_sig || (p->p_flag & SPRSTOP))
		if (issig(0))
			psig();
	if (held)
		mod_rele(&auxcore_wrapper);
	return 0;
}

/* ---- profile ---- */

static int
aux_pexec(gp)
	struct guest_proc *gp;
{
	struct aux_proc *ap = AUXP(gp);

	if (ap->ap_mac && aux_macdetach)
		(*aux_macdetach)(gp);
	ap->ap_mac = 0;
	if (!(ap->ap_compat & COMPAT_EXEC))
		ap->ap_compat = COMPAT_DEFAULT;
	ap->ap_flags = 0;
	ap->ap_sv_onstack = ap->ap_sv_intr = 0;
	ap->ap_ss_sp = 0;
	ap->ap_ss_onstack = 0;
	return 0;
}

/* the child's ITIMER_REAL starts disarmed */
static int
aux_pfork(pg, cg)
	struct guest_proc *pg, *cg;
{
	AUXP(cg)->ap_itid = 0;
	AUXP(cg)->ap_mac = 0;		/* the child is no task of the layer */
	return 0;
}

static void
aux_pexit(gp)
	struct guest_proc *gp;
{
	aux_itstop(AUXP(gp));
	if (AUXP(gp)->ap_mac && aux_macdetach)
		(*aux_macdetach)(gp);
}

/*
 * User ranges: A/UX shared libraries and the Mac ROM live in quadrant
 * 1 (0x40000000-0x7fffffff), which the stock rule refuses.  A range
 * there must not wrap or leave the quadrant; others keep the stock rule.
 */
static int
aux_vur(gp, a, len)
	struct guest_proc *gp;
	caddr_t a;
	u_int len;
{
	u_long s = (u_long)a, e = s + len - 1;

	if ((s >> 30) != 1)
		return __amix_valid_usr_range(a, len);
	return len != 0 && e >= s && (e >> 30) == 1;
}

struct guest_profile aux_profile = { "aux" };

static int
auxcore_load()
{
	struct auxent *ae;
	struct guest_disp *d;

	for (ae = auxcalls; ae->ae_num >= 0; ae++) {
		if (ae->ae_fn == 0 && ae->ae_amix == 0 && !(ae->ae_flags & AE_NOSYS))
			ae->ae_flags |= AE_TODO;
		auxtab[ae->ae_num] = ae;
	}
	aux_profile.gpf_name = "aux";
	aux_profile.gpf_wrapper = &auxcore_wrapper;
	aux_profile.gpf_privsz = sizeof (struct aux_proc);
	aux_profile.gpf_exec = aux_pexec;
	aux_profile.gpf_fork = aux_pfork;
	aux_profile.gpf_exit = aux_pexit;
	aux_profile.gpf_vur = aux_vur;
	aux_profile.gpf_sendsig = aux_sendsig;
	d = &aux_profile.gpf_disp[32];
	d->gd_kind = GD_SYSCALL;
	d->gd_flags = GDF_USER;
	d->gd_fn = aux_systrap;
	aux_profile.gpf_disp[47] = *d;
	/* Mac tasks after UI_SET (gp_flags); others get the stock signal */
	d = &aux_profile.gpf_disp[8];
	d->gd_kind = GD_EMULATE;
	d->gd_flags = GDF_USER;
	d->gd_fn = guest_priv;
	d = &aux_profile.gpf_disp[10];
	d->gd_kind = GD_REFLECT;
	d->gd_flags = GDF_USER;
	d->gd_fn = guest_aline;
	d = &aux_profile.gpf_disp[2];
	d->gd_kind = GD_REFLECT;
	d->gd_flags = GDF_USER;
	d->gd_fn = guest_fnote;
	return guest_profile_add(&aux_profile);
}

static int
auxcore_unload()
{
	guest_profile_del(&aux_profile);
	aux_fidfree();
	return 0;
}

MOD_MISC_WRAPPER(auxcore, auxcore_load, auxcore_unload, "A/UX system calls");
