/*
 * uinter.c -- /dev/uinter0: the A/UX user-interface device as seen by
 * libmac1_s and Patch.067C.  One layer (the Mac environment), created
 * by startmac:
 *
 *	GETVERSION, TEST, UNMAP, MAP(0x3000), CREATELAYER, SHMID,
 *	ATTACHGFD, PHYS_SCREENS, ROM(0x40800000), SET(1)
 *
 * after which the task's A-line traps go to its low-memory vector $28
 * and its privileged instructions run against the virtual SR.  The
 * layer ends when its task exits or execs.
 *
 * Commands are A/UX's, passed unchanged by the personality's ioctl:
 * group 'Q', BSD direction bits and size; _IO commands take a value.
 * The screen is a slot $E card over a display session, whose keyboard
 * and mouse feed the event queue.  The rest of the A/UX set answers as
 * a machine without drives or SCSI would.
 *
 * K&R C.
 */

#include "uinter.h"
#include "sys/open.h"
#include "sys/sysmacros.h"
#include "sys/ipc.h"
#include "sys/shm.h"
#include "sys/resource.h"
#include "sys/file.h"
#include "vm/as.h"
#include "vm/seg.h"
#include "vm/seg_vn.h"

extern int nodev(), ttimeout(), untimeout();
extern k_sigset_t cantmask;
extern void (*aux_macdetach)(), (*aux_uitick)();
extern int (*aux_macpoll)();
extern struct shminfo shminfo;
extern struct shmid_ds shmem[];
extern int shmsys(), sync(), uadmin();
extern void dlm_cacheflush();
extern char ui_shmdssz[sizeof (struct shmid_ds) == 112 ? 1 : -1];	/* the kernel's */

struct uilayer ui;
static int ui_nopen;
static int uiflag[1] = { 0 };
int	uinter_trace = 0;	/* 1: print each ioctl */
int	uinter_adtest = 0;	/* 1: record root's Shut Down instead of halting */
int	uinter_adcall = 0;	/* the uadmin call recorded: cmd << 8 | fcn */
int	uinter_lapchk = 1;	/* 0: leave Patch.067C's AppleTalk queue calls alone */
char	uinter_pram[64] = "/etc/aux/pram";	/* XPRAM across sessions */
static int ui_pramdirty;
static unsigned char ui_pramsaved[256];
struct uicall ui_calls[UI_NCALL];
long	ui_ncalls;		/* entries ever made */
long	ui_nposted;		/* ticks posted */

/* Retained global files survive an application exec, then join its fd table. */
static file_t *ui_gfiles[AUX_GNOFILE];
static int ui_gfderr;

static void
ui_gfdclear()
{
    int i;
    file_t *fp;

    ui_gfderr=0;
    for (i=0;i<AUX_GNOFILE;i++) {
        fp=ui_gfiles[i];ui_gfiles[i]=0;
        if(fp)closef(fp);
    }
}

static void
ui_gfdrefresh(gp)
    struct guest_proc *gp;
{
    int i;
    file_t *fp,*old;

    if(ui.l_state!=LS_INUSE || gp!=ui.l_gp ||
       gp->gp_proc!=u.u_procp || ui.l_proc!=u.u_procp)return;
    ui_gfderr=0;
    for(i=0;i<AUX_GNOFILE;i++) {
        fp=0;
        if(getf(AUX_GFD+i,&fp)!=0)fp=0;
        old=ui_gfiles[i];
        if(fp==old)continue;
        if(fp && fp->f_count>=32767) {ui_gfderr=ENFILE;continue;}
        if(fp)fp->f_count++;
        ui_gfiles[i]=fp;
        if(old)closef(old);
    }
}

static void
ui_gfdrollback(made)
    unsigned char *made;
{
    int i;
    file_t *fp;

    for(i=0;i<AUX_GNOFILE;i++)if(made[i]) {
        if(getf(AUX_GFD+i,&fp)==0) {
            setf(AUX_GFD+i,0);setpof(AUX_GFD+i,0);closef(fp);
        }
        made[i]=0;
    }
}

static int
ui_gfdjoin(gp,made)
    struct guest_proc *gp;
    unsigned char *made;
{
    int i,fd,e;
    file_t *fp;

    bzero((caddr_t)made,AUX_GNOFILE);
    if(!gp || gp==ui.l_gp || gp->gp_proc!=u.u_procp ||
       ui.l_state!=LS_INUSE || !ui.l_proc || !ui.l_gp ||
       ui.l_gp->gp_proc!=ui.l_proc)return EINVAL;
    if(ui_gfderr)return ui_gfderr;
    for(i=0;i<AUX_GNOFILE;i++)if(ui_gfiles[i]) {
        if(getf(AUX_GFD+i,&fp)==0) {
            if(fp==ui_gfiles[i])continue;
            e=EBUSY;goto fail;
        }
        if(ui_gfiles[i]->f_count>=32767) {e=ENFILE;goto fail;}
        e=ufalloc(AUX_GFD+i,&fd);
        if(e)goto fail;
        if(fd!=AUX_GFD+i) {setf(fd,0);e=EBUSY;goto fail;}
        ui_gfiles[i]->f_count++;
        setf(fd,ui_gfiles[i]);setpof(fd,FCLOSEXEC);made[i]=1;
    }
    return 0;
fail:
    ui_gfdrollback(made);
    return e;
}

void
ui_note(kind, num, res)
	int kind, num, res;
{
	struct uicall *c = &ui_calls[(ui_ncalls - 1) & (UI_NCALL - 1)];

	if (ui_ncalls && c->c_kind == kind && c->c_num == (num & 0xff) && c->c_res == res) {
		c->c_n++;
		return;
	}
	c = &ui_calls[ui_ncalls++ & (UI_NCALL - 1)];
	c->c_kind = kind;
	c->c_num = num;
	c->c_res = res;
	c->c_n = 1;
}

/*
 * XPRAM $08-$1F after a PRAM reset: the last four bytes of the system
 * parameters (double-click and caret time $88, among them), 'NuMc',
 * then the first sixteen (valid $A8, serial ports, font, keyboard).
 * $76: default OS (Mac OS) and startup device.
 */
static unsigned char pram_sys[24] = {
	0x13, 0x88, 0x00, 0x4c, 'N', 'u', 'M', 'c',
	0xa8, 0x00, 0x00, 0x00, 0xcc, 0x0a, 0xcc, 0x0a,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x63, 0x00
};
static unsigned char pram_76[6] = { 0x00, 0x01, 0xff, 0xff, 0xff, 0xdf };
static int ui_tcmd, ui_te, ui_tpid;

/* the caller's A/UX state, 0 for a native process */
static struct guest_proc *
auxgp()
{
	struct guest_proc *gp = GUESTP(u.u_procp);

	return gp && gp->gp_prof == &aux_profile ? gp : 0;
}

/* ---- the tick: SIGIOT to the task each clock tick (UI_TIMER) ---- */

/* the tick goes to the running task, if it asked for ticks */
static void
uitick(arg)
	caddr_t arg;
{
	struct uitask *t;

	ui.l_tid = 0;
	if (ui.l_state != LS_INUSE || ui.l_active == 0)
		return;
	ui_evtick();
	for (t = ui.l_task; t < ui.l_task + UI_NTASK; t++)
		if ((t->t_gp == ui.l_active && t->t_tick) ||
		    (t->t_left > 0 && --t->t_left == 0)) {
			ui_nposted++;
			psignal(t->t_proc, SIGIOT);
		}
	wakeup((caddr_t)&ui.l_tid);
	if ((ui.l_tid = ttimeout(uitick, (caddr_t)0, 1L)) == -1)
		ui.l_tid = 0;
}

static void
uitickstop()
{
	int s = splhi_();

	if (ui.l_tid) {
		untimeout(ui.l_tid);
		ui.l_tid = 0;
	}
	splx_(s);
}

