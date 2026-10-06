| SndTest: the INIT's entry and its Toolbox and Unix calls.
| Position-independent: the entry copies everything to the system heap.
	.text
	.globl	entry
entry:
	moveml	%d3-%d7/%a2-%a6,-(%sp)
	lea	_end(%pc),%a0
	lea	entry(%pc),%a1
	movel	%a0,%d3
	subl	%a1,%d3
	movel	%d3,%d0
	.short	0xa71e			| _NewPtrSysClear
	tstl	%d0
	bnes	1f
	moveal	%a0,%a2
	lea	entry(%pc),%a0
	moveal	%a2,%a1
	movel	%d3,%d0
	.short	0xa02e			| _BlockMove
	.short	0xa0bd			| _FlushCodeCache
	lea	st_main(%pc),%a0
	lea	entry(%pc),%a1
	subl	%a1,%a0
	addl	%a2,%a0
	jsr	(%a0)
1:	moveml	(%sp)+,%d3-%d7/%a2-%a6
	rts

| auxsysr(n, a, b, c) -> d0, or -errno
	.globl	auxsysr
auxsysr:
	movel	4(%sp),%d0
	movel	16(%sp),-(%sp)
	movel	16(%sp),-(%sp)
	movel	16(%sp),-(%sp)
	clrl	-(%sp)
	trap	#0
	lea	16(%sp),%sp
	bccs	1f
	negl	%d0
1:	rts

| newhandlesys(size) -> handle in the system heap, locked; 0 if none
	.globl	newhandlesys
newhandlesys:
	movel	4(%sp),%d0
	.short	0xa722
	tstw	%d0
	beqs	1f
	subal	%a0,%a0
	bras	2f
1:	movel	%a0,-(%sp)
	.short	0xa029			| _HLock
	moveal	(%sp)+,%a0
2:	movel	%a0,%d0
	rts

| newptr(size) -> pointer in the system heap, cleared
	.globl	newptr
newptr:
	movel	4(%sp),%d0
	.short	0xa71e
	movel	%a0,%d0
	rts

	.globl	ticks
ticks:
	movel	0x16a:w,%d0
	rts

| sysbeep(ticks)
	.globl	sysbeep
sysbeep:
	movew	6(%sp),-(%sp)
	.short	0xa9c8
	rts

| sndnew(&chan, synth, init, proc)
	.globl	sndnew
sndnew:
	clrw	-(%sp)
	movel	6(%sp),-(%sp)
	movew	16(%sp),-(%sp)
	movel	20(%sp),-(%sp)
	movel	28(%sp),-(%sp)
	.short	0xa807
	movew	(%sp)+,%d0
	extl	%d0
	rts

| sndplay(chan, h, async)
	.globl	sndplay
sndplay:
	clrw	-(%sp)
	movel	6(%sp),-(%sp)
	movel	14(%sp),-(%sp)
	movew	24(%sp),%d0
	lslw	#8,%d0
	movew	%d0,-(%sp)
	.short	0xa805
	movew	(%sp)+,%d0
	extl	%d0
	rts

| snddo(chan, cmd): SndDoCommand, waiting for room
	.globl	snddo
snddo:
	clrw	-(%sp)
	movel	6(%sp),-(%sp)
	movel	14(%sp),-(%sp)
	clrw	-(%sp)
	.short	0xa803
	movew	(%sp)+,%d0
	extl	%d0
	rts

| the callBackCmd routine: (chan, var cmd)
	.globl	t_cb
t_cb:
	movel	%a0,-(%sp)
	moveal	8(%sp),%a0
	movel	4(%a0),-(%sp)
	bsrl	cb_run
	addql	#4,%sp
	moveal	(%sp)+,%a0
	moveal	(%sp)+,%a1
	addql	#8,%sp
	jmp	(%a1)
