	.text
	.globl entry, forceword, forward, previous, leavepoll
entry:
	moveml %d0-%d7/%a0-%a6,-(%sp)
	bsr.w frontentry
	nop
	nop
	tstl (%a0)
	beq forward
	cmpil #3,(%a0)
	bge forward
	movel 0x904,%a5
	clrl -(%sp)
	.short 0xa975
	movel (%sp)+,%d0
	lea forceword(%pc),%a0
	cmpil #1,(%a0)
	bne deadline
	movel %d0,%d1
	addil #300,%d1
	movel %d1,4(%a0)
	movel #2,(%a0)
deadline:
	subl 4(%a0),%d0
	bpl clear
	bsr foreground
	tstl %d0
	beq forward
clear:
	bsr cleanup
	bcc cleared
	lea forceword(%pc),%a0
	movel #4,(%a0)
	bra forward
cleared:
	lea forceword(%pc),%a0
	clrl (%a0)
forward:
	moveml (%sp)+,%d0-%d7/%a0-%a6
	.short 0x4ef9
previous:
	.long 0
foreground:
	link %a6,#-16
	clrw -(%sp)
	pea -8(%a6)
	moveq #-1,%d0
	movel %d0,-(%sp)
	movew #0x39,-(%sp)
	.short 0xa88f
	movew (%sp)+,%d0
	bne done
	clrw -(%sp)
	pea -16(%a6)
	movew #0x37,-(%sp)
	.short 0xa88f
	movew (%sp)+,%d0
	bne done
	movel -8(%a6),%d0
	cmpl -16(%a6),%d0
	bne done
	movel -4(%a6),%d0
	cmpl -12(%a6),%d0
	bne done
	lea forceword(%pc),%a0
	movel #3,(%a0)
	movew #20002,%d0
	.short 0xa9c9
	moveq #1,%d0
	bra finish
done:
	moveq #0,%d0
finish:
	unlk %a6
	rts
cleanup:
	moveml %d0-%d7/%a0-%a6,-(%sp)
	moveq #1,%d0
	movel %d0,-(%sp)
	movel #0x6e,-(%sp)
	clrl -(%sp)
	moveq #38,%d0
leavepoll:
	trap #0
	nop
	lea 12(%sp),%sp
	moveml (%sp)+,%d0-%d7/%a0-%a6
	rts
	.balign 4
forceword:
	.long 0,0

	.org 0x1fe
.text
.globl frontentry,fronttrap,frontflag,frontmagic
frontentry:
 movew %ccr,-(%sp)
 tstl frontflag(%pc)
 beq frontdone
 linkw %a6,#-280
 movel 0x904,%a5
 movel %a5,-260(%a6)
 clrw -(%sp)
 pea -280(%a6)
 movew #55,-(%sp)
 .short 0xa88f
 movew (%sp)+,%d0
 bne frontcleanup
 clrw -(%sp)
 pea -272(%a6)
 moveq #-1,%d0
 movel %d0,-(%sp)
 movew #57,-(%sp)
 .short 0xa88f
 movew (%sp)+,%d0
 bne frontcleanup
 movel -280(%a6),%d0
 cmpl -272(%a6),%d0
 bne frontcleanup
 movel -276(%a6),%d0
 cmpl -268(%a6),%d0
 bne frontcleanup
 clrl -(%sp)
 .short 0xa924
 movel (%sp)+,-264(%a6)
 tstl -264(%a6)
 beq frontcleanup
 movel -264(%a6),-(%sp)
 pea -256(%a6)
 .short 0xa919
 pea -280(%a6)
 pea 5
 pea 0x6e
 clrl -(%sp)
 moveq #38,%d0
fronttrap:
 trap #0
 lea 16(%sp),%sp
frontcleanup:
 unlk %a6
frontdone:
 movew (%sp)+,%ccr
 lea (wneword,%pc,%zd0.w),%a0
 rts
frontmagic:
 .long 0x46574e31
frontflag:
 .long 0
.set wneword,forceword