/* ---- XPRAM: read from its file as a session starts, written back as it ends ---- */

/* a Mac's PRAM after a reset */
static void
pramreset()
{
	bzero((caddr_t)ui.l_pram, sizeof ui.l_pram);
	bcopy((caddr_t)pram_sys, (caddr_t)ui.l_pram + 0x08, sizeof pram_sys);
	bcopy((caddr_t)pram_76, (caddr_t)ui.l_pram + 0x76, sizeof pram_76);
	/* 32-bit addressing: 24-bit mode needs the low 16 MB mirrored across the space */
	ui.l_pram[0x8a] = 5;
	bcopy((caddr_t)ui.l_pram, (caddr_t)ui_pramsaved, sizeof ui_pramsaved);
}

/* no file, or a short one: the reset contents */
static void
prampin()
{
	struct vnode *vp;
	unsigned char b[sizeof ui.l_pram];
	int resid = 0;

	pramreset();
	if (lookupname(uinter_pram, UIO_SYSSPACE, FOLLOW, NULLVPP, &vp))
		return;
	if (vp->v_type == VREG && VOP_ACCESS(vp, VREAD, 0, u.u_cred) == 0 && vn_rdwr(UIO_READ, vp, (caddr_t)b, (int)sizeof b, (off_t)0,
	    UIO_SYSSPACE, 0, 0x7fffffffL, u.u_cred, &resid) == 0 && resid == 0) {
		bcopy((caddr_t)b, (caddr_t)ui.l_pram, sizeof b);
		ui.l_pram[0x8a] |= 5;
		bcopy((caddr_t)ui.l_pram, (caddr_t)ui_pramsaved, sizeof ui_pramsaved);
	}
	VN_RELE(vp);
}

/* as root, whoever ends the session; only a regular file, not through a link */
static void
pramout()
{
	struct vnode *vp;
	struct cred *cr;
	int resid = 0, e;

	if (!ui_pramdirty)
		return;
	ui_pramdirty = 0;
	cr = crget();
	e = lookupname(uinter_pram, UIO_SYSSPACE, NO_FOLLOW, NULLVPP, &vp);
	if (e == 0) {
		if (vp->v_type != VREG)
			e = EINVAL;
		VN_RELE(vp);
	} else if (e == ENOENT)
		e = 0;
	if (e == 0 && (e = vn_open(uinter_pram, UIO_SYSSPACE, FWRITE | FCREAT | FTRUNC, 0644,
	    &vp, CRCREAT)) == 0) {
		if (vp->v_type != VREG)
			e = EINVAL;
		else
			e = vn_rdwr(UIO_WRITE, vp, (caddr_t)ui_pramsaved, (int)sizeof ui.l_pram,
			    (off_t)0, UIO_SYSSPACE, 0, 0x7fffffffL, cr, &resid);
		VOP_CLOSE(vp, FWRITE, 1, (off_t)0, cr);
		VN_RELE(vp);
	}
	crfree(cr);
	if (e || resid)
		printf("uinter: cannot write %s (%d)\n", uinter_pram, e);
}

/* Session memory must name the owning shared object in the current address space. */
int
ui_macrange(gp,a,n)
struct guest_proc *gp;
unsigned long a,n;
{
    struct seg *seg;struct segvn_data *sv;struct shmid_ds *sp;
    unsigned long off;int i;
    if(!gp || gp->gp_proc!=u.u_procp || ui.l_state!=LS_INUSE || !ui.l_proc ||
        !ui_task(gp) || a<0x3000UL || (a&1) || !n || n>0xffffffffUL-a)return 0;
    seg=as_segat(u.u_procp->p_as,(caddr_t)a);
    if(!seg || seg->s_ops!=&segvn_ops || !seg->s_data ||
        a<(unsigned long)seg->s_base || a-(unsigned long)seg->s_base>=seg->s_size ||
        n>seg->s_size-(a-(unsigned long)seg->s_base))return 0;
    sv=(struct segvn_data *)seg->s_data;
    if(sv->anon_index>btop(0xffffffffUL))return 0;
    off=ctob(sv->anon_index);
    if(off>0xffffffffUL-(a-(unsigned long)seg->s_base))return 0;
    off+=a-(unsigned long)seg->s_base;
    for(i=0;i<shminfo.shmmni;i++) {
        sp=shmem+i;
        if((sp->shm_perm.mode&IPC_ALLOC) && sp->shm_perm.key==0x744c4f57 &&
            sp->shm_cpid==ui.l_proc->p_pid && sp->shm_amp && sp->shm_amp==sv->amp &&
            off<(unsigned long)sp->shm_segsz && n<=(unsigned long)sp->shm_segsz-off)
            return valid_usr_range((caddr_t)a,n);
    }
    return 0;
}

/*
 * Mac RAM (shm key 'tLOW') that p made goes away with its last
 * attachment, so a killed startmac leaves nothing that blocks the
 * next one.  A live session's other attachers keep it until they go.
 */
static void
ui_rmram(p)
	struct proc *p;
{
	struct { int op, id, cmd; caddr_t buf; } a;
	struct shmid_ds *sp;
	long rv[2];
	int i;

	for (i = 0; i < shminfo.shmmni; i++) {
		sp = &shmem[i];
		if (!(sp->shm_perm.mode & IPC_ALLOC) || sp->shm_perm.key != 0x744c4f57 ||
		    sp->shm_cpid != p->p_pid)
			continue;
		a.op = 1;		/* shmctl */
		a.id = sp->shm_perm.seq * shminfo.shmmni + i;
		a.cmd = IPC_RMID;
		a.buf = 0;
		(void)shmsys(&a, rv);
	}
}

/* gp's slot in the layer, 0 if it is none of its tasks */
struct uitask *
ui_task(gp)
	struct guest_proc *gp;
{
	struct uitask *t;

	if (gp == 0 || ui.l_state != LS_INUSE)
		return 0;
	for (t = ui.l_task; t < ui.l_task + UI_NTASK; t++)
		if (t->t_gp == gp)
			return t;
	return 0;
}

/* Accepted joins share the owner's installed File Manager callbacks. */
int
ui_patchshared(gp)
	struct guest_proc *gp;
{
	struct uitask *t = ui_task(gp);

	return t && t != ui.l_task && ui.l_proc && ui.l_gp &&
	    ui.l_task[0].t_proc == ui.l_proc &&
	    ui.l_task[0].t_gp == ui.l_gp && ui.l_gp->gp_proc == ui.l_proc &&
	    t->t_proc == gp->gp_proc && t->t_proc == u.u_procp &&
	    (AUXP(gp)->ap_mac & APM_TASK);
}

/* the ui page for p: it lies in the shared Mac memory, so every task has it */
caddr_t
ui_uipof(p)
	struct proc *p;
{
	if (ui.l_uip == 0)
		return 0;
	return ui.l_uiproc == p || (GUESTP(p) && ui_task(GUESTP(p))) ? ui.l_uip : 0;
}

/* gp may run Mac code now */
#define	UIACT(gp)	(ui.l_state == LS_INUSE && ui.l_active == (gp))

/* the caller may share the session: same effective user, or root */
static int
uimay()
{
	struct cred *c = u.u_cred;

	return c->cr_uid == ui.l_proc->p_cred->cr_uid || suser(c);
}

/* Pending applications retain their process identity through attach and exec. */
struct uicoff {
    struct uicoff *c_next;
    struct proc *c_proc;
    struct guest_proc *c_gp;
    long c_pid;
    struct proc *c_owner;
    struct guest_proc *c_ownergp;
    long c_psn[2], c_a5;
    short c_token;
    short c_len;
    char c_name[128];
};
static struct uicoff *ui_coffs;
static int ui_ncoff;
static unsigned short ui_coffseq;
static int coffbind();

