| guest_gate.s -- common part of the vector gates.
|
| A gate (generated per vector) sends a native process straight to the
| vector's stock handler and a guest process here:
|
|   save d0-a6 and usp as the stock trap path does, guest_trap(frame)
|   0: handled -> ureturn (queues, class trap return, signals, rte)
|   else: restore everything and jump to guest_chain[vector] with the
|         CPU frame untouched

	.text
	.globl	gc_gate_c
gc_gate_c:
	moveml	&0xfffe,%sp@-		| d0-d7/a0-a6
	movel	sup_cacr,%d0
	movec	%d0,%cacr
	movel	%usp,%a0
	movel	%a0,%sp@-
	pea	%sp@
	jsr	guest_trap
	addql	&4,%sp
	moveal	%sp@+,%a0
	movel	%a0,%usp
	tstl	%d0
	bnew	Ldecline
	jmp	ureturn
Ldecline:
	movew	%sp@(66),%d0		| format/vector
	andiw	&0x0ffc,%d0
	lea	guest_chain,%a0
	movel	%a0@(0,%d0:w),%sp@-	| stock handler
	moveml	%sp@(4),&0x7fff		| d0-a6 back
	movel	%sp@,%sp@(60)		| handler over the saved a6
	lea	%sp@(60),%sp
	rts

| gc_linea -- A-line of a guest with GPF_ALINE, from the vector-10
| gate: the 8-byte format-0 frame the guest's dispatcher expects goes
| on the user stack and the guest continues at the long at vVBR + $28.
|
|   user frame:  SR | vSR   PC (the A-line word)   $0028
|   then:        real SR = CCR only, vSR = S and IPL
|
| Pending signals, a reschedule or a fault on the user stack take the
| C path (gc_gate_c), which does the same and leaves through ureturn.
| A fault in a moves is resolved by the page-fault path or lands at
| Llafault with this stack (the u+0x374 pad, as the user-access
| primitives use it).

GP_FLAGS = 12
GP_VSR = 20
GP_VVBR = 28
GP_MMU = 68
GP_SPACE = 72
P_EVPDP = 0xc8
P_SIG = 0x9c
P_HOLD = 0xa4

	.globl	gc_linea
gc_linea:
	moveml	&0xc0c0,%sp@-		| d0-d1/a0-a1; frame at 16(sp)
	moveal	curproc,%a1
	movel	%a1@(P_HOLD),%d0
	notl	%d0
	andl	%a1@(P_SIG),%d0
	bnew	Llaslow
	tstl	runrun
	bnew	Llaslow
	moveal	%a1@(P_EVPDP),%a1
	btst	&6,%a1@(GP_FLAGS+2)
	bnew	Llaslow
	moveq	&1,%d0
	movec	%d0,%sfc
	movec	%d0,%dfc
	movel	&Llafault,u+0x374
	moveal	%a1@(GP_VVBR),%a0
	movesl	%a0@(0x28),%d1		| the guest's handler
	movel	%usp,%a0
	subql	&8,%a0
	movew	%sp@(16),%d0
	orw	%a1@(GP_VSR),%d0
	movesw	%d0,%a0@
	movel	%sp@(18),%d0
	movesl	%d0,%a0@(2)
	movew	&0x0028,%d0
	movesw	%d0,%a0@(6)
	clrl	u+0x374
	addql	&1,guest_nlinea
	movel	%sp@(18),guest_lineapc
	movel	%a0,%usp
	movel	%d1,%sp@(18)
	andiw	&0x00ff,%sp@(16)
	andiw	&0x2700,%a1@(GP_VSR)
	moveml	%sp@+,&0x0303
	rte
Llafault:
	clrl	u+0x374
Llaslow:
	moveml	%sp@+,&0x0303
	jmp	gc_gate_c

| gc_ftrap -- trap #n of a guest with GPF_FTRAP: the CPU's format-0
| frame goes on the guest's supervisor stack, as guest_reflect builds it;
| under a shadow MMU, in the supervisor's space.
|
|   guest frame:  SR | vSR   PC   format/vector (none for a 68000)
|   then:         real SR = CCR, vSR = S, M and IPL kept
|
| Pending signals, a reschedule, another format or a fault take the C path.

