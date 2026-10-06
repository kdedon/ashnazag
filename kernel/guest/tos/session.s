| session.s -- "Session..." in the Desk menu of every GEM program: Log
| Out ends the TOS session; root also gets Shut Down, which the host
| refuses to anyone else.  A desk accessory, position-independent.
|
|   m68k-elf-as -m68040 session.s; ld -Ttext=0; objcopy -O binary; session.sh

	.set	CART, 0xfa0000		| the machine layer: /dev/tos at +$78, host calls at +$7c
	.set	TOSIOC_HALT, 0x5409

	.text
	.globl	_start
_start:	lea	stack+1024(pc),sp
	lea	pb(pc),a0
	lea	ctrl(pc),a1
	move.l	a1,(a0)+
	lea	global(pc),a1
	move.l	a1,(a0)+
	lea	intin(pc),a1
	move.l	a1,(a0)+
	lea	intout(pc),a1
	move.l	a1,(a0)+
	lea	addrin(pc),a1
	move.l	a1,(a0)+
	lea	addrout(pc),a1
	move.l	a1,(a0)
	move.l	#0x0a000100,d0		| appl_init
	bsr	aes
	move.w	intout(pc),d7
	moveq	#-1,d6			| our menu entry: none
	tst.w	d7
	bmi.s	loop
	cmp.l	#0xabcdef42,CART
	bne.s	loop
	cmp.l	#0x544f534d,CART+0x18	| "TOSM"
	bne.s	loop
	lea	intin(pc),a0
	move.w	d7,(a0)
	lea	title(pc),a0
	bsr	addr
	move.l	#0x23010101,d0		| menu_register
	bsr	aes
	move.w	intout(pc),d6
	moveq	#24,d0			| getuid: the real uid
	move.l	CART+0x7c,a0
	jsr	(a0)
	tst.l	d0
	seq	d5			| root

loop:	lea	msg(pc),a0
	bsr	addr
	move.l	#0x17000101,d0		| evnt_mesag
	bsr	aes
	lea	msg(pc),a0
	cmp.w	#40,(a0)		| AC_OPEN
	bne.s	loop
	tst.w	d6
	bmi.s	loop
	cmp.w	8(a0),d6
	bne.s	loop
	lea	ask(pc),a0
	tst.b	d5
	beq.s	1f
	lea	askroot(pc),a0
1:	bsr	alert
	cmp.w	#1,d0
	beq.s	logout
	tst.b	d5
	beq.s	loop
	cmp.w	#2,d0
	bne.s	loop
	clr.l	-(sp)			| ioctl(/dev/tos, TOSIOC_HALT, 0)
	move.l	#TOSIOC_HALT,-(sp)
	move.l	CART+0x78,-(sp)
2:	moveq	#54,d0
	move.l	CART+0x7c,a0
	jsr	(a0)
	bcc.s	3f
	cmp.l	#4,d0			| EINTR: the carrier signal interrupted it
	beq.s	2b
3:	lea	12(sp),sp
	lea	refused(pc),a0
	bsr.s	alert
	bra.s	loop

| exit(0): the host ends the session, its display and its drives
logout:	clr.l	-(sp)
1:	moveq	#1,d0
	move.l	CART+0x7c,a0
	jsr	(a0)
	bra.s	1b

| form_alert(1, a0) -> d0
alert:	lea	intin(pc),a1
	move.w	#1,(a1)
	bsr.s	addr
	move.l	#0x34010101,d0
	bsr.s	aes
	move.w	intout(pc),d0
	rts

addr:	lea	addrin(pc),a1
	move.l	a0,(a1)
	rts

| the AES call whose opcode and intin, intout, addrin counts are d0's bytes
aes:	lea	ctrl+8(pc),a0
	moveq	#3,d1
1:	clr.w	-(a0)
	move.b	d0,1(a0)
	lsr.l	#8,d0
	dbra	d1,1b
	lea	pb(pc),a0
	move.l	a0,d1
	move.l	#200,d0
	trap	#2
	rts

title:	.asciz	"  Session..."
	.even
ask:	.asciz	"[2][End this session?][Log Out|Cancel]"
	.even
askroot: .asciz	"[2][End this session, or shut|down the machine?][Log Out|Shut Down|Cancel]"
	.even
refused: .asciz	"[3][The machine was not|shut down.][ OK ]"
	.balign	4
pb:	.space	24
ctrl:	.space	10
global:	.space	30
intin:	.space	32
intout:	.space	14
addrin:	.space	8
addrout: .space	4
msg:	.space	16
stack:	.space	1024
