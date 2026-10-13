| AmigaOS command: list the CyberStorm SCSI as Phase 5 board 8512/100
| when its ROM did not, so the AMIX boot passes it to the kernel.
	.text
	movea.l	4.w,%a6
	lea	name(%pc),%a1
	moveq	#0,%d0
	jsr	-552(%a6)		| OpenLibrary
	tst.l	%d0
	beq.s	out
	movea.l	%d0,%a6
	suba.l	%a0,%a0
	move.l	#8512,%d0
	moveq	#100,%d1
	jsr	-72(%a6)		| FindConfigDev
	tst.l	%d0
	bne.s	close
	jsr	-48(%a6)		| AllocConfigDev
	tst.l	%d0
	beq.s	close
	movea.l	%d0,%a0
	move.b	#100,17(%a0)		| er_Product
	move.w	#8512,20(%a0)		| er_Manufacturer
	move.l	#0xF00000,32(%a0)	| cd_BoardAddr
	move.l	#0x80000,36(%a0)	| cd_BoardSize
	jsr	-30(%a6)		| AddConfigDev
close:
	movea.l	%a6,%a1
	movea.l	4.w,%a6
	jsr	-414(%a6)		| CloseLibrary
out:
	moveq	#0,%d0
	rts
name:
	.asciz	"expansion.library"
	.balign	4
