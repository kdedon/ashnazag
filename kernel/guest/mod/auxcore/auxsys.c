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
extern int runrun;

int aux_lstatdiag;
long aux_lstatpid,aux_lstatcount;
long aux_lstatrows[32][36],aux_lstatmeta[32][52],aux_lstatrequest[32][40];
extern timestruc_t hrestime;
static int aux_vur();

static void
lstatdiag(gp,num,a,e,value,r)
struct guest_proc *gp;
int num,e;
long *a,value;
char *r;
{
    long *v,*m,*q;char *name;unsigned long sp,cache;int i;unsigned char sig[10],caller[16],request[32];
    if(!aux_lstatdiag || num!=111 || gp->gp_proc->p_pid!=aux_lstatpid ||
        !(AUXP(gp)->ap_mac & APM_TASK) || aux_lstatcount<0 || aux_lstatcount>=32)return;
    v=aux_lstatrows[aux_lstatcount];m=aux_lstatmeta[aux_lstatcount];
    q=aux_lstatrequest[aux_lstatcount++];bzero((caddr_t)q,40*4);
    bzero((caddr_t)v,36*4);bzero((caddr_t)m,52*4);
    sp=GR_USP(r);
    v[0]=gp->gp_proc->p_pid;v[1]=GR_PC(r);v[2]=sp;v[3]=e;
    v[4]=value;v[5]=lbolt;v[6]=u.u_procp->p_cursig;
    v[7]=a[0];v[8]=a[1];
    v[10]=copyin((caddr_t)sp,(caddr_t)(v+12),32)?-1:0;
    m[1]=a[1];m[3]=v[18];m[4]=hrestime.tv_sec;m[5]=hrestime.tv_nsec;
    if(!e && aux_vur(gp,(caddr_t)a[1],58) &&
        !copyin((caddr_t)a[1],(caddr_t)(m+8),58))m[0]|=1;
    if(!v[10] && v[12]==0x17b80 && v[18]==0x2145a &&
        GR_PC(r)==0x4578c &&
        !copyin((caddr_t)0x17b7a,(caddr_t)sig,6) &&
        sig[0]==0x61 && sig[1]==0xff && G32(sig+2)==0x0002dc04 &&
        !copyin((caddr_t)0x21454,(caddr_t)sig,6) &&
        sig[0]==0x61 && sig[1]==0xff && G32(sig+2)==0xffff670c &&
        !copyin((caddr_t)0x2150e,(caddr_t)sig,10) &&
        G32(sig)==0x202b0012 && G16(sig+4)==0xb0b9 && G32(sig+6)==0x0008aa64) {
        cache=(unsigned long)v[17];m[2]=cache;m[0]|=2;
        if(cache<=~0UL-96 && aux_vur(gp,(caddr_t)cache,96) &&
            !copyin((caddr_t)cache,(caddr_t)(m+23),96))m[0]|=4;
        if(!copyin((caddr_t)0x8aa64,(caddr_t)(m+6),4))m[0]|=8;
        if(sp<=~0UL-576 && aux_vur(gp,(caddr_t)(sp+568),8) &&
            !copyin((caddr_t)(sp+568),(caddr_t)(m+47),8))m[0]|=16;
        if(sp<=~0UL-576 && aux_vur(gp,(caddr_t)(sp+552),16) &&
            !copyin((caddr_t)(sp+552),(caddr_t)caller,16)) {
            m[7]=G32(caller);m[49]=G32(caller+4);
            m[50]=G32(caller+8);m[51]=G32(caller+12);m[0]|=32;
            if(m[7]==0x217c0 &&
                !copyin((caddr_t)0x2175e,(caddr_t)sig,8) &&
                G32(sig)==0x48e73f30 && G32(sig+4)==0x9efc00fc &&
                !copyin((caddr_t)0x217bc,(caddr_t)sig,6) &&
                G32(sig)==0x6100fc10 && G16(sig+4)==0x2f40 &&
                sp<=~0UL-892 && aux_vur(gp,(caddr_t)(sp+852),40) &&
                !copyin((caddr_t)(sp+852),(caddr_t)caller,16) &&
                !copyin((caddr_t)(sp+860),(caddr_t)request,32)) {
                q[0]=1;q[1]=G32(request);q[2]=G32(request+4);
                q[3]=G32(request+8);q[4]=G32(caller);
                for(i=0;i<8;i++)q[6+i]=G32(request+i*4);
                if(q[1]==0x287d4 &&
                    !copyin((caddr_t)0x287ce,(caddr_t)sig,6) &&
                    G32(sig)==0x61ffffff && G16(sig+4)==0x8f8e &&
                    (unsigned long)q[4]<=~0UL-96 &&
                    aux_vur(gp,(caddr_t)q[4],96) &&
                    !copyin((caddr_t)q[4],(caddr_t)(q+14),96))q[0]|=2;
            }
        }
    }
    name=(char *)(v+20);v[9]=-2;
    for(i=0;i<63;i++) {
        if((unsigned long)a[0]+i<(unsigned long)a[0] ||
            copyin((caddr_t)((unsigned long)a[0]+i),name+i,1)) {
            name[i]=0;v[9]=-1;break;
        }
        if(!name[i]) {v[9]=i;break;}
    }
}

