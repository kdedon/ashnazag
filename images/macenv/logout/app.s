| Log Out, an application.  As a Mac's ShutDown does first, it asks
| every other process to quit and waits, up to 30 s, for them to go: the
| Finder's code must not run while another process owns the resource
| chain.  Then it ends the session as Shut Down's Logout button does, by
| _AUXDispatch(12, nil), which never returns.
| Frame: -8 own PSN, -16 the PSN walked, -24 its address descriptor,
| -32 the Quit event, -40 its reply, -56 an event record.
	.text
	pea	-4(%a5)
	.short	0xa86e			| InitGraf: WaitNextEvent needs a port
	.short	0xa8fe			| InitFonts
	.short	0xa912			| InitWindows
	link	%a6,#-56
	clr.w	-(%sp)
	pea	-8(%a6)
	move.w	#0x37,-(%sp)		| GetCurrentProcess
	.short	0xa88f
	addq.l	#2,%sp
	moveq	#0,%d4			| quits sent
	move.w	#300,%d3		| polls of 6 ticks
poll:
	moveq	#0,%d5			| others left
	clr.l	-16(%a6)
	clr.l	-12(%a6)
next:
	clr.w	-(%sp)
	pea	-16(%a6)
	move.w	#0x38,-(%sp)		| GetNextProcess
	.short	0xa88f
	move.w	(%sp)+,%d0
	bne.s	walked
	move.l	-16(%a6),%d0
	cmp.l	-8(%a6),%d0
	bne.s	other
	move.l	-12(%a6),%d0
	cmp.l	-4(%a6),%d0
	beq.s	next
other:
	addq.l	#1,%d5
	tst.l	%d4
	bne.s	next
	bsr.s	quit
	bra.s	next
walked:
	moveq	#1,%d4
	tst.l	%d5
	beq.s	logout
	clr.w	-(%sp)
	move.w	#-1,-(%sp)
	pea	-56(%a6)
	moveq	#6,%d0
	move.l	%d0,-(%sp)
	clr.l	-(%sp)
	.short	0xa860			| WaitNextEvent
	addq.l	#2,%sp
	subq.w	#1,%d3
	bne.s	poll
logout:
	subq.l	#4,%sp
	move.w	#12,-(%sp)
	clr.l	-(%sp)
	.short	0xabf9
	addq.l	#4,%sp
	unlk	%a6
	.short	0xa9f4		| _ExitToShell

| an 'aevt' 'quit' event, no reply, to the process at -16(a6)
quit:
	clr.w	-(%sp)
	move.l	#0x70736e20,-(%sp)	| 'psn '
	pea	-16(%a6)
	moveq	#8,%d0
	move.l	%d0,-(%sp)
	pea	-24(%a6)
	move.w	#0x0825,%d0		| AECreateDesc
	.short	0xa816
	move.w	(%sp)+,%d0
	bne.s	9f
	clr.w	-(%sp)
	move.l	#0x61657674,-(%sp)	| 'aevt'
	move.l	#0x71756974,-(%sp)	| 'quit'
	pea	-24(%a6)
	move.w	#-1,-(%sp)		| kAutoGenerateReturnID
	clr.l	-(%sp)
	pea	-32(%a6)
	move.w	#0x0b14,%d0		| AECreateAppleEvent
	.short	0xa816
	move.w	(%sp)+,%d0
	bne.s	8f
	clr.w	-(%sp)
	pea	-32(%a6)
	pea	-40(%a6)
	moveq	#0x11,%d0		| kAENoReply, kAENeverInteract
	move.l	%d0,-(%sp)
	clr.w	-(%sp)
	moveq	#-1,%d0
	move.l	%d0,-(%sp)
	clr.l	-(%sp)
	clr.l	-(%sp)
	move.w	#0x0d17,%d0		| AESend
	.short	0xa816
	addq.l	#2,%sp
	clr.w	-(%sp)
	pea	-32(%a6)
	move.w	#0x0204,%d0		| AEDisposeDesc
	.short	0xa816
	addq.l	#2,%sp
8:	clr.w	-(%sp)
	pea	-24(%a6)
	move.w	#0x0204,%d0
	.short	0xa816
	addq.l	#2,%sp
9:	rts