static void
coffdrop(gp,all)
struct guest_proc *gp;
int all;
{
    struct uicoff **link=&ui_coffs,*c;
    if(all) ui_coffseq=0;
    while((c=*link)!=0) {
        if(all || c->c_gp==gp) {
            *link=c->c_next; ui_ncoff--;
            if(prfind(c->c_pid)==c->c_proc && GUESTP(c->c_proc)==c->c_gp) {
                AUXP(c->c_gp)->ap_mac &= ~APM_COFF;
                if(all && c->c_proc!=ui.l_proc && !ui_task(c->c_gp))
                    psignal(c->c_proc,SIGKILL);
            }
            kmem_free((caddr_t)c,sizeof *c);
        } else link=&c->c_next;
    }
}

static int
coffname(gp,b,set)
struct guest_proc *gp;
char *b;
int set;
{
    struct uicoff *c;
    struct proc *p=u.u_procp;
    char name[128];
    long pid=G32(b);
    int n=(short)G16(b+8),i;
    if(ui.l_state!=LS_INUSE || !ui.l_proc) return EINVAL;
    if(u.u_cred->cr_uid!=ui.l_proc->p_cred->cr_uid && u.u_cred->cr_uid!=0)
        return EPERM;
    for(c=ui_coffs;c && c->c_pid!=pid;c=c->c_next);
    if(set) {
        if(pid!=p->p_pid || gp->gp_proc!=p) return EPERM;
        if(n<0 || n>=sizeof name) return EINVAL;
        if(!n) { coffdrop(gp,0); return 0; }
        if(copyin((caddr_t)G32(b+4),name,n)) return EFAULT;
        if(!name[n-1]) n--;
        if(!n) { coffdrop(gp,0); return 0; }
        for(i=0;i<n;i++) if(!name[i]) return EINVAL;
        name[n]=0;
        if(!c) {
            if(ui_ncoff>=UI_NTASK) return EAGAIN;
            c=(struct uicoff *)kmem_alloc(sizeof *c,KM_NOSLEEP);
            if(!c) return ENOMEM;
            c->c_owner=0;c->c_ownergp=0;c->c_token=0;
            c->c_psn[0]=c->c_psn[1]=c->c_a5=0;
            c->c_next=ui_coffs; ui_coffs=c; ui_ncoff++;
        } else if(c->c_proc!=p || c->c_gp!=gp) return ESRCH;
        c->c_proc=p; c->c_gp=gp; c->c_pid=pid; c->c_len=n;
        bcopy(name,c->c_name,n+1);
        AUXP(gp)->ap_mac |= APM_COFF;
        return 0;
    }
    if(!ui_task(gp)) return EPERM;
    if(!c || prfind(pid)!=c->c_proc || GUESTP(c->c_proc)!=c->c_gp)
        return ESRCH;
    if(c->c_proc!=p &&
       (gp!=ui.l_gp || (c->c_proc->p_parent!=p && !ui_task(c->c_gp))))
        return EPERM;
    if(c->c_proc->p_cred->cr_uid!=ui.l_proc->p_cred->cr_uid &&
       c->c_proc->p_cred->cr_uid!=0) return EPERM;
    if(n<0) return EINVAL;
    /* A zero-sized legacy request receives the complete C pathname. */
    if(!n) n=c->c_len+1;
    else if(n>c->c_len) n=c->c_len;
    bcopy(c->c_name,name,n);
    if(copyout(name,(caddr_t)G32(b+4),n)) return EFAULT;
    P16(b+8,n);
    return 0;
}

long uin_envdiag[8];

static int
coffenv(gp, b)
	struct guest_proc *gp;
	char *b;
{
	struct aux_proc *ap = AUXP(gp);
	unsigned char p[4];
	unsigned long a, v, bytes = 0;
	int i;
	unsigned char c;

	uin_envdiag[0] = u.u_procp->p_pid; uin_envdiag[1] = 1;
	uin_envdiag[6] = uin_envdiag[7] = 0;
	uin_envdiag[2] = ap->ap_envp; uin_envdiag[3] = ap->ap_envc;
	uin_envdiag[4] = ap->ap_envlo; uin_envdiag[5] = ap->ap_envhi;
	if (!UIACT(gp) || !ui_task(gp) || gp->gp_proc != u.u_procp)
		return EPERM;
	a = ap->ap_envp; uin_envdiag[1] = 2;
	if (!a || (a & 3) || ap->ap_envc < 0 || ap->ap_envc > 16384 ||
	    !ap->ap_envlo || (unsigned long)ap->ap_envhi <= (unsigned long)ap->ap_envlo ||
	    a < (unsigned long)ap->ap_envlo || a >= (unsigned long)ap->ap_envhi ||
	    (unsigned long)(ap->ap_envc + 1) * 4 > (unsigned long)ap->ap_envhi - a)
		return EINVAL;
	for (i = 0; i <= ap->ap_envc; i++) {
		uin_envdiag[1] = 3;
		if (copyin((caddr_t)(a + i * 4), (caddr_t)p, 4))
			return EFAULT;
		v = G32(p); uin_envdiag[1] = 4;
		uin_envdiag[6] = i; uin_envdiag[7] = v;
		if (i == ap->ap_envc) {
			if (v) return EINVAL;
			break;
		}
		if (!v) return EINVAL;
		uin_envdiag[1] = 6;
		do {
			if (bytes++ == 1048576) return EINVAL;
			if (!valid_usr_range((caddr_t)v, 1) ||
			    copyin((caddr_t)v, (caddr_t)&c, 1)) return EFAULT;
			if (v++ == 0xffffffffUL && c) return EINVAL;
		} while (c);
	}
	P32(b, a);
	P32(b + 4, ap->ap_envc);
	uin_envdiag[1] = 5;
	return 0;
}

/*
 * A task that joined leaves (exit, exec, kill, UI_KILLMYLAYER): if it
 * was running, the first task runs again.
 */
static void
uileave(t)
	struct uitask *t;
{
	struct guest_proc *gp = t->t_gp;

    coffdrop(gp,0);
	gp->gp_flags &= ~(GPF_ALINE | GPF_PRIV | GPF_SPIN | GPF_VPEND | GPF_FAULT | GPF_AWATCH);
	AUXP(gp)->ap_mac = 0;
	if (t->t_held && gp->gp_proc == u.u_procp)
		u.u_procp->p_hold = t->t_hold;
	t->t_proc = 0;
	t->t_gp = 0;
	t->t_force = 0;
	t->t_forceentry = 0;
	t->t_held = 0;
	t->t_tick = 0;
	t->t_left = 0;
	if (ui.l_active == gp) {
		ui.l_active = ui.l_gp;
		ui_forcewatch();
		wakeup((caddr_t)&ui.l_active);
	}
}

/* A carrier binds only its own admitted child to its public native world. */
static int
coffbind(gp,b)
struct guest_proc *gp;
char *b;
{
    struct uicoff *c;
    unsigned char low[4];
    long pid=G32(b),hi=G32(b+4),lo=G32(b+8),a5=G32(b+12);
    if(ui.l_state!=LS_INUSE || gp!=ui.l_gp || gp->gp_proc!=u.u_procp ||
        ui.l_proc!=u.u_procp || !ui_task(gp)) return EPERM;
    if((!hi && (unsigned long)lo<=2UL) || !ui_macrange(gp,(unsigned long)a5,4)) return EINVAL;
    if(copyin((caddr_t)a5,(caddr_t)low,4) ||
        copyin((caddr_t)0x904,(caddr_t)low,4)) return EFAULT;
    if(G32(low)!=a5) return EINVAL;
    for(c=ui_coffs;c && c->c_pid!=pid;c=c->c_next);
    if(!c || prfind(pid)!=c->c_proc || GUESTP(c->c_proc)!=c->c_gp)
        return ESRCH;
    if(c->c_proc->p_parent!=u.u_procp || c->c_gp->gp_proc!=c->c_proc ||
        !ui_task(c->c_gp) || !(AUXP(c->c_gp)->ap_mac & APM_COFF)) return EPERM;
    if(c->c_owner) {
        if(c->c_owner!=u.u_procp || c->c_ownergp!=gp || c->c_psn[0]!=hi ||
            c->c_psn[1]!=lo || c->c_a5!=a5) return EBUSY;
    } else {
        if(ui_coffseq>=32767) return EAGAIN;
        c->c_owner=u.u_procp;c->c_ownergp=gp;c->c_psn[0]=hi;c->c_psn[1]=lo;
        c->c_a5=a5;c->c_token=++ui_coffseq;
    }
    c->c_gp->gp_flags |= GPF_AWATCH;
    P32(b+16,c->c_token);
    return 0;
}