GP_VPEND = 22
GP_VUSP = 24
GP_VSFC = 36
GP_VDFC = 40
GPF_SPIN_BIT = 2
GPF_VPEND_BIT = 3

	.globl	gc_ftrap
gc_ftrap:
	moveml	&0xe0f0,%sp@-		| d0-d2/a0-a3; frame at 28(sp)
	moveal	curproc,%a1
	movel	%a1@(P_HOLD),%d0
	notl	%d0
	andl	%a1@(P_SIG),%d0
	bnew	Lftslow
	tstl	runrun
	bnew	Lftslow
	movew	%sp@(34),%d0
	cmpiw	&0x0fff,%d0		| format 0
	bhiw	Lftslow
	moveal	%a1@(P_EVPDP),%a1
	moveq	&1,%d1
	movec	%d1,%sfc
	movec	%d1,%dfc
	movew	%a1@(GP_VSR),%d2
	movel	%usp,%a2
	moveal	%a2,%a0
	btst	&13,%d2			| vS: on the active stack
	bnew	Lft1
	btst	&GPF_SPIN_BIT,%a1@(GP_FLAGS+3)
	bnew	Lft1
	tstl	%a1@(GP_SPACE)
	bnew	Lftslow			| S changes: the C path switches spaces
	moveal	%a1@(GP_VUSP),%a0
	tstl	%a1@(GP_MMU)
	beqw	Lft1
	bset	&5,%a1@(GP_VSR)		| vS early: a fill below picks the supervisor's space too
	moveml	&0xc0c0,%sp@-		| the supervisor's space holds the table and the stack
	movel	&0x2000,%sp@-
	movel	%a1,%sp@-
	jsr	guest_mmu_space
	addql	&8,%sp
	moveml	%sp@+,&0x0303
Lft1:
	movel	&Lftfault,u+0x374
	moveal	%a1@(GP_VVBR),%a3
	movesl	%a3@(0,%d0:w),%d1	| the guest's handler
	btst	&0,%a1@(GP_FLAGS+1)	| GPF_FRAME68K: SR and PC only
	beqw	Lft2
	subql	&6,%a0
	bra	Lft5
Lft2:
	subql	&8,%a0
	movesw	%d0,%a0@(6)
Lft5:
	movew	%sp@(28),%d0
	andiw	&0x80ff,%d0
	andiw	&0x3700,%d2
	orw	%d2,%d0
	movesw	%d0,%a0@
	movel	%sp@(30),%d0
	movesl	%d0,%a0@(2)
	clrl	u+0x374
	btst	&13,%d2			| from vUSP: it gets the user sp
	bnew	Lft3
	btst	&GPF_SPIN_BIT,%a1@(GP_FLAGS+3)
	bnew	Lft3
	movel	%a2,%a1@(GP_VUSP)
Lft3:
	movel	%a0,%usp
	andiw	&0x1700,%d2
	oriw	&0x2000,%d2
	movew	%d2,%a1@(GP_VSR)
	andiw	&0x0700,%d2
	bnew	Lft4
	bclr	&GPF_VPEND_BIT,%a1@(GP_FLAGS+3)
Lft4:
	andiw	&0x7fff,%sp@(28)	| T1 off
	movel	%d1,%sp@(30)
	addql	&1,guest_nftrap
	moveml	%sp@+,&0x0f07
	rte
Lftfault:
	clrl	u+0x374
	btst	&13,%d2			| the user's space back if it changed
	bnew	Lftslow
	btst	&GPF_SPIN_BIT,%a1@(GP_FLAGS+3)
	bnew	Lftslow
	tstl	%a1@(GP_MMU)
	beqw	Lftslow
	bclr	&5,%a1@(GP_VSR)
	clrl	%sp@-
	movel	%a1,%sp@-
	jsr	guest_mmu_space
	addql	&8,%sp
Lftslow:
	moveml	%sp@+,&0x0f07
	jmp	gc_gate_c

