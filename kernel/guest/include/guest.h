/*
 * guest.h -- 68k guest processes.
 *
 * A guest process is an SVR4 process whose p_evpdp points to a
 * struct guest_proc.  Its profile (A/UX, TOS, Amiga) says what each
 * exception vector does for it; the static vector gates call
 * guest_trap for a guest process only.
 *
 *	vector gate --native--> stock handler
 *	     | guest
 *	guest_trap hook -> profile disposition -> gd_fn
 *	                    (declined: stock handler, frame untouched)
 *
 * Needs <sys/types.h>, <sys/proc.h>.  K&R C.
 */

#ifndef _GUEST_H
#define _GUEST_H

#define	GUEST_NVEC	256

/* dispositions */
#define	GD_NATIVE	0	/* host kernel handler */
#define	GD_REFLECT	1	/* CPU frame, continue at the guest vector */
#define	GD_EMULATE	2	/* privileged / unimplemented instruction */
#define	GD_SYSCALL	3	/* host call or the profile's translator */
#define	GD_HOSTCALL	4	/* $7300/$7301, thunk stubs */
#define	GD_FILTER	5	/* per function, while the vector is the sentinel */

/* gd_flags */
#define	GDF_USER	0x01	/* only from user mode */
#define	GDF_SENTINEL	0x02	/* gd_arg: sentinel index */

struct guest_regs;		/* the saved frame, see GR_* */

struct guest_disp {
	unsigned char	gd_kind;
	unsigned char	gd_flags;
	unsigned short	gd_arg;
	int		(*gd_fn)();	/* (gp, regs, vec): 0 handled,
					 * else the stock handler runs */
};

struct guest_profile {
	char			*gpf_name;
	struct modwrapper	*gpf_wrapper;	/* held per guest process */
	int			gpf_privsz;	/* profile state per process */
	struct guest_disp	gpf_disp[GUEST_NVEC];
	int			(*gpf_exec)();	/* (gp): new image */
	int			(*gpf_fork)();	/* (parent gp, child gp) */
	void			(*gpf_exit)();	/* (gp) */
	int			(*gpf_sendsig)(); /* (gp, sig, sip, hdlr) */
	int			(*gpf_vur)();	/* (gp, addr, len) */
	int			(*gpf_fsig)();	/* (p, gp): replaces the IPL hold */
	void			(*gpf_intr)();	/* (gp, regs): before the trap tail */
	struct guest_profile	*gpf_next;
};

struct guest_ctr;

struct guest_proc {
	struct proc		*gp_proc;
	struct guest_profile	*gp_prof;
	struct guest_ctr	*gp_ctr;	/* container, 0 = none */
	unsigned int		gp_flags;	/* GPF_* */
	unsigned int		gp_size;	/* bytes allocated */
	/* virtual CPU: the real SR keeps CCR and T */
	unsigned short		gp_vsr;		/* S, M, IPL */
	unsigned short		gp_vpend;	/* IPL bits of the highest pending interrupt */
	unsigned long		gp_vusp;	/* the inactive stack pointer */
	unsigned long		gp_vvbr;
	unsigned long		gp_vcacr;
	unsigned long		gp_vsfc, gp_vdfc;
	/* the last access fault, while GPF_FAULT */
	unsigned long		gp_fpc, gp_fea;
	unsigned long		gp_fpre;	/* SIGBUS/SIGSEGV pending before it */
	unsigned short		gp_fssw, gp_fpad;
};

/* gp_flags; the gates read the low 16 bits at offsets 14 and 15 */
#define	GPF_ALINE	0x01	/* A-line: frame to vVBR + $28 */
#define	GPF_PRIV	0x02	/* privileged instructions emulated */
#define	GPF_SPIN	0x04	/* vSR.S pinned on, one stack */
#define	GPF_VPEND	0x08	/* a signal waits for the virtual IPL */
#define	GPF_FAULT	0x10	/* gp_fpc.. hold an access fault */
#define	GPF_PRIVBAD	0x20	/* an unemulated instruction was reported */
#define	GPF_EXEC	0x40	/* gpf_exit from an exec of another profile */
#define	GPF_UNOTE	0x80	/* its fatal-fault notice was left out */
#define	GPF_FTRAP	0x100	/* traps #1-#15 but #13: frame to the guest vector in the gate */
#define	GPF_FTRAP13	0x200	/* trap #13 too */
#define	GPF_CPU030	0x400	/* sees a 68030: its bus fault frames, its MMU */
#define	GPF_PROFILE	0xffff0000	/* profile's own bits */
#define	GUEST_PRIV(gp)	((char *)((gp) + 1))	/* profile state */
#define	GUESTP(p)	((struct guest_proc *)(p)->p_evpdp)

