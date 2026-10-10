	.text
	.globl polldown, pollup, polltrap, polltrack
polldown:
	moveml %d0-%d7/%a0-%a6,-(%sp)
	bsr refresh
	moveml (%sp)+,%d0-%d7/%a0-%a6
	tstb 0x172
	bne polldown
	rts
pollup:
	moveml %d0-%d7/%a0-%a6,-(%sp)
	bsr refresh
	moveml (%sp)+,%d0-%d7/%a0-%a6
	tstb 0x172
	bne done
	lea 4(%sp),%sp
	.short 0x4ef9
polltrack:
	.long 0
done:
	rts
refresh:
	clrl -(%sp)
	movel #0x6e,-(%sp)
	clrl -(%sp)
	moveq #38,%d0
polltrap:
	trap #0
	nop
	lea 12(%sp),%sp
	tstl polldiag(%pc)
	beq refreshdone
	moveq #0,%d0
	moveb 0x172,%d0
	movel %d0,-(%sp)
	pea 4
	movel #0x6e,-(%sp)
	clrl -(%sp)
	moveq #38,%d0
pollstatetrap:
	trap #0
	nop
	lea 16(%sp),%sp
refreshdone:
	rts

	.globl pollhit, hitpointtrap, hitresulttrap
pollhit:
	subqw #2,%sp
	movel 0x830,-(%sp)
	pea (%sp)
	.short 0xa871
	moveml %d0-%d7/%a0-%a6,-(%sp)
	movew %ccr,-(%sp)
	bsr hitpoint
	movew (%sp)+,%ccr
	moveml (%sp)+,%d0-%d7/%a0-%a6
	pea (%a3)
	.short 0xa8ad
	moveb (%sp)+,%d0
	moveml %d0-%d7/%a0-%a6,-(%sp)
	movew %ccr,-(%sp)
	bsr hitresult
	movew (%sp)+,%ccr
	moveml (%sp)+,%d0-%d7/%a0-%a6
	rts
hitpoint:
	pea 2
	movel #0x6e,-(%sp)
	clrl -(%sp)
	moveq #38,%d0
hitpointtrap:
	trap #0
	nop
	lea 12(%sp),%sp
	rts
hitresult:
	pea 3
	movel #0x6e,-(%sp)
	clrl -(%sp)
	moveq #38,%d0
hitresulttrap:
	trap #0
	nop
	lea 12(%sp),%sp
	rts

polldiag:
	.long 0