int
ui_coffworld(gp,b)
struct guest_proc *gp;
char *b;
{
    struct uicoff *c;
    if(!ui_patchshared(gp) || !(AUXP(gp)->ap_mac & APM_COFF)) return 0;
    for(c=ui_coffs;c && c->c_gp!=gp;c=c->c_next);
    if(!c || !c->c_owner || prfind(c->c_pid)!=c->c_proc ||
        GUESTP(c->c_proc)!=gp || gp->gp_proc!=c->c_proc ||
        c->c_owner!=ui.l_proc || c->c_ownergp!=ui.l_gp ||
        c->c_owner!=c->c_proc->p_parent || !ui_task(gp)) return 0;
    P32(b,c->c_token);P32(b+4,c->c_psn[0]);P32(b+8,c->c_psn[1]);P32(b+12,c->c_a5);
    return 1;
}

/*
 * Until t's process is the one to run Mac code.  Meanwhile every
 * signal it may hold waits (Mac handlers would run beside the other
 * task); its own mask comes back as it runs.  SIGKILL and SIGSTOP end
 * the wait with EINTR, the mask still held: the caller asks again.
 */
static int
uiwait(t)
	struct uitask *t;
{
	struct proc *p = u.u_procp;
	struct guest_proc *gp = t->t_gp;

	if (!t->t_held) {
		t->t_hold = p->p_hold;
		t->t_held = 1;
	}
	p->p_hold |= ~cantmask;
	while (ui.l_active != gp) {
		if (sleep((caddr_t)&ui.l_active, (PZERO + 1) | PCATCH))
			return EINTR;
		if (t->t_gp != gp)
			return EINTR;		/* the layer ended */
	}
	p->p_hold = t->t_hold;
	t->t_held = 0;
	return 0;
}

/*
 * The task or the ui page's holder leaves (exit, exec): the ui page is
 * free; for the task, no more ticks or reflection and the layer is free
 * for the next startmac.  Its mappings go with the address space.
 */
static void
uidetach(gp)
	struct guest_proc *gp;
{
	struct uitask *t;

    if(ui_stackspaceclear(gp))psignal(gp->gp_proc,SIGKILL);
    {
        int pe = ui_pmidleclear(gp);
        if(pe) {
            psignal(gp->gp_proc,SIGKILL);
            if(pe!=1)ui_pmidleforget(gp);
        }
    }
    if(ui_otrouteclear(gp)) {
        psignal(gp->gp_proc,SIGKILL);
        ui_otrouteforget(gp);
    }
    coffdrop(gp,gp==ui.l_gp);
	if (ui.l_uiproc == gp->gp_proc) {
		ui.l_uip = 0;
		ui.l_uiproc = 0;
	}
	AUXP(gp)->ap_mac &= ~APM_UIP;
	if ((t = ui_task(gp)) != 0 && t != ui.l_task) {
		uileave(t);
		return;
	}
	if (ui.l_gp != gp)
		return;
	/* the others run on this Mac memory: they go with it */
	for (t = ui.l_task + 1; t < ui.l_task + UI_NTASK; t++)
		if (t->t_proc) {
			psignal(t->t_proc, SIGKILL);
			uileave(t);
		}
	if (ui.l_task[0].t_held && gp->gp_proc == u.u_procp)
		u.u_procp->p_hold = ui.l_task[0].t_hold;
	ui.l_task[0].t_proc = 0;
	ui.l_task[0].t_gp = 0;
	ui.l_active = 0;
	wakeup((caddr_t)&ui.l_active);
	uitickstop();
	ui_unscreen(0);
	ui_inreset();
	gp->gp_flags &= ~(GPF_ALINE | GPF_PRIV | GPF_SPIN | GPF_VPEND | GPF_FAULT | GPF_AWATCH);
	AUXP(gp)->ap_mac = 0;
	pramout();
	ui_frontclear();
	ui_aslmclear(gp);
	ui_rmram(gp->gp_proc);
	ui_gfdclear();
	ui.l_state = LS_EMPTY;
	ui.l_proc = 0;
	ui.l_gp = 0;
	ui.l_shmid = -1;
	ui.l_romaddr = 0;
	ui.l_romproc = 0;
}

/* ---- entry points ---- */

