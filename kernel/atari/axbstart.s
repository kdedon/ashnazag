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
1:	.word	0xf4f8			| cpusha %bc
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