/*
 * A container: one guest session's shared state (guest RAM, ROM copy,
 * vCPU page, register pages, virtual interrupts).
 */
struct guest_ctr {
	int			gc_ref;
	int			gc_shmid;	/* guest RAM */
	struct guest_romdesc	*gc_rom;
	char			*gc_romcopy;
	char			*gc_vcpu;	/* vCPU page, kernel alias */
	struct guest_rpage	*gc_rpage;
	int			gc_nrpage;
	unsigned long		gc_caps;	/* GCAP_* */
};

#define	GCAP_SYSCALLS	0x01	/* native trap #0 allowed */
#define	GCAP_RAWHW	0x02	/* passthrough grant */

/* vCPU page (one locked page per container) */
#define	VCPU_MAGIC	0x00	/* magic, version, profile id */
#define	VCPU_VSR	0x08	/* vsr, vipl_pending_max, in_pv_region */
#define	VCPU_VUSP	0x10	/* vusp, vssp, vvbr */
#define	VCPU_TICKS	0x20	/* catch-up counters per timer channel */
#define	VCPU_PEND	0x40	/* pending bitmap per level, vectors, pic state */
#define	VCPU_INPUT	0x100	/* input event ring */
#define	VCPU_ASYNC	0x800	/* async completion ring */

/*
 * The frame a gate saves: usp, d0-d7, a0-a6, then the CPU's
 * exception frame.  Same layout as the stock trap path's pcb.
 */
#define	GR_USP(r)	(*(long *)((char *)(r) + 0))
#define	GR_D(r, n)	(*(long *)((char *)(r) + 4 + 4 * (n)))
#define	GR_A(r, n)	(*(long *)((char *)(r) + 36 + 4 * (n)))
#define	GR_SR(r)	(*(unsigned short *)((char *)(r) + 64))
#define	GR_PC(r)	(*(long *)((char *)(r) + 66))
#define	GR_FV(r)	(*(unsigned short *)((char *)(r) + 70))
#define	GR_VEC(r)	((GR_FV(r) & 0xfff) >> 2)

/* static shims (always in the kernel) */
extern struct proc *guest_loading;	/* exec that makes a guest */
extern struct guest_profile *guest_loadprof;
extern int (*guest_trap_hook)();	/* (regs) */
extern void (*guest_exec_hook)();	/* (p) */
extern int (*guest_fork_hook)();	/* (pp, cp) */
extern void (*guest_exit_hook)();	/* (p, stat) */
extern int (*guest_sendsig_hook)();	/* (sig, sip, hdlr) */
extern int (*guest_vur_hook)();		/* (addr, len) */
extern int (*guest_fsig_hook)();	/* (p) */
extern int __amix_sendsig();
extern int __amix_valid_usr_range();
extern int __amix_fsig();
extern long guest_nlinea;		/* A-line traps of guests */
extern long guest_lineapc;		/* PC of the last one */
extern long guest_nftrap, guest_nfpriv;	/* gate fast paths taken */

/* guestcore */
extern int guest_profile_add();		/* (pf) */
extern void guest_profile_del();	/* (pf) */
extern int guest_aline();		/* disposition: A-line reflection */
extern int guest_priv();		/* disposition: privilege emulation */
extern int guest_fline();		/* the same for 68030 MMU instructions in line F */
extern int guest_fnote();		/* disposition: note an access fault */
extern int guest_detach();		/* (pf): curproc leaves its guest */
extern int guest_attach();		/* (pf): curproc becomes a guest */
extern int guest_getsr();		/* (gp, regs): the SR the guest sees */
extern void guest_setsr();		/* (gp, regs, sr): switches stacks */
extern void guest_trapret();		/* reschedule, deliverable signals */
extern int guest_reflect();		/* (gp, regs, pc, fv, x, n, ipl) */

#endif	/* _GUEST_H */
