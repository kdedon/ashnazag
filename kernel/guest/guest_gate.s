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