int
uiopen(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	struct cred *cr;
{
	if (getminor(*devp) != 0)
		return ENXIO;
	ui_nopen++;
	return 0;
}

int
uiclose(dev, flag, otyp, cr)
	dev_t dev;
	int flag, otyp;
	struct cred *cr;
{
	ui_nopen = 0;
	return 0;
}

int
uirdwr(dev, uiop, cr)
	dev_t dev;
	struct uio *uiop;
	struct cred *cr;
{
	return EINVAL;
}

/* the value of an _IO command, or its argument in b (IN) */
static int
uiarg(cmd, arg, b)
	int cmd, arg;
	char *b;
{
	int n = UIOC_SIZE(cmd);

	if (!(cmd & UIOC_IN) || n == 0)
		return 0;
	return copyin((caddr_t)arg, b, n) ? EFAULT : 0;
}

/* ROM, ui page, low memory: startmac's part */
static int
uisetup(gp, cmd, arg, b, rvp)
	struct guest_proc *gp;
	int cmd, arg;
	char *b;
	int *rvp;
{
	struct proc *p = u.u_procp;
	static long lm030[2] = { 0x12f, 0xcb1 };
	char f[UI_UIPSIZE / 64];
	long a, n;
	int e, i;

	switch (UIOC_NUM(cmd)) {
	case 1:				/* UI_SET: Mac vectors on */
		if (!UIACT(gp))
			return EINVAL;
		gp->gp_vsr = 0x2000;
		gp->gp_vvbr = 0;
		gp->gp_flags |= GPF_ALINE | GPF_PRIV | GPF_SPIN;
		ui_forcereserve(0);
		ui_patcharm(gp);
		/* A/UX 2's Mac ran on 68030s only */
		if (AUXP(gp)->ap_flags & APF_AUX2)
			gp->gp_flags |= GPF_CPU030;
		return 0;
	case 2:				/* UI_CLEAR */
		if (!UIACT(gp))
			return EINVAL;
		if (ui_pmidleclose(gp))
			return EFAULT;
		gp->gp_flags &= ~(GPF_ALINE | GPF_PRIV | GPF_SPIN | GPF_VPEND | GPF_FAULT | GPF_AWATCH);
		gp->gp_vsr = 0;
		return 0;
	case 5:				/* UI_ROM(addr) */
		if (ui.l_gp != gp || ui.l_romaddr)
			return EINVAL;
		if ((e = ui_rommap((caddr_t)arg, AUXP(gp)->ap_flags & APF_AUX2)) != 0)
			return e;
		ui.l_romaddr = (caddr_t)arg;
		ui.l_romproc = p;
		return 0;
	case 6:				/* UI_UNROM */
		if (ui.l_romaddr == 0 || ui.l_romproc != p)
			return EINVAL;
		e = ui_romunmap(ui.l_romaddr);
		ui.l_romaddr = 0;
		ui.l_romproc = 0;
		return e;
	case 7:				/* UI_MAP(uaddr) */
		if (ui.l_uip || (arg & (UI_UIPSIZE - 1)) ||
		    !valid_usr_range((caddr_t)arg, UI_UIPSIZE))
			return EINVAL;
		/* struct ui_interface: positions 0, mouse button up */
		bzero(f, sizeof f);
		if (copyout(f, (caddr_t)arg, 16) || subyte((caddr_t)arg + 0x458, 0x80))
			return EFAULT;
		ui.l_uip = (caddr_t)arg;
		ui.l_uiproc = p;
		AUXP(gp)->ap_mac |= APM_UIP;
		return 0;
	case 8:				/* UI_UNMAP */
		if (ui.l_uiproc == p && ui.l_gp != gp) {
			ui.l_uip = 0;
			ui.l_uiproc = 0;
			AUXP(gp)->ap_mac &= ~APM_UIP;
		}
		return 0;
	case 21:			/* UI_CREATELAYER */
		if (ui.l_state != LS_EMPTY)
			return EEXIST;
		if (ui.l_uiproc != p)
			return EINVAL;
		ui.l_state = LS_INUSE;
		ui.l_proc = p;
		ui.l_gp = gp;
		bzero((caddr_t)ui.l_task, sizeof ui.l_task);
		ui.l_task[0].t_proc = p;
		ui.l_task[0].t_gp = gp;
		ui.l_active = gp;
		ui.l_task[0].t_force = 0;
		ui.l_task[0].t_forceentry = 0;
		wakeup((caddr_t)&ui.l_state);
		ui.l_shmid = -1;
		ui.l_evmask = 0xffef;
		ui.l_lapchk = 0;
		ui.l_sdchk = 0;
		ui.l_dmchk = 0;
		ui.l_fsqchk = 0;
		AUXP(gp)->ap_mac |= APM_TASK;
		ui_gfdrefresh(gp);
		prampin();
		*rvp = 0;
		return 0;
	case 32:			/* UI_COPY_OUT {start, count} */
		a = G32(b);
		n = G32(b + 4);
		if (a < 0 || n < 0 || a > UI_LOWSIZE || n > UI_LOWSIZE - a)
			return EINVAL;
		if ((e = ui_romload(AUXP(gp)->ap_flags & APF_AUX2)) != 0)
			return e;
		e = copyout((caddr_t)ui_rom.r_low + a, (caddr_t)a, n) ? EFAULT : 0;
		ui_romrel();
		/* CPUFlag and MMUType of the 68030 an A/UX 2 Mac sees */
		if (e == 0 && (AUXP(gp)->ap_flags & APF_AUX2))
			for (i = 0; i < 2; i++)
				if (a <= lm030[i] && lm030[i] < a + n && subyte((caddr_t)lm030[i], 3))
					e = EFAULT;
		return e;
	case 67:			/* UI_GET_PRODINFO(ptr) */
		if ((e = ui_romload(AUXP(gp)->ap_flags & APF_AUX2)) != 0)
			return e;
		if (ui_rom.r_prodoff < 0)
			e = EINVAL;
		else if (copyout((caddr_t)ui_rom.r_prod, (caddr_t)arg, UI_PRODSIZE))
			e = EFAULT;
		ui_romrel();
		return e;
	}
	return ENOTTY;
}

static void
uisyncto(arg)
	caddr_t arg;
{
	wakeup((caddr_t)&ui.l_state);
}

/*
 * UI_SYNC: wait up to 20 s for a session.  The joiner then attaches its
 * Mac memory, which the default soft RLIMIT_VMEM (16 MB) is too small
 * for: it gets the memory's size plus 16 MB, within its hard limit.
 */
static int
uisync()
{
	struct rlimit *lim = &u.u_rlimit[RLIMIT_VMEM];
	struct shmid_ds *sp;
	long end = lbolt + 20 * HZ;
	u_long want;
	int i, id;

	while (ui.l_state != LS_INUSE) {
		if ((long)(lbolt - end) >= 0 || (id = ttimeout(uisyncto, (caddr_t)0, (long)HZ)) == -1)
			return EAGAIN;
		i = sleep((caddr_t)&ui.l_state, (PZERO + 1) | PCATCH);
		untimeout(id);
		if (i)
			return EINTR;
	}
	if (!uimay())
		return 0;
	for (i = 0; i < shminfo.shmmni; i++) {
		sp = &shmem[i];
		if (!(sp->shm_perm.mode & IPC_ALLOC) || sp->shm_perm.key != 0x744c4f57 ||
		    sp->shm_cpid != ui.l_proc->p_pid)
			continue;
		want = sp->shm_segsz + 0x1000000;
		if (lim->rlim_cur < want)
			lim->rlim_cur = lim->rlim_max < want ? lim->rlim_max : want;
	}
	return 0;
}

/* root's Restart (fcn 1) or Shut Down (0): sync, then uadmin(A_SHUTDOWN, fcn) */
static int
uiadmin(fcn)
	int fcn;
{
	struct { int cmd, fcn, mdep; } a;
	int rv[2];

	if (u.u_cred->cr_ruid != 0 || !suser(u.u_cred))
		return EPERM;
	if (uinter_adtest) {
		uinter_adcall = 2 << 8 | fcn;
		psignal(u.u_procp, SIGKILL);
		return 0;
	}
	sync();
	a.cmd = 2;
	a.fcn = fcn;
	a.mdep = 0;
	return uadmin(&a, rv);
}

/*
 * Patch.067C's two transition queue walkers ask the LAP Manager for the
 * queue (selector 25) through $0B18 without checking it, as its own
 * dispatcher does.  Under a System without a LAP Manager ($0B18 <= 0)
 * root's Log Out and Shut Down then jump to $17.  Each walker's call
 * becomes a call to that dispatcher, with a1 first pointing at an empty
 * queue header (the .MPP driver header, whose long at +2 is 0), which
 * the dispatcher leaves when there is no LAP Manager.
 */
static unsigned char lapold[] = {
	0x70, 0x19, 0x4e, 0xb0, 0x01, 0xe2, 0x0b, 0x18, 0x00, 0x02
};
static struct lapsite {
	long	a;			/* the moveq #25,d0 */
	unsigned char new[10];		/* lea hdr(pc),a1; moveq; jsr disp(pc) */
} lapsite[] = {
	{ 0x381ee, { 0x43, 0xfa, 0xe2, 0x40, 0x70, 0x19, 0x4e, 0xba, 0xe2, 0x64 } },
	{ 0x38244, { 0x43, 0xfa, 0xe1, 0xea, 0x70, 0x19, 0x4e, 0xba, 0xe2, 0x0e } }
};
/* the dispatcher at $3645A and the header at $36430 */
static unsigned char lapdisp[] = {
	0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x4a, 0xb9, 0x00, 0x00, 0x0b, 0x18, 0x6f, 0x0a,
	0x4e, 0xf0, 0x01, 0xf2, 0x00, 0x00, 0x0b, 0x18,
	0x00, 0x02, 0x70, 0xff, 0x4e, 0x75
};

static void
ui_lapchk()
{
	unsigned char b[sizeof lapdisp];
	int i;

	if (ui.l_lapchk || !uinter_lapchk)
		return;
	ui.l_lapchk = 1;
	if (copyin((caddr_t)0x36430, b, 8) || bcmp(b, lapdisp, 8) ||
	    copyin((caddr_t)0x3645a, b, 22) || bcmp(b, lapdisp + 8, 22))
		return;
	for (i = 0; i < 2; i++)
		if (copyin((caddr_t)lapsite[i].a, b, 10) || bcmp(b, lapold, 10))
			return;
	for (i = 0; i < 2; i++)
		if (copyout(lapsite[i].new, (caddr_t)lapsite[i].a, 10))
			return;
	dlm_cacheflush();
}

/*
 * Patch.067C hooks the File Manager queue at the first _AUXDispatch after
 * boot and ends its hook in a jump through the FSQueueHook it found.
 * 7.x's File Manager patches leave a hook there; 8.1's (HFS Plus) are kept
 * out, so it finds A/UX's -1 (none) and Log Out's first _Close jumps to
 * -1.  Until it hooks in, a -1 FSQueueHook becomes an rts of its own; once
 * hooked, a -1 it kept does.
 */
#define	FSQRTS	0x13644			/* ExtFSHookRts */
#define	FSQSTATE 0x52f12		/* hookstate: 2 once hooked */
#define	FSQREAL	0x87d28			/* the hook it found */
static unsigned char fsqsig[] = {
	0x20, 0x39, 0x00, 0x05, 0x2f, 0x12,	/* cAUXDispatch: hookstate */
	0x4e, 0xf0, 0x01, 0xf1, 0x00, 0x08, 0x7d, 0x28	/* jmp ([realFSQueueHook]) */
};

/* once, with Patch.067C loaded */
static void
ui_fsqsig()
{
	unsigned char b[8];

	if (ui.l_fsqchk)
		return;
	ui.l_fsqchk = -1;
	if (copyin((caddr_t)0x40f8, b, 6) || bcmp(b, fsqsig, 6) ||
	    copyin((caddr_t)0x135da, b, 8) || bcmp(b, fsqsig + 6, 8) ||
	    copyin((caddr_t)FSQRTS, b, 2) || G16(b) != 0x4e75)
		return;
	ui.l_fsqchk = 1;
}

static void
ui_fsqchk()
{
	unsigned char b[4];
	long a;

	if (ui.l_fsqchk != 1 || copyin((caddr_t)FSQSTATE, b, 4))
		return;
	if (G32(b) >= 2) {
		ui.l_fsqchk = 2;
		a = FSQREAL;
	} else
		a = 0x3e2;
	if (copyin((caddr_t)a, b, 4) || G32(b) != 0xffffffff)
		return;
	P32(b, FSQRTS);
	(void)copyout(b, (caddr_t)a, 4);
}

/*
 * A user's Shut Down and Restart log out without A/UX's dialog, which
 * offers a Shut Down with root's password: in ShutDwnPower and
 * ShutDwnStart the call of shutDownDialog becomes moveq #0, its result
 * when the dialog chose Logout.  The patch follows the session's real
 * uid, which a login can change.
 */
static long sdsite[] = { 0xd16a, 0xd1d2 };
static unsigned char sdold[2][8] = {
	{ 0x42, 0xa7, 0x42, 0xa7, 0x61, 0x00, 0x03, 0x46 },
	{ 0x48, 0x78, 0x00, 0x01, 0x61, 0x00, 0x02, 0xde }
};
static unsigned char sdentry[] = { 0x2f, 0x0d, 0x2f, 0x02, 0x9e, 0xfc, 0x04, 0x08 };
static unsigned char sdnew[] = { 0x70, 0x00, 0x4e, 0x71 };

static void
ui_sdchk()
{
	unsigned char b[16];
	int i, n, user;

	if (ui.l_proc == 0)
		return;
	user = ui.l_proc->p_cred->cr_ruid != 0;
	if (ui.l_sdchk == user + 1)
		return;
	ui.l_sdchk = user + 1;
	if (copyin((caddr_t)0xd4b2, b, 8) || bcmp(b, sdentry, 8))
		return;
	for (i = 0; i < 2; i++)
		if (copyin((caddr_t)sdsite[i] - 4, b + 8 * i, 8) ||
		    bcmp(b + 8 * i, sdold[i], 4) ||
		    (bcmp(b + 8 * i + 4, sdold[i] + 4, 4) && bcmp(b + 8 * i + 4, sdnew, 4)))
			return;
	for (i = 0, n = 0; i < 2; i++)
		if (bcmp(b + 8 * i + 4, user ? sdnew : sdold[i] + 4, 4)) {
			if (copyout(user ? sdnew : sdold[i] + 4, (caddr_t)sdsite[i], 4))
				return;
			n = 1;
		}
	if (n)
		dlm_cacheflush();
}

/*
 * Patch.067C keeps the Trash and Temporary Items in $HOME/.mac/<host>
 * and makes both directories with mode 0777, less the umask.  Here both
 * mkdir calls get 0700, so other users cannot read them.
 */
static struct dmsite {
	long	a;			/* the pea #mode */
	unsigned char next[8];		/* what follows it */
} dmsite[] = {
	{ 0x7706, { 0x48, 0x6f, 0x00, 0x04, 0x61, 0xff, 0x00, 0x00 } },	/* host dir */
	{ 0xb37c, { 0x2f, 0x0b, 0x61, 0xff, 0x00, 0x03, 0x6c, 0x4c } }	/* .mac */
};
static unsigned char dmold[] = { 0x48, 0x78, 0x01, 0xff };
static unsigned char dmnew[] = { 0x48, 0x78, 0x01, 0xc0 };

static void
ui_dmchk()
{
	unsigned char b[12];
	int i;

	if (ui.l_dmchk)
		return;
	ui.l_dmchk = 1;
	for (i = 0; i < 2; i++)
		if (copyin((caddr_t)dmsite[i].a, b, 12) || bcmp(b + 4, dmsite[i].next, 8) ||
		    (bcmp(b, dmold, 4) && bcmp(b, dmnew, 4)))
			return;
	for (i = 0; i < 2; i++)
		if (copyout(dmnew, (caddr_t)dmsite[i].a, 4))
			return;
	dlm_cacheflush();
}

/*
 * UI_ATTACHLAYER: the caller, which has the Mac memory at 0, joins the
 * session: the ROM at the first task's address, room for the global
 * fds.  It returns its slot once it is to run.
 */
static int
uiattach(gp, rvp)
	struct guest_proc *gp;
	int *rvp;
{
	struct uitask *t;
	struct rlimit *lim;
	caddr_t rom = ui.l_romaddr ? ui.l_romaddr : (caddr_t)UI_ROMBASE;
	char w[4];
	unsigned char made[AUX_GNOFILE];
	rlim_t oldlimit;
	int e;

	if (ui.l_state != LS_INUSE || ui.l_uip == 0 || ui_task(gp))
		return EINVAL;
	if (!uimay())
		return EPERM;
	for (t = ui.l_task + 1; t < ui.l_task + UI_NTASK && t->t_proc; t++)
		;
	if (t == ui.l_task + UI_NTASK)
		return EAGAIN;
	if (copyin(ui.l_uip, w, sizeof w))
		return EFAULT;
    lim=&u.u_rlimit[RLIMIT_NOFILE];oldlimit=lim->rlim_cur;
    if(lim->rlim_cur<AUX_GFD+AUX_GNOFILE)
        lim->rlim_cur=lim->rlim_max<AUX_GFD+AUX_GNOFILE ?
            lim->rlim_max:AUX_GFD+AUX_GNOFILE;
    if((e=ui_gfdjoin(gp,made))!=0) {
        lim->rlim_cur=oldlimit;return e;
    }
    if ((e = ui_attachscreen(gp)) != 0) {
        ui_gfdrollback(made);lim->rlim_cur=oldlimit;return e;
    }
	if ((e = ui_rommap(rom, AUXP(gp)->ap_flags & APF_AUX2)) != 0) {
		ui_detachscreen(gp);
        ui_gfdrollback(made);lim->rlim_cur=oldlimit;
		return e;
	}
	t->t_proc = u.u_procp;
	t->t_gp = gp;
	t->t_force = 0;
	t->t_forceentry = 0;
	t->t_held = 0;
	t->t_tick = 0;
	t->t_left = 0;
	AUXP(gp)->ap_mac |= APM_TASK;
	lim = &u.u_rlimit[RLIMIT_NOFILE];
	if (lim->rlim_cur < AUX_GFD + AUX_GNOFILE)
		lim->rlim_cur = lim->rlim_max < AUX_GFD + AUX_GNOFILE ?
		    lim->rlim_max : AUX_GFD + AUX_GNOFILE;
	*rvp = t - ui.l_task;
	return uiwait(t);
}

/*
 * Everything else startmac and Patch.067C issue: events; no drives,
 * SCSI or VM; PRAM in memory.
 */
/* Session preferences stay volatile unless a privileged task writes them. */
static int
pramwrite(gp, addr, off, n)
	struct guest_proc *gp;
	long addr, off, n;
{
	unsigned char bytes[sizeof ui.l_pram];
	int root;

	if (off < 0 || n < 0 || off > sizeof bytes || n > sizeof bytes - off)
		return EINVAL;
	if (ui.l_state != LS_INUSE || !ui.l_proc || !ui_task(gp))
		return EPERM;
	root = u.u_cred->cr_uid == 0;
	if (!root && u.u_cred->cr_uid != ui.l_proc->p_cred->cr_uid)
		return EPERM;
	if (copyin((caddr_t)addr, (caddr_t)bytes, n)) return EFAULT;
	bcopy((caddr_t)bytes, (caddr_t)ui.l_pram + off, n);
	if (root && n) {
		bcopy((caddr_t)bytes, (caddr_t)ui_pramsaved + off, n);
		ui_pramdirty = 1;
	}
	return 0;
}

static int
uimisc(gp, cmd, arg, b, rvp)
	struct guest_proc *gp;
	int cmd, arg;
	char *b;
	int *rvp;
{
	struct rlimit *lim;
	struct uitask *t, *o;
	long off, n;
	int i;

	if (UIACT(gp))
		ui_fsqchk();
	switch (UIOC_NUM(cmd)) {
	case 4:				/* UI_UNSCREEN */
		if (ui.l_gp == gp)
			ui_unscreen(1);
		return ui.l_state == LS_INUSE ? 0 : EINVAL;
	case 9: case 10:		/* UI_CURSOR, UI_UNCURSOR */
		if (!UIACT(gp))
			return EINVAL;
		ui_inflags(IN_CUR, UIOC_NUM(cmd) == 9);
		return 0;
	case 24: case 25:		/* UI_DEVICES, UI_UNDEVICES */
		if (!UIACT(gp))
			return EINVAL;
		ui_inflags(IN_DEV, UIOC_NUM(cmd) == 24);
		return 0;
	case 16: case 30: case 55:	/* UI_POSTEVENT, UI_POST_MOD, UI_POST_EVTREC */
		if (!UIACT(gp))
			return EINVAL;
		return ui_postevent(b, UIOC_NUM(cmd) == 16 ? 0 : UIOC_NUM(cmd) == 30 ? 1 : 2);
	case 18:			/* UI_FLUSHEVENTS */
		if (!UIACT(gp))
			return EINVAL;
		ui_flushevents(b);
		return 0;
	case 23:			/* UI_SETLAYER */
	case 26:			/* UI_SETSELRECT */
	case 57: case 58: case 59: case 60: case 61:	/* VM holds and locks */
		return ui.l_state == LS_INUSE ? 0 : EINVAL;
	case 20:			/* UI_SETEVENTMASK */
		if (!UIACT(gp))
			return EINVAL;
		ui.l_evmask = G16(b);
		return 0;
	case 12:			/* UI_DELAY(Ticks to wait for): ticks since boot */
		off = G32(b);
		while (ui.l_tid && UIACT(gp) && (long)(lbolt - off) < 0)
			if (sleep((caddr_t)&ui.l_tid, (PZERO + 1) | PCATCH))
				return EINTR;
		P32(b, lbolt);
		return 0;
	case 19:			/* UI_GETOSEVENT */
		if (!UIACT(gp))
			return EINVAL;
		*rvp = 0;
		i = ui_getosevent(b);
		ui_forcepublish(gp);
		return i;
	case 28: case 29:		/* UI_READPRAM, UI_WRITEPRAM */
		off = G32(b + 4) & 0xffff;
		n = (G32(b + 4) >> 16) & 0xffff;
		if (off + n > sizeof ui.l_pram)
			return EINVAL;
		if (UIOC_NUM(cmd) == 28)
			return copyout((caddr_t)ui.l_pram + off, (caddr_t)G32(b), n) ?
			    EFAULT : 0;
		return pramwrite(gp, G32(b), off, n);
	case 34:			/* UI_PHYS_SCREENS */
		if (ui.l_state != LS_INUSE)
			return EINVAL;
		/* A/UX 2: the table follows the slot space's base and shift */
		if (UIOC_SIZE(cmd) == 6 + 48)
			b += 6;
		for (i = 0; i < 6; i++) {
			P8(b + 8 * i, 0xff);
			P8(b + 8 * i + 1, 0);
			P8(b + 8 * i + 2, 0);
			P8(b + 8 * i + 3, 0);
			P32(b + 8 * i + 4, 0);
		}
		return ui.l_gp == gp ? ui_screens(b) : 0;
	case 35:			/* UI_TIMER: one tick for the layer */
		if ((t = ui_task(gp)) == 0)
			return EINVAL;
		/* A/UX 2: one tick after n, 0 none; gives the ticks that were left */
		if (UIOC_SIZE(cmd) == 4 && (AUXP(gp)->ap_flags & APF_AUX2)) {
			n = G32(b);
			P32(b, t->t_left);
			t->t_left = n > 0 ? n : 0;
		} else
			t->t_tick = 1;
		ui_lapchk();
		ui_fsqsig();
		ui_sdchk();
		ui_dmchk();
		if (ui.l_tid == 0 && (ui.l_tid = ttimeout(uitick, (caddr_t)0, 1L)) == -1) {
			ui.l_tid = 0;
			return EAGAIN;
		}
		return 0;
	case 38:			/* UI_SWITCH(pid): pid's task runs, the caller waits */
		if ((t = ui_task(gp)) == 0)
			return EINVAL;
		if (!UIACT(gp))
			return uiwait(t);	/* only the running task hands over */
		/* -pid: pid runs and the caller, a joined task, is done */
		n = (long)G32(b) < 0 ? -(long)G32(b) : G32(b);
		if ((long)G32(b) < 0 && t == ui.l_task)
			return 0;
		for (o = ui.l_task; o < ui.l_task + UI_NTASK; o++)
			if (o->t_proc && o->t_proc->p_pid == n)
				break;
		if (o == ui.l_task + UI_NTASK && (long)G32(b) >= 0)
			return EINVAL;
		if (ui.l_uip && o < ui.l_task + UI_NTASK && suword(ui.l_uip + 0xff8, (int)n))
			return EFAULT;
		if ((long)G32(b) < 0) {
			uileave(t);
			if (o < ui.l_task + UI_NTASK && o != t && o->t_gp != ui.l_active) {
				ui.l_active = o->t_gp;
			ui_forcewatch();
				wakeup((caddr_t)&ui.l_active);
			}
			psignal(u.u_procp, SIGKILL);
			return 0;
		}
		if (o != t) {
			ui.l_active = o->t_gp;
			ui_forcewatch();
			wakeup((caddr_t)&ui.l_active);
		}
		return uiwait(t);
	case 39:			/* UI_SLEEP: until the caller runs */
		if ((t = ui_task(gp)) == 0)
			return EINVAL;
		return uiwait(t);
	case 36:			/* UI_ATTACHGFD: room for the global fds */
		if (ui_task(gp) == 0)
			return EINVAL;
		lim = &u.u_rlimit[RLIMIT_NOFILE];
		if (lim->rlim_cur < AUX_GFD + AUX_GNOFILE)
			lim->rlim_cur = lim->rlim_max < AUX_GFD + AUX_GNOFILE ?
			    lim->rlim_max : AUX_GFD + AUX_GNOFILE;
		return 0;
	case 42:			/* UI_GETDQEL: no drives, the end of the list */
		return EINVAL;
	case 46:			/* UI_SHMID */
		if (ui.l_gp != gp)
			return EINVAL;
		ui.l_shmid = G32(b);
		return 0;
	case 47: case 48:		/* UI_VIDEO_CONTROL, _STATUS */
		if (!UIACT(gp))
			return EINVAL;
		return ui_video(UIOC_NUM(cmd) == 47, b);
	case 50:			/* UI_SYNC: a session to join, waited for 20 s */
		return uisync();
	case 51:			/* UI_SET_KCHR */
		if (!UIACT(gp))
			return EINVAL;
		n = G32(b + 4);
		if (n < 0 || n > sizeof ui.l_kchr)
			return EINVAL;
		ui.l_kchrlen = 0;
		if (copyin((caddr_t)G32(b), (caddr_t)ui.l_kchr, n))
			return EFAULT;
		ui.l_kchrlen = n;
		return 0;
	case 52:			/* UI_TEST */
		return ui.l_state == LS_INUSE ? EEXIST : 0;
	case 56:			/* UI_GETKIFLAGS */
		P16(b, 0);
		return 0;
	case 62:			/* UI_VM_GETPHYS */
		return EINVAL;
	case 63:			/* UI_SETCOFFNAME {id, path, len} */
        return coffname(gp,b,1);
    case 0x95:
        if(UIOC_SIZE(cmd)!=8 || (cmd & (UIOC_IN|UIOC_OUT))!=UIOC_OUT)
            return EINVAL;
        return coffenv(gp,b);
    case 0x94:
        if(UIOC_SIZE(cmd)!=16 || (cmd & (UIOC_IN|UIOC_OUT))!=(UIOC_IN|UIOC_OUT))
            return EINVAL;
        return ui_otacceptdiag(gp,b);
    case 0x93:
        if(UIOC_SIZE(cmd)!=12 || (cmd & (UIOC_IN|UIOC_OUT))!=UIOC_IN)
            return EINVAL;
        return ui_otprobearm(gp,b);
    case 0x92:
        if(UIOC_SIZE(cmd)!=8 || (cmd & (UIOC_IN|UIOC_OUT))!=(UIOC_IN|UIOC_OUT))
            return EINVAL;
        return ui_otroutecontext(gp,b);
    case 0x91:
        if(UIOC_SIZE(cmd)!=20 || (cmd & (UIOC_IN|UIOC_OUT))!=UIOC_IN)
            return EINVAL;
        return ui_otroute(gp,b);
    case 0x90:
        if(UIOC_SIZE(cmd)!=20 || (cmd & (UIOC_IN|UIOC_OUT))!=(UIOC_IN|UIOC_OUT))
            return EINVAL;
        return coffbind(gp,b);
	case 64:			/* UI_GETCOFFNAME */
        return coffname(gp,b,0);
	case 68:			/* UI_GET_INTERR_VECTORS: none saved */
		*rvp = 1;
		return 0;
	case 69:			/* UI_GETKEYS */
		return UIACT(gp) ? ui_getkeys((caddr_t)arg) : EINVAL;
	case 70:			/* UI_GETSCSIID: none usable */
		{
			char s[0x1c];

			bzero(s, sizeof s);
			return copyout(s, (caddr_t)arg, sizeof s) ? EFAULT : 0;
		}
	case 27:			/* UI_HASKIDS */
		return ECHILD;
	case 43:			/* UI_KILLMYLAYER: a joined task only leaves */
		if (ui_task(gp))
			uidetach(gp);
		return 0;
	case 44: case 45:		/* UI_REBOOT, UI_SHUTDOWN */
		if (UIACT(gp) && ui_pmidleclose(gp))
			return EFAULT;
		return uiadmin(UIOC_NUM(cmd) == 45 ? 0 : 1);
	case 37:			/* UI_ATTACHLAYER {id, size, flags} */
		return uiattach(gp, rvp);
	case 40:			/* UI_SELECT: no select events */
		return ui_task(gp) ? 0 : EINVAL;
	}
	return EINVAL;	/* 11, 17, 22, 31, 33, 41, 53, 54, 65, 66, 71: not here */
}

int
uiioctl(dev, cmd, arg, mode, cr, rvp)
	dev_t dev;
	int cmd, arg, mode;
	struct cred *cr;
	int *rvp;
{
	struct guest_proc *gp = auxgp();
	char b[64];
	int e, n = UIOC_SIZE(cmd);

	*rvp = 0;
	if ((cmd & 0xff00) != UIOC_GROUP || n > sizeof b)
		return EINVAL;
	if (UIOC_NUM(cmd) == 0) {	/* UI_GETVERSION */
		/* A/UX 2's Toolbox wants the driver it shipped with */
		*rvp = gp && (AUXP(gp)->ap_flags & APF_AUX2) ? 4 : UI_VERSION;
		return 0;
	}
	if (gp == 0)
		return EINVAL;		/* the Mac side is an A/UX process */
	bzero(b, sizeof b);
	if ((e = uiarg(cmd, arg, b)) != 0)
		return e;
	e = uisetup(gp, cmd, arg, b, rvp);
	if (e == ENOTTY)
		e = uimisc(gp, cmd, arg, b, rvp);
	ui_note('Q', UIOC_NUM(cmd), -e);
	if (uinter_trace && (cmd != ui_tcmd || e != ui_te || u.u_procp->p_pid != ui_tpid)) {
		char t[8];

		ui_tcmd = cmd;		/* a polled call shows once */
		ui_te = e;
		ui_tpid = u.u_procp->p_pid;
		t[0] = 'Q';
		t[1] = '0' + UIOC_NUM(cmd) / 10;
		t[2] = '0' + UIOC_NUM(cmd) % 10;
		t[3] = 0;
		aux_tlog((int)u.u_procp->p_pid, t, (long)-e);
	}
	if (e == 0 && (cmd & UIOC_OUT) && n &&
	    copyout(b, (caddr_t)arg, n))
		e = EFAULT;
	return e;
}

/* ---- module ---- */

static int
uinter_load()
{
	/* Mac RAM (tLOW) is up to 32 MB plus 4 MB */
	if (shminfo.shmmax < 0x2400000)
		shminfo.shmmax = 0x2400000;
	aux_macdetach = uidetach;
	aux_macgfd = ui_gfdrefresh;
	aux_uitick = ui_update;
	aux_macpoll = ui_forcepoll;
	aux_slotmgr = ui_slotmgr;
	aux_alwatch = ui_alwatch;
	ui.l_shmid = -1;
	pramreset();
	return 0;
}

static int
uinter_unload()
{
	if (ui.l_state != LS_EMPTY || ui_nopen || ui_ncoff || ui_rombusy())
		return EBUSY;
	aux_macdetach = 0;
	aux_macgfd = 0;
	aux_uitick = 0;
	aux_macpoll = 0;
	aux_slotmgr = 0;
	aux_alwatch = 0;
	ui_romfree();
	return 0;
}

struct mod_drv_data uinter_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	  { uiopen, uiclose, uirdwr, uirdwr, uiioctl, nodev, nodev, nodev,
	    nodev, nodev, 0, 0, uiflag }, UI_MAJOR, 1 }
};

MOD_DRV_WRAPPER(uinter, uinter_load, uinter_unload, 0, "A/UX user interface");