int aux_filediag;
long aux_filecalls[2],aux_fileerrors[2],aux_filecount,aux_filefailedcount,aux_filebad;
long aux_filelast[32][8],aux_filefailed[16][8];
long aux_openfailed[16][23],aux_openfailedcount;

static void
filediag(gp,num,a,e,value,pc)
    struct guest_proc *gp;
    int num,e;
    long *a,value,pc;
{
    struct aux_proc *ap=AUXP(gp);
    file_t *fp;
    long row[8];
    int k,i;
    if((ap->ap_mac & (APM_TASK|APM_COFF)) != (APM_TASK|APM_COFF) ||
        (num!=3 && num!=19 && num!=5))return;
    if(aux_filediag==2) {
        bzero((caddr_t)aux_filecalls,sizeof aux_filecalls);
        bzero((caddr_t)aux_fileerrors,sizeof aux_fileerrors);
        bzero((caddr_t)aux_filelast,sizeof aux_filelast);
        bzero((caddr_t)aux_filefailed,sizeof aux_filefailed);
        aux_filecount=aux_filefailedcount=aux_filebad=aux_openfailedcount=0;
        bzero((caddr_t)aux_openfailed,sizeof aux_openfailed);aux_filediag=1;
    }
    if(num==5) {
        if(e && aux_openfailedcount<16) {
            long *record=aux_openfailed[aux_openfailedcount++];
            char *name=(char *)(record+7);
            record[0]=gp->gp_proc->p_pid;record[1]=e;record[2]=a[1];
            record[3]=a[2];record[4]=a[0];record[5]=pc;
            for(i=0;i<63;i++) {
                if(copyin((caddr_t)(a[0]+i),name+i,1)){name[i]=0;record[6]=-1;break;}
                record[6]=i+1;
                if(!name[i])break;
            }
        }
        return;
    }
    if(aux_filecount>=4096){aux_filediag=0;return;}
    k=num==19;aux_filecalls[k]++;
    row[0]=gp->gp_proc->p_pid;row[1]=num;row[2]=a[0];row[3]=e;
    row[4]=value;row[5]=row[6]=row[7]=0;
    if(!getf((int)a[0],&fp)) {
        row[5]=(long)fp;row[6]=(long)fp->f_vnode;row[7]=fp->f_offset;
    }
    for(i=0;i<8;i++)aux_filelast[aux_filecount&31][i]=row[i];
    aux_filecount++;
    if(e) {
        aux_fileerrors[k]++;aux_filebad++;
        if(aux_filefailedcount<16) {
            for(i=0;i<8;i++)aux_filefailed[aux_filefailedcount][i]=row[i];
            aux_filefailedcount++;
        }
    }
}

int aux_polldiag;
long aux_pollintr,aux_pollretry,aux_polllast[4];
long aux_nsys, aux_lastsys;	/* Mac task's calls; the last: number << 16 | errno */
int aux_trace = 0;	/* 1: every call, 2: results, 4: open paths, 8: failures, name changes,
			   16: keep the first AUX_TBUF bytes */
