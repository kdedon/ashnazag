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
	tstb	runrun
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