| gc_fpriv -- vector 8 of a guest with GPF_PRIV in virtual supervisor
| mode: rte (format 0, or a 68000's SR and PC), move to and from SR (Dn, (sp)+, -(sp), #imm)
| and ori/andi/eori #,SR against gp_vsr, as gcpu.c emulates them,
| move to and from USP against gp_vusp, movec of SFC, DFC and USP, and on
| a 68040 cpusha bc and fsave/frestore at (An), d16(An) or abs.l; a
| 68000's move from SR to Dn or -(sp) in user mode too.  An
| IPL below gp_vpend (an interrupt would be taken), pending signals, a
| reschedule, other instructions or a fault take the C path.

	.globl	gc_fpriv
gc_fpriv:
	moveml	&0xffe0,%sp@-		| d0-d7/a0-a2; frame at 44(sp)
	moveal	curproc,%a1
	movel	%a1@(P_HOLD),%d0
	notl	%d0
	andl	%a1@(P_SIG),%d0
	bnew	Lfpslow
	tstl	runrun
	bnew	Lfpslow
	moveal	%a1@(P_EVPDP),%a1
	movew	%a1@(GP_VSR),%d6
	btst	&13,%d6
	bnew	Lfpsv
	btst	&0,%a1@(GP_FLAGS+1)	| GPF_FRAME68K: a 68000 reads SR in user mode too
	beqw	Lfpslow
Lfpsv:
	moveq	&1,%d1
	movec	%d1,%sfc
	movec	%d1,%dfc
	movel	&Lfpfault,u+0x374
	moveal	%sp@(46),%a0
	movesw	%a0@,%d0
	movel	%usp,%a2
	movew	%sp@(44),%d7		| the SR the guest sees
	andiw	&0x80ff,%d7
	movew	%d6,%d1
	andiw	&0x3700,%d1
	orw	%d1,%d7
	btst	&13,%d6
	bnew	Lfpsv1
	cmpiw	&0x40e7,%d0
	beqw	Lfppush
	movew	%d0,%d1
	andiw	&0xfff8,%d1
	cmpiw	&0x40c0,%d1
	beqw	Lfptod
	bra	Lfpfault
Lfpsv1:
	cmpiw	&0x4e73,%d0
	beqw	Lfprte
	cmpiw	&0x46fc,%d0
	beqw	Lfpimm
	cmpiw	&0x46df,%d0
	beqw	Lfppop
	cmpiw	&0x40e7,%d0
	beqw	Lfppush
	movew	%d0,%d1
	andiw	&0xfff8,%d1
	cmpiw	&0x46c0,%d1
	beqw	Lfpfromd
	cmpiw	&0x40c0,%d1
	beqw	Lfptod
	cmpiw	&0x007c,%d0
	beqw	Lfpori
	cmpiw	&0x027c,%d0
	beqw	Lfpandi
	cmpiw	&0x0a7c,%d0
	beqw	Lfpeori
	movew	%d0,%d1
	andiw	&0xfff0,%d1
	cmpiw	&0x4e60,%d1
	beqw	Lfpusp
	movew	%d0,%d1
	andiw	&0xfffe,%d1
	cmpiw	&0x4e7a,%d1
	beqw	Lfpmovec
	movew	%d0,%d1
	andiw	&0xff80,%d1
	cmpiw	&0xf300,%d1
	beqw	Lfpfpu
	cmpiw	&0xf4f8,%d0
	beqw	Lfpcpush
	bra	Lfpfault
Lfpcpush:				| cpusha bc
	btst	&1,gc_fast+3
	beqw	Lfpfault
	.short	0xf4f8
	addql	&2,%a0
	bra	Lfpdone
Lfpfpu:					| fsave, frestore at (An), d16(An), abs.l: d2 the address
	btst	&0,gc_fast+3
	beqw	Lfpfault
	moveq	&0,%d4
	movew	%d0,%d1
	andiw	&0x0038,%d1
	cmpiw	&0x0010,%d1
	beqw	Lfpf2
	cmpiw	&0x0028,%d1
	beqw	Lfpf5
	movew	%d0,%d1
	andiw	&0x003f,%d1
	cmpiw	&0x0039,%d1
	bnew	Lfpfault
	movesl	%a0@(2),%d2
	addql	&6,%a0
	bra	Lfpf9
Lfpf5:
	movesw	%a0@(2),%d4
	extl	%d4
	addql	&2,%a0
Lfpf2:
	addql	&2,%a0
	movew	%d0,%d1
	andiw	&7,%d1
	cmpiw	&3,%d1
	bcsw	Lfpa1
	beqw	Lfpa3
	cmpiw	&5,%d1
	bcsw	Lfpa4
	beqw	Lfpa5
	cmpiw	&7,%d1
	bcsw	Lfpa6
	movel	%a2,%d2
	bra	Lfpa9
Lfpa1:
	lslw	&2,%d1
	movel	%sp@(32,%d1:w),%d2
	bra	Lfpa9
Lfpa3:
	movel	%a3,%d2
	bra	Lfpa9
Lfpa4:
	movel	%a4,%d2
	bra	Lfpa9
Lfpa5:
	movel	%a5,%d2
	bra	Lfpa9
Lfpa6:
	movel	%a6,%d2
Lfpa9:
	addl	%d4,%d2
Lfpf9:
	movel	%a0,%d5			| the next pc
	moveal	%d2,%a1
	moveq	&4,%d3			| d3: the frame's size
	btst	&6,%d0
	bnew	Lfrs
	lea	%sp@(-112),%sp
	.short	0xf317			| fsave (sp)
	moveq	&0,%d1
	moveb	%sp@,%d1
	beqw	Lfsv1
	cmpib	&0x41,%d1
	bnew	Lfsvf
	moveb	%sp@(1),%d1
	beqw	Lfsv1
	cmpib	&0x30,%d1
	beqw	Lfsv0
	cmpib	&0x60,%d1
	bnew	Lfsvf
Lfsv0:
	addl	%d1,%d3
Lfsv1:
	movel	&Lfsvf,u+0x374
	moveal	%sp,%a0
	movel	%d3,%d1
	lsrl	&2,%d1
	subql	&1,%d1
Lfsv2:
	movel	%a0@+,%d4
	movesl	%d4,%a1@+
	dbf	%d1,Lfsv2
	cmpil	&4,%d3
	beqw	Lfsv3
	movel	&0x41000000,%sp@	| idle: a switch still saves fp0-fp7
	.short	0xf357			| frestore (sp)
Lfsv3:
	lea	%sp@(112),%sp
	moveal	%d5,%a0
	bra	Lfpdone
Lfsvf:					| not stored: the state goes back
	.short	0xf357			| frestore (sp)
	lea	%sp@(112),%sp
	bra	Lfpfault
Lfrs:
	movesl	%a1@,%d1		| the header
	movel	%d1,%d4
	clrw	%d4
	swap	%d4
	movew	%d4,%d1
	andiw	&0xff00,%d1
	beqw	Lfrs1
	cmpiw	&0x4100,%d1
	bnew	Lfpfault
	andiw	&0x00ff,%d4
	beqw	Lfrs1
	cmpiw	&0x0030,%d4
	beqw	Lfrs0
	cmpiw	&0x0060,%d4
	bnew	Lfpfault
Lfrs0:
	addl	%d4,%d3
Lfrs1:
	lea	%sp@(-112),%sp
	movel	&Lfrsf,u+0x374
	moveal	%sp,%a0
	movel	%d3,%d1
	lsrl	&2,%d1
	subql	&1,%d1
Lfrs2:
	movesl	%a1@+,%d4
	movel	%d4,%a0@+
	dbf	%d1,Lfrs2
	.short	0xf357			| frestore (sp)
	lea	%sp@(112),%sp
	moveal	%d5,%a0
	bra	Lfpdone
Lfrsf:
	lea	%sp@(112),%sp
	bra	Lfpfault
Lfpmovec:				| movec of SFC, DFC or USP: d3 the field
	movesw	%a0@(2),%d2
	movew	%d2,%d1
	andiw	&0x0fff,%d1
	moveq	&GP_VSFC,%d3
	tstw	%d1
	beqw	Lfpmc1
	moveq	&GP_VDFC,%d3
	cmpiw	&1,%d1
	beqw	Lfpmc1
	moveq	&GP_VUSP,%d3
	cmpiw	&0x800,%d1
	bnew	Lfpfault
Lfpmc1:
	addql	&4,%a0
	movew	%d2,%d1			| register 0-15 (d0-d7, a0-a7)
	lsrw	&8,%d1
	lsrw	&4,%d1
	btst	&0,%d0
	bnew	Lfpmcw
	movel	%a1@(0,%d3:w),%d4	| to the register
	cmpiw	&11,%d1
	bcsw	Lfpmr0
	beqw	Lfpmr3
	cmpiw	&13,%d1
	bcsw	Lfpmr4
	beqw	Lfpmr5
	cmpiw	&15,%d1
	bcsw	Lfpmr6
	moveal	%d4,%a2
	bra	Lfpdone
Lfpmr0:
	lslw	&2,%d1
	movel	%d4,%sp@(0,%d1:w)
	bra	Lfpdone
Lfpmr3:
	moveal	%d4,%a3
	bra	Lfpdone
Lfpmr4:
	moveal	%d4,%a4
	bra	Lfpdone
Lfpmr5:
	moveal	%d4,%a5
	bra	Lfpdone
Lfpmr6:
	moveal	%d4,%a6
	bra	Lfpdone
Lfpmcw:					| from the register
	cmpiw	&11,%d1
	bcsw	Lfpmw0
	beqw	Lfpmw3
	cmpiw	&13,%d1
	bcsw	Lfpmw4
	beqw	Lfpmw5
	cmpiw	&15,%d1
	bcsw	Lfpmw6
	movel	%a2,%d4
	bra	Lfpmw9
Lfpmw0:
	lslw	&2,%d1
	movel	%sp@(0,%d1:w),%d4
	bra	Lfpmw9
Lfpmw3:
	movel	%a3,%d4
	bra	Lfpmw9
Lfpmw4:
	movel	%a4,%d4
	bra	Lfpmw9
Lfpmw5:
	movel	%a5,%d4
	bra	Lfpmw9
Lfpmw6:
	movel	%a6,%d4
Lfpmw9:
	movel	%d4,%a1@(0,%d3:w)
	bra	Lfpdone
Lfpusp:					| a1: the guest; register n in d1
	movew	%d0,%d1
	andiw	&7,%d1
	addql	&2,%a0
	btst	&3,%d0
	bnew	Lfpfrusp
	cmpiw	&3,%d1			| move An,usp
	bcsw	Lfpus1
	beqw	Lfpus3
	cmpiw	&5,%d1
	bcsw	Lfpus4
	beqw	Lfpus5
	cmpiw	&7,%d1
	bcsw	Lfpus6
	movel	%a2,%d2
	bra	Lfpus9
Lfpus1:
	lslw	&2,%d1
	movel	%sp@(32,%d1:w),%d2
	bra	Lfpus9
Lfpus3:
	movel	%a3,%d2
	bra	Lfpus9
Lfpus4:
	movel	%a4,%d2
	bra	Lfpus9
Lfpus5:
	movel	%a5,%d2
	bra	Lfpus9
Lfpus6:
	movel	%a6,%d2
Lfpus9:
	movel	%d2,%a1@(GP_VUSP)
	bra	Lfpdone
Lfpfrusp:				| move usp,An
	movel	%a1@(GP_VUSP),%d2
	cmpiw	&3,%d1
	bcsw	Lfpur1
	beqw	Lfpur3
	cmpiw	&5,%d1
	bcsw	Lfpur4
	beqw	Lfpur5
	cmpiw	&7,%d1
	bcsw	Lfpur6
	moveal	%d2,%a2
	bra	Lfpdone
Lfpur1:
	lslw	&2,%d1
	movel	%d2,%sp@(32,%d1:w)
	bra	Lfpdone
Lfpur3:
	moveal	%d2,%a3
	bra	Lfpdone
Lfpur4:
	moveal	%d2,%a4
	bra	Lfpdone
Lfpur5:
	moveal	%d2,%a5
	bra	Lfpdone
Lfpur6:
	moveal	%d2,%a6
	bra	Lfpdone
Lfprte:
	btst	&0,%a1@(GP_FLAGS+1)	| bit 16, GPF_FRAME68K: SR and PC only
	beqw	Lfprte4
	movesw	%a2@,%d0
	movesl	%a2@(2),%a0
	addql	&6,%a2
	bra	Lfpset
Lfprte4:
	movesw	%a2@(6),%d1
	cmpiw	&0x0fff,%d1
	bhiw	Lfpfault		| format 0 only
	movesw	%a2@,%d0
	movesl	%a2@(2),%a0
	addql	&8,%a2
	bra	Lfpset
Lfpimm:
	movesw	%a0@(2),%d0
	addql	&4,%a0
	bra	Lfpset
Lfppop:
	movesw	%a2@+,%d0
	addql	&2,%a0
	bra	Lfpset
Lfpfromd:
	andiw	&7,%d0
	lslw	&2,%d0
	movew	%sp@(2,%d0:w),%d0
	addql	&2,%a0
	bra	Lfpset
Lfpori:
	movesw	%a0@(2),%d0
	orw	%d7,%d0
	addql	&4,%a0
	bra	Lfpset
Lfpandi:
	movesw	%a0@(2),%d0
	andw	%d7,%d0
	addql	&4,%a0
	bra	Lfpset
Lfpeori:
	movesw	%a0@(2),%d0
	eorw	%d7,%d0
	addql	&4,%a0
	bra	Lfpset
Lfppush:
	subql	&2,%a2
	movesw	%d7,%a2@
	addql	&2,%a0
	bra	Lfpdone
Lfptod:
	andiw	&7,%d0
	lslw	&2,%d0
	movew	%d7,%sp@(2,%d0:w)
	addql	&2,%a0
	bra	Lfpdone
Lfpset:					| d0: the new SR
	movew	%sr,%d5			| interrupts post signals and gp_vpend
	oriw	&0x0700,%sr		| from vsr: check them, then commit
	moveal	curproc,%a1
	movel	%a1@(P_HOLD),%d1
	notl	%d1
	andl	%a1@(P_SIG),%d1
	moveal	%a1@(P_EVPDP),%a1
	bnew	Lfpsi
	tstl	runrun
	bnew	Lfpsi
	moveq	&0,%d4
	movew	%d0,%d1
	eorw	%d6,%d1
	btst	&13,%d1
	beqw	Lfps0
	tstl	%a1@(GP_SPACE)
	bnew	Lfpsi
	tstl	%a1@(GP_MMU)
	sne	%d4			| S changes under the MMU: switch spaces once committed
Lfps0:
	movew	%d0,%d1
	andiw	&0x0700,%d1
	cmpw	%a1@(GP_VPEND),%d1
	bcsw	Lfpsi			| an interrupt would be taken
	btst	&GPF_SPIN_BIT,%a1@(GP_FLAGS+3)
	bnew	Lfps1
	btst	&13,%d0
	bnew	Lfps2
	movel	%a1@(GP_VUSP),%d1	| S off: the other stack
	movel	%a2,%a1@(GP_VUSP)
	moveal	%d1,%a2
	bra	Lfps2
Lfps1:
	oriw	&0x2000,%d0
Lfps2:
	movew	%d0,%d1
	andiw	&0x3700,%d1
	movew	%d1,%a1@(GP_VSR)
	andiw	&0x0700,%d1
	bnew	Lfps3
	bclr	&GPF_VPEND_BIT,%a1@(GP_FLAGS+3)
Lfps3:
	movew	%sp@(44),%d1
	andiw	&0x7f00,%d1
	andiw	&0x80ff,%d0
	orw	%d1,%d0
	movew	%d0,%sp@(44)
	tstb	%d4
	beqw	Lfpdone
	moveml	&0x00a0,%sp@-		| a0, a2
	moveq	&0,%d1
	movew	%a1@(GP_VSR),%d1
	andiw	&0x2000,%d1
	movel	%d1,%sp@-
	movel	%a1,%sp@-
	jsr	guest_mmu_space
	addql	&8,%sp
	moveml	%sp@+,&0x0500
Lfpdone:
	clrl	u+0x374
	movel	%a2,%usp
	movel	%a0,%sp@(46)
	addql	&1,guest_nfpriv
	moveml	%sp@+,&0x07ff
	rte
Lfpsi:
	movew	%d5,%sr
Lfpfault:
	clrl	u+0x374
Lfpslow:
	moveml	%sp@+,&0x07ff
	jmp	gc_gate_c