int aux_execaux2 = 0;	/* the image being exec'd is an A/UX 2 one */
void (*aux_macdetach)() = 0;
void (*aux_macgfd)() = 0;
int (*aux_slotmgr)() = 0;
void (*aux_uitick)() = 0;

/*
 * Trace ring: "pid text value" lines, readable through /dev/kmem
 * (aux_tbuf, aux_tpos = bytes written; with aux_trace 16 the lines that fit).
 */
char aux_tbuf[AUX_TBUF] = { 0 };
long aux_tpos = 0;

static void
tput(s)
	char *s;
{
	for (; *s; aux_tpos++)
		if (aux_tpos < AUX_TBUF || !(aux_trace & 16))
			aux_tbuf[aux_tpos % AUX_TBUF] = *s++;
		else
			s++;
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
	long p0 = aux_tpos;
	int sr;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (sr) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (sr | 0x700) : "memory");
	tdec((long)pid);
	tput(" ");
	tput(s);
	tput(" ");
	tdec(v);
	tput("\n");
	if ((aux_trace & 16) && aux_tpos > AUX_TBUF)
		aux_tpos = p0;		/* a line that does not fit */
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

/* trace ring: "tag a0..a{n-1}" in hex, nb bytes at ua if readable; the result */
void
aux_thex(tag, a, n, ua, nb, v)
	char *tag;
	long *a, ua, v;
	int n, nb;
{
	static char hx[] = "0123456789abcdef";
	unsigned char m[16];
	char t[128], *p = t;
	int i;

	while (*tag)
		*p++ = *tag++;
	for (; n > 0; n--, a++)
		for (*p++ = ' ', i = 28; i >= 0; i -= 4)
			*p++ = hx[*a >> i & 15];
	if (nb > (int)sizeof m)
		nb = sizeof m;
	if (ua && nb > 0 && copyin((caddr_t)ua, (caddr_t)m, (u_int)nb) == 0)
		for (*p++ = ' ', *p++ = ':', i = 0; i < nb; i++) {
			*p++ = hx[m[i] >> 4];
			*p++ = hx[m[i] & 15];
		}
	*p = 0;
	aux_tlog((int)u.u_procp->p_pid, t, v);
}

