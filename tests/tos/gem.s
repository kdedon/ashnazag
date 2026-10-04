| gem.s -- start-up and GEMDOS/BIOS traps for a TOS program built from C
	.text
	.globl	_start, trap1, trap13
_start:	move.l	4(sp),a0		| basepage
	lea	stack+4096,sp
	move.l	#_end,d0
	sub.l	a0,d0
	move.l	d0,-(sp)		| Mshrink(0, basepage, size)
	move.l	a0,-(sp)
	clr.w	-(sp)
	move.w	#0x4a,-(sp)
	trap	#1
	lea	12(sp),sp
	jsr	main
	move.w	d0,-(sp)		| Pterm(main())
	move.w	#0x4c,-(sp)
	trap	#1

| long trap1(short *w, int n): the call whose n words are w
trap1:	movem.l	d2/a2-a3,-(sp)
	bsr.s	args
	trap	#1
	bra.s	done
trap13:	movem.l	d2/a2-a3,-(sp)
	bsr.s	args
	trap	#13
done:	move.l	a3,sp
	movem.l	(sp)+,d2/a2-a3
	rts
args:	move.l	(sp)+,a1
	move.l	sp,a3
	move.l	16(sp),a0
	move.l	20(sp),d1
	lea	(a0,d1.l*2),a0
1:	move.w	-(a0),-(sp)
	subq.l	#1,d1
	bne.s	1b
	jmp	(a1)

	.bss
	.lcomm	stack, 4096
