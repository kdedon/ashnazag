| axbstart.s -- head of the AXB loader and its hand-off code.
|
| The root sector checks the magic and calls offset 4 as
| loader(device, AXB start sector, root sector).  The loader is
| position independent; it copies go_kernel out of the kernel's way.
	.text
	.globl	_start
_start:
	.long	0x41584232		| 'AXB2'
	bra.w	entry
	.globl	ax_timeout
ax_timeout:
	.long	3			| prompt seconds, set by the installer
	.globl	ax_ksum
ax_ksum:
	.long	0			| kernel word sum, set by the installer

| Uninitialised data lies past the image; clear it.
entry:	lea	%pc@(__bss_start),%a0
	lea	%pc@(_end),%a1
	bras	2f
1:	clrb	%a0@+
2:	cmpal	%a1,%a0
	blts	1b
	bra.w	loader

| go(base, struct go *): IPL 7, caches, MMU and transparent translation
| off, then run the copy of go_kernel at base.  Caches go off before the
| copy runs so it is fetched from memory.
|   0 cpu (30, 40, 60)  4 entry  8 stack  12 boot record dst
|   16 its src  20 its length  24 segment count
|   28 segments: dst, src, file size, memory size
|   92 address of a boot record word to translate to physical, or 0
	.globl	go, go_kernel, go_end
go:	movew	#0x2700,%sr
	moveal	%sp@(4),%a4
	moveal	%sp@(8),%a6
	moveq	#0,%d0
	cmpil	#30,%a6@
	bnes	1f
	movel	#0x0808,%d1		| clear both caches
	movec	%d1,%cacr
	movec	%d0,%cacr
	movel	%d0,%sp@-
	pmove	%sp@,%tc
	pmove	%sp@,%tt0
	pmove	%sp@,%tt1
	addql	#4,%sp
	jmp	%a4@
1:	movel	%a6@(92),%d1
	beqs	2f
	moveal	%d1,%a1
	moveal	%a1@,%a0
	moveq	#5,%d1
	.word	0x4e7b,0x1001		| movec %d1,%dfc
	cmpil	#60,%a6@
	bnes	3f
	.word	0xf5c8			| plpar (%a0)
	movel	%a0,%a1@
	bras	2f
3:	.word	0xf568			| ptestr (%a0)
	.word	0x4e7a,0x1805		| movec %mmusr,%d1
	btst	#1,%d1			| transparent: as is
	bnes	2f
	btst	#0,%d1
	beqs	2f
	andiw	#0xf000,%d1
	movel	%a0,%d2
	andil	#0xfff,%d2
	orl	%d2,%d1
	movel	%d1,%a1@
2:	.word	0xf4f8			| cpusha %bc
	.word	0x4e7b,0x0002		| movec %d0,%cacr
	.word	0x4e7b,0x0003		| movec %d0,%tc
	.word	0x4e7b,0x0004		| movec %d0,%itt0
	.word	0x4e7b,0x0005		| movec %d0,%itt1
	.word	0x4e7b,0x0006		| movec %d0,%dtt0
	.word	0x4e7b,0x0007		| movec %d0,%dtt1
	.word	0xf518			| pflusha
	jmp	%a4@

| go_kernel: copy the segments and the boot record, enter.  Runs from
| the start of a 2 KB area that also holds its stack, the parameters
| (a6) and the boot record.
go_kernel:
	lea	%pc@(go_kernel+2048),%sp
	movel	%a6@(24),%d7
	lea	%a6@(28),%a5
3:	subql	#1,%d7
	bmis	5f
	moveal	%a5@+,%a0
	moveal	%a5@+,%a1
	movel	%a5@+,%d1
	movel	%a5@+,%d2
	subl	%d1,%d2
	bsrs	cp
4:	subql	#4,%d2
	bmis	7f
	clrl	%a0@+
	bras	4b
7:	addql	#4,%d2
6:	subql	#1,%d2
	bmis	3b
	clrb	%a0@+
	bras	6b
