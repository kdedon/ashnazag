| Entry and traps for mtcp.c.  The segment starts here.
	.text
	.globl	auxwrite, trapopen, trapctl, newptr, idle, probe0, probe15
	.globl	findsys, openrf, getrsrc, dnrcall, resproc
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
| FindFolder(kOnSystemDisk, 'macs', false, &vref, &dirid): the System Folder
findsys:
	clr.w	-(%sp)
	move.w	#-32768,-(%sp)
	move.l	#0x6d616373,-(%sp)
	clr.w	-(%sp)
	move.l	14(%sp),-(%sp)
	move.l	22(%sp),-(%sp)
	moveq	#0,%d0
	.short	0xa823
	move.w	(%sp)+,%d0
	ext.l	%d0
	rts
| HOpenResFile(vref, dirid, name, fsRdPerm): the file's reference, -1 if none
openrf:
	clr.w	-(%sp)
	move.w	8(%sp),-(%sp)
	move.l	12(%sp),-(%sp)
	move.l	20(%sp),-(%sp)
	move.w	#0x0100,-(%sp)
	.short	0xa81a
	move.w	(%sp)+,%d0
	ext.l	%d0
	rts
| Get1Resource(type, id), detached and locked: the code's address, 0 if none
getrsrc:
	clr.l	-(%sp)
	move.l	8(%sp),-(%sp)
	move.w	18(%sp),-(%sp)
	.short	0xa81f
	move.l	(%sp),%d0
	beq.s	1f
	move.l	%d0,-(%sp)
	.short	0xa992
	move.l	(%sp),%a0
	.short	0xa029
	move.l	(%a0),%d0
1:	addq.l	#4,%sp
	rts
| (*code)(sel, a, b, c, d): the resolver, C calls that may use any register
dnrcall:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0
	move.l	68(%sp),-(%sp)
	move.l	68(%sp),-(%sp)
	move.l	68(%sp),-(%sp)
	move.l	68(%sp),-(%sp)
	move.l	68(%sp),-(%sp)
	jsr	(%a0)
	lea	20(%sp),%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts
| pascal resultProc(hostInfo, userData): *userData = 1
resproc:
	move.l	4(%sp),%a0
	moveq	#1,%d0
	move.l	%d0,(%a0)
	move.l	(%sp)+,%a0
	addq.l	#8,%sp
	jmp	(%a0)
