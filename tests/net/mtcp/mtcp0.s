| Entry and traps for mtcp.c.  The segment starts here.
	.text
	.globl	auxwrite, trapopen, trapctl, newptr, idle, probe0, probe15
entry:	bra	start
| A/UX open(path, 0) by trap #0 and by trap #15: a failing call the kernel trace shows
probe0:
	moveq	#5,%d0
	trap	#0
	rts
probe15:
	move.l	4(%sp),%a0
	moveq	#0,%d1
	moveq	#5,%d0
	trap	#15
	rts
| A/UX write(fd, buf, n): trap #0, number in d0, arguments above the return address
auxwrite:
	moveq	#4,%d0
	trap	#0
	bcc.s	1f
	moveq	#-1,%d0
1:	rts
| _Open, _Control (synchronous): parameter block in a0, result in d0
trapopen:
	move.l	4(%sp),%a0
	.short	0xa000
	ext.l	%d0
	rts
trapctl:
	move.l	4(%sp),%a0
	.short	0xa004
	ext.l	%d0
	rts
| _NewPtr(size): the pointer, 0 if none
newptr:
	move.l	4(%sp),%d0
	.short	0xa11e
	move.l	%a0,%d0
	rts
| WaitNextEvent(everyEvent, &ev, 60, nil)
idle:
	lea	-16(%sp),%sp
	move.l	%sp,%a0
	clr.w	-(%sp)
	move.w	#-1,-(%sp)
	move.l	%a0,-(%sp)
	pea	60
	clr.l	-(%sp)
	.short	0xa860
	addq.l	#2,%sp
	lea	16(%sp),%sp
	rts
