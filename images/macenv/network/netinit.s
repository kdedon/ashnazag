	.text
	.globl entry
entry:
	moveml %d0-%d7/%a0-%a6,-(%sp)
	moveq #0x6e,%d0
	.short 0xa346
	movel %a0,%a2
	cmpil #0x414e4554,2(%a2)
	beq.s init_done
	movel #resident_end-resident,%d0
	.short 0xa71e
	tstw %d0
	bne.s init_done
	movel %a0,%a3
	movel %a0,%a1
	lea resident(%pc),%a0
	movew #resident_end-resident-1,%d0
1:	moveb (%a0)+,(%a1)+
	dbra %d0,1b
	movel %a2,6(%a3)
	.short 0xa0bd
	movel %a3,%a0
	moveq #0x6e,%d0
	.short 0xa247
init_done:
	moveml (%sp)+,%d0-%d7/%a0-%a6
	rts
resident:
	bra.s dispatch
	.long 0x414e4554
previous:
	.long 0
dispatch:
	cmpiw #0x2d,%d0
	bne.s forward
	cmpib #0x0e,49(%a0)
	bne.s forward
	cmpib #0x81,50(%a0)
	bne.s forward
	tstb 51(%a0)
	bne.s forward
	moveml %d1-%d7/%a1-%a6,-(%sp)
	movel %a0,-(%sp)
	moveq #0x16,%d0
	movel previous(%pc),%a1
	jsr (%a1)
	movel (%sp)+,%a2
	movel %a2,%a0
	tstw %d0
	bne.s driver_done
	movel #enet_end-enet,%d0
	.short 0xa722
	tstw %d0
	bne.s driver_done
	movel %a0,%a3
	movel (%a0),%a1
	lea enet(%pc),%a0
	movew #enet_end-enet-1,%d0
1:	moveb (%a0)+,(%a1)+
	dbra %d0,1b
	.short 0xa0bd
	movel %a3,(%a2)
	movel %a3,%a0
	moveq #0,%d0
driver_done:
	moveml (%sp)+,%d1-%d7/%a1-%a6
	rts
forward:
	movel previous(%pc),-(%sp)
	rts
	.even
enet:
	.incbin "enet.drvr"
enet_end:
resident_end:
	.section .note.GNU-stack,"",@progbits
