	.text
	.globl entry
entry:
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
	movew #20002,%d0
	.short 0xa9c9
	moveq #1,%d0
	bra finish
done:
	moveq #0,%d0
finish:
	unlk %a6
	rts
	.balign 4
