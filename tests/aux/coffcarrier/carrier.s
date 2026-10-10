	.text
	.globl gettooltrap, settooltrap, setstackbase, stackcontext, auxsys4, entry, auxwrite, auxsysr, auxspawn, auxwait, waitnext, getnext, getprocess, exitapp
entry:
	movel %sp,%d2
	movel %a5,%d3
	movel 0x908,%d4
	movel %a4,%d5
	movel (%sp),%d6
	jsr stackalloc(%pc)
	tstl %d0
	beq exitnative
	movel %d0,%a4
	lea 65472(%a4),%a4
	movel %d0,(%a4)
	movel %d2,4(%a4)
	movel %d3,8(%a4)
	movel %d4,12(%a4)
	movel %d5,16(%a4)
	movel %d6,44(%a4)
	movel %a4,%sp
	pea exitapp(%pc)
	pea -4(%a5)
	.short 0xa86e
	bra start
gettooltrap:
	movel 4(%sp),%d0
	.short 0xa746
	movel %a0,%d0
	rts
settooltrap:
	movel 4(%sp),%d0
	movel 8(%sp),%a0
	.short 0xa647
	rts
setstackbase:
	movel 4(%sp),0x908
	rts
stackcontext:
	movel %a4,%d0
	rts
auxsys4:
	movel 4(%sp),%d0
	movel 20(%sp),-(%sp)
	movel 20(%sp),-(%sp)
	movel 20(%sp),-(%sp)
	movel 20(%sp),-(%sp)
	clrl -(%sp)
	trap #0
	lea 20(%sp),%sp
	bccs 1f
	negl %d0
1:	rts
auxwrite:
	moveq #4,%d0
	trap #0
	rts
auxsysr:
	movel 4(%sp),%d0
	movel 16(%sp),-(%sp)
	movel 16(%sp),-(%sp)
	movel 16(%sp),-(%sp)
	clrl -(%sp)
	trap #0
	lea 16(%sp),%sp
	bccs 1f
	negl %d0
1:	rts
auxwait:
	moveq #0,%d0
	movew #151,%d0
	trap #0
	bccs 1f
	negl %d0
	rts
1:	movel 8(%sp),%a0
	movel %d1,(%a0)
	rts
auxspawn:
	moveml %d2-%d7/%a2-%a6,-(%sp)
	movel 48(%sp),%a3
	movel 52(%sp),%a2
	moveq #2,%d0
	trap #0
	movew %ccr,%d2
	btst #0,%d2
	bnes forkerror
	tstb %d1
	beqs forkparent
	/* Forked native frames share memory; enter C on the private stack. */
	lea 0x3fffff00,%sp
	movel %d0,16(%a3)
	movel %d1,20(%a3)
	movew %d2,24(%a3)
	movel %sp,28(%a3)
	movel %a2,-(%sp)
	jsr childexec(%pc)
	pea 127
	clrl -(%sp)
	moveq #1,%d0
	trap #0
	bra .
forkerror:
	movel %d0,(%a3)
	movel %d1,4(%a3)
	movew %d2,8(%a3)
	movel %sp,12(%a3)
	negl %d0
	bra forkreturn
forkparent:
	movel %d0,(%a3)
	movel %d1,4(%a3)
	movew %d2,8(%a3)
	movel %sp,12(%a3)
forkreturn:
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
waitnext:
	movel 4(%sp),%a0
	moveml %d2-%d7/%a2-%a6,-(%sp)
	clrw -(%sp)
	clrw -(%sp)
	movel %a0,-(%sp)
	pea 1
	clrl -(%sp)
	.short 0xa860
	addqw #2,%sp
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
exitapp:
	jsr stackrestore(%pc)
	movel 4(%a4),%sp
	movel 44(%a4),(%sp)
	movel 8(%a4),%a5
	movel (%a4),%d2
	movel 16(%a4),%a4
	clrl -(%sp)
	clrl -(%sp)
	movel %d2,-(%sp)
	pea 2
	clrl -(%sp)
	moveq #52,%d0
	trap #0
	lea 20(%sp),%sp
exitnative:
	.short 0xa9f4
	bra exitnative
getnext:
	clrw -(%sp)
	movel 6(%sp),-(%sp)
	movew #56,-(%sp)
	.short 0xa88f
	movew (%sp)+,%d0
	extl %d0
	rts
getprocess:
	clrw -(%sp)
	movel 6(%sp),-(%sp)
	movel 14(%sp),-(%sp)
	movew #58,-(%sp)
	.short 0xa88f
	movew (%sp)+,%d0
	extl %d0
	rts


	.globl currenta5
currenta5:
	movel 0x904,%d0
	rts

	.globl systemversion
systemversion:
	moveq #0,%d0
	movew 0x15a,%d0
	rts

	.globl getcurrent
getcurrent:
	clrw -(%sp)
	movel 6(%sp),-(%sp)
	movew #55,-(%sp)
	.short 0xa88f
	movew (%sp)+,%d0
	extl %d0
	rts