5:	moveal	%a6@(12),%a0
	moveal	%a6@(16),%a1
	movel	%a6@(20),%d1
	bsrs	cp
	moveal	%a6@(4),%a0
	moveal	%a6@(8),%sp
	jmp	%a0@

| copy d1 bytes from a1 to a0, both even
cp:	subql	#4,%d1
	bmis	1f
	movel	%a1@+,%a0@+
	bras	cp
1:	addql	#4,%d1
2:	subql	#1,%d1
	bmis	3f
	moveb	%a1@+,%a0@+
	bras	2b
3:	rts
go_end:

| trap_frame(trap, frame, size): copy the call's argument frame (opcode
| first, as the ROM reads it) to the stack and trap #1, #13 or #14.
	.globl	trap_frame
trap_frame:
	moveml	%d2-%d7/%a2-%a6,%sp@-
	movel	%sp@(48),%d0
	moveal	%sp@(52),%a0
	movel	%sp@(56),%d1
	moveal	%sp,%a6
	subal	%d1,%sp
	moveal	%sp,%a1
1:	subql	#1,%d1
	bmis	2f
	moveb	%a0@+,%a1@+
	bras	1b
2:	cmpil	#1,%d0
	bnes	3f
	trap	#1
	bras	5f
3:	cmpil	#13,%d0
	bnes	4f
	trap	#13
	bras	5f
4:	trap	#14
5:	moveal	%a6,%sp
	moveml	%sp@+,%d2-%d7/%a2-%a6
	rts

| ramprobe(base, max): bytes of RAM from base, in 1 MB steps up to max,
| 68030 data cache off.  Each step's first long gets its own address;
| a bus error, a value that does not stick or base losing its mark
| (an alias) ends the count.
	.globl	ramprobe
ramprobe:
	moveml	%d2-%d3/%a2,%sp@-
	moveal	%sp@(16),%a0
	movel	%sp@(20),%d2
	movew	%sr,%sp@-
	oriw	#0x0700,%sr
	movec	%cacr,%d0
	movel	%d0,%sp@-
	andiw	#0xfeff,%d0		| data cache off
	oriw	#0x0800,%d0		| and cleared
	movec	%d0,%cacr
	movec	%vbr,%a2
	movel	%a2@(8),%sp@-
	lea	%pc@(2f),%a1
	movel	%a1,%a2@(8)
	movel	%sp,%d3
	moveq	#0,%d1
	movel	#0x54545242,%a0@	| 'TTRB'; a bus error here leaves 0
1:	cmpl	%d2,%d1
	bcc.s	2f
	lea	%a0@(0,%d1:l),%a1
	tstl	%d1
	beq.s	3f
	movel	%a1,%a1@
	nop
	cmpl	%a1@,%a1
	bne.s	2f
3:	cmpil	#0x54545242,%a0@
	bne.s	2f
	addil	#0x100000,%d1
	bra.s	1b
2:	movel	%d3,%sp
	movel	%sp@+,%a2@(8)
	movel	%sp@+,%d0
	movec	%d0,%cacr
	movew	%sp@+,%sr
	movel	%d1,%d0
	moveml	%sp@+,%d2-%d3/%a2
	rts

| peek(addr, int *ok): the long at addr, in supervisor mode.  A bus
| error returns 0 with *ok 0 instead of reaching TOS.
	.globl	peek
peek:	moveal	%sp@(4),%a0
	movel	%a2,%sp@-
	movew	%sr,%sp@-
	oriw	#0x0700,%sr
	movec	%vbr,%a2
	movel	%a2@(8),%sp@-
	lea	%pc@(1f),%a1
	movel	%a1,%a2@(8)
	moveal	%sp@(18),%a1
	clrl	%a1@
	movel	%sp,%d1
	moveq	#0,%d0
	nop
	movel	%a0@,%d0
	nop
	addql	#1,%a1@
1:	movel	%d1,%sp
	movel	%sp@+,%a2@(8)
	movew	%sp@+,%sr
	movel	%sp@+,%a2
	rts