/* socket calls with their arguments and addresses or option values */
static void
socktrace(num, a, v)
	int num;
	long *a, v;
{
	char t[8];
	long ua = 0;
	int nb = 0;

	switch (num) {
	case 70: case 75: case 76:
		ua = v < 0 ? 0 : a[1];
		nb = 16;
		break;
	case 71: case 72:
		ua = a[1];
		nb = 16;
		break;
	case 77: case 90:
		ua = a[3];
		nb = 4;
		break;
	}
	bcopy("sock ", t, 5);
	t[5] = '0' + num / 10;
	t[6] = '0' + num % 10;
	t[7] = 0;
	aux_thex(t, a, 5, ua, nb, v);
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
		if (aux_trace & 8)
			aux_tlog((int)p->p_pid, "nosys", (long)num);
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
	if(aux_lstatdiag)lstatdiag(gp,num,a,e,rv.r_val1,r);
	if(aux_filediag)filediag(gp,num,a,e,rv.r_val1,GR_PC(r));
	if (!e && aux_macgfd && (num==5 || num==6 || num==41 || num==62))
		(*aux_macgfd)(gp);
	if (aux_trace & 2)
		printf("aux %d: %d -> %d %x %x\n", (int)p->p_pid, num, e, rv.r_val1,
		    rv.r_val2);
	if (mac_socktrace && num >= 70 && num <= 93)
		socktrace(num, a, e ? (long)-e : (long)rv.r_val1);
	if (e == EINTR || e == ERESTART) {
        if(aux_polldiag && (num==38 || num==50) && a[0]==0x6e) {
            aux_pollintr++;aux_polllast[0]=ap->ap_compat;aux_polllast[1]=p->p_cursig;
            aux_polllast[2]=u.u_sigrestart;aux_polllast[3]=GR_PC(r);
            if((ap->ap_compat & COMPAT_BSDSIGNALS) && p->p_cursig &&
               (u.u_sigrestart & 1L << (p->p_cursig-1)) &&
               ((ap->ap_compat & COMPAT_SYSCALLS) || aux_restartable(num)))aux_pollretry++;
        }
		/* Polling must return to the button loop after a timer signal. */
		if (num != 82 && !((num==38 || num==50) && a[0]==0x6e) &&
            (ap->ap_compat & COMPAT_BSDSIGNALS) && p->p_cursig &&
		    (u.u_sigrestart & 1L << (p->p_cursig - 1)) &&
		    ((ap->ap_compat & COMPAT_SYSCALLS) || aux_restartable(num))) {
			GR_PC(r) -= 2;
			goto out;
		}
		e = EINTR;
	}
	if (ap->ap_mac) {
		aux_nsys++;
		aux_lastsys = (long)num << 16 | e;
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
	ap->ap_envp = ap->ap_envc = ap->ap_envlo = ap->ap_envhi = 0;
	if (!(ap->ap_compat & COMPAT_EXEC))
		ap->ap_compat = COMPAT_DEFAULT;
	ap->ap_flags = aux_execaux2 ? APF_AUX2 : 0;
	ap->ap_sv_onstack = ap->ap_sv_intr = 0;
	ap->ap_ss_sp = 0;
	ap->ap_ss_onstack = 0;
	return 0;
}

/* the child's ITIMER_REAL and alarm start disarmed */
static int
aux_pfork(pg, cg)
	struct guest_proc *pg, *cg;
{
	AUXP(cg)->ap_itid = 0;
	AUXP(cg)->ap_alid = 0;
	AUXP(cg)->ap_mac = 0;		/* the child is no task of the layer */
	return 0;
}

static void
aux_pexit(gp)
	struct guest_proc *gp;
{
	AUXP(gp)->ap_envp = AUXP(gp)->ap_envc = 0;
	AUXP(gp)->ap_envlo = AUXP(gp)->ap_envhi = 0;
	aux_fidexit(gp->gp_proc);
	aux_itstop(AUXP(gp));
	aux_alstop(AUXP(gp), gp->gp_flags & GPF_EXEC);
	if (AUXP(gp)->ap_mac && aux_macdetach)
		(*aux_macdetach)(gp);
}

/*
 * User ranges: A/UX shared libraries and the Mac ROM live in quadrant
 * 1 (0x40000000-0x7fffffff), which the stock rule refuses.  A range
 * in quadrant 0 or 1 must not wrap or leave its quadrant, and may end
 * at its last byte (the Mac's stack at 0x3fff0000), which the stock
 * rule refuses too; others keep the stock rule.
 */
static int
aux_vur(gp, a, len)
	struct guest_proc *gp;
	caddr_t a;
	u_int len;
{
	u_long s = (u_long)a, e = s + len - 1;

	if ((s >> 30) > 1)
		return __amix_valid_usr_range(a, len);
	return len != 0 && e >= s && (e >> 30) == (s >> 30);
}

struct guest_profile aux_profile = { "aux" };
int	(*aux_alwatch)() = 0;

/* vector 10: the layer sees a watched task's A-line first */
static int
aux_aline(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	if ((gp->gp_flags & GPF_AWATCH) && aux_alwatch && (*aux_alwatch)(gp, r)) {
		guest_trapret();
		return 0;
	}
	return guest_aline(gp, r, v);
}

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
	aux_profile.gpf_fpu040 = 1;
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
	aux_profile.gpf_disp[11] = *d;
	aux_profile.gpf_disp[11].gd_fn = guest_fline;
	aux_profile.gpf_disp[4] = *d;
	aux_profile.gpf_disp[4].gd_fn = guest_illeg;
	d = &aux_profile.gpf_disp[10];
	d->gd_kind = GD_REFLECT;
	d->gd_flags = GDF_USER;
	d->gd_fn = aux_aline;
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

MOD_STR_WRAPPER(auxcore, auxcore_load, auxcore_unload, "A/UX system calls");
