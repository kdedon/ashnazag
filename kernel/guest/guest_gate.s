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
	.globl	guest_gate_c
guest_gate_c:
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

| guest_linea -- A-line of a guest with GPF_ALINE, from the vector-10
| gate: the 8-byte format-0 frame the guest's dispatcher expects goes
| on the user stack and the guest continues at the long at vVBR + $28.
|
|   user frame:  SR | vSR   PC (the A-line word)   $0028
|   then:        real SR = CCR only, vSR = S and IPL
|
| Pending signals, a reschedule or a fault on the user stack take the
| C path (guest_gate_c), which does the same and leaves through ureturn.
| A fault in a moves is resolved by the page-fault path or lands at
| Llafault with this stack (the u+0x374 pad, as the user-access
| primitives use it).

GP_FLAGS = 12
GP_VSR = 20
GP_VVBR = 28
P_EVPDP = 0xc8
P_SIG = 0x9c
P_HOLD = 0xa4

	.globl	guest_linea
guest_linea:
	moveml	&0xc0c0,%sp@-		| d0-d1/a0-a1; frame at 16(sp)
	moveal	curproc,%a1
	movel	%a1@(P_HOLD),%d0
	notl	%d0
	andl	%a1@(P_SIG),%d0
	bnew	Llaslow
	tstl	runrun
	bnew	Llaslow
	moveal	%a1@(P_EVPDP),%a1
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
	jmp	guest_gate_c

| guest_ftrap -- trap #n of a guest with GPF_FTRAP: the CPU's format-0
| frame goes on the guest's supervisor stack, as guest_reflect builds it.
|
|   guest frame:  SR | vSR   PC   format/vector
|   then:         real SR = CCR, vSR = S, M and IPL kept
|
| Pending signals, a reschedule, another format or a fault take the C path.

GP_VPEND = 22
GP_VUSP = 24
GPF_SPIN_BIT = 2
GPF_VPEND_BIT = 3

	.globl	guest_ftrap
guest_ftrap:
	moveml	&0xe0e0,%sp@-		| d0-d2/a0-a2; frame at 24(sp)
	moveal	curproc,%a1
	movel	%a1@(P_HOLD),%d0
	notl	%d0
	andl	%a1@(P_SIG),%d0
	bnew	Lftslow
	tstl	runrun
	bnew	Lftslow
	movew	%sp@(30),%d0
	cmpiw	&0x0fff,%d0		| format 0
	bhiw	Lftslow
	moveal	%a1@(P_EVPDP),%a1
	moveq	&1,%d1
	movec	%d1,%sfc
	movec	%d1,%dfc
	movel	&Lftfault,u+0x374
	moveal	%a1@(GP_VVBR),%a0
	movesl	%a0@(0,%d0:w),%d1	| the guest's handler
	movew	%a1@(GP_VSR),%d2
	movel	%usp,%a2
	moveal	%a2,%a0
	btst	&13,%d2			| vS: on the active stack
	bnew	Lft1
	btst	&GPF_SPIN_BIT,%a1@(GP_FLAGS+3)
	bnew	Lft1
	moveal	%a1@(GP_VUSP),%a0
Lft1:
	subql	&8,%a0
	movew	%sp@(24),%d0
	andiw	&0x80ff,%d0
	andiw	&0x3700,%d2
	orw	%d2,%d0
	movesw	%d0,%a0@
	movel	%sp@(26),%d0
	movesl	%d0,%a0@(2)
	movew	%sp@(30),%d0
	movesw	%d0,%a0@(6)
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
	andiw	&0x7fff,%sp@(24)	| T1 off
	movel	%d1,%sp@(26)
	addql	&1,guest_nftrap
	moveml	%sp@+,&0x0707
	rte
Lftfault:
	clrl	u+0x374
Lftslow:
	moveml	%sp@+,&0x0707
	jmp	guest_gate_c

| guest_fpriv -- vector 8 of a guest with GPF_PRIV in virtual supervisor
| mode: rte (format 0), move to and from SR (Dn, (sp)+, -(sp), #imm)
| and ori/andi/eori #,SR against gp_vsr, as gcpu.c emulates them, and
| move to and from USP against gp_vusp.  An
| IPL below gp_vpend (an interrupt would be taken), pending signals, a
| reschedule, other instructions or a fault take the C path.

	.globl	guest_fpriv
guest_fpriv:
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
	beqw	Lfpslow
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
	bra	Lfpfault
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
	jmp	guest_gate_c
