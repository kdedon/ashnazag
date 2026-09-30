| macabi0.s -- start-up, A/UX system call stubs and the instruction
| sequences macabi.c runs as a Mac task: an A-line trap and its
| handler, an access fault and its handler, privileged instructions
| the kernel emulates.
|
| sys15(n, a0, d1, a1, d2, a2): trap #15, arguments in registers.
| sys0(n, args...): trap #0, arguments on the stack.
| Both return d0, or -1 with the A/UX errno in errno; d1 goes to sysd1.

	.text
	.globl	_start
_start:
	movel	%sp,%a0
	pea	%a0@(4)
	movel	%a0@,%sp@-
	jsr	main
	moveal	%d0,%a0
	moveq	&1,%d0
	trap	&15
	bra.b	_start

	.globl	sys15
sys15:
	moveml	&0x2020,%sp@-
	movel	%sp@(12),%d0
	moveal	%sp@(16),%a0
	movel	%sp@(20),%d1
	moveal	%sp@(24),%a1
	movel	%sp@(28),%d2
	moveal	%sp@(32),%a2
	trap	&15
	moveml	%sp@+,&0x0404
	bcs.b	Lerr
	movel	%d1,sysd1
	rts

	.globl	sys0
sys0:
	movel	%sp@(4),%d0
	movel	%sp@+,%sp@
	trap	&0
	bcc.b	L0ok
	movel	%sp@,%sp@-
	bra.b	Lerr
L0ok:
	movel	%sp@,%sp@-
	movel	%d1,sysd1
	rts
Lerr:
	movel	%d0,errno
	moveq	&-1,%d0
	rts

| A-line: the frame the kernel built (SR.w, PC.l, $0028) is recorded,
| then the trap word is skipped.
	.globl	aline_h
aline_h:
	movel	%sp@,la_w0
	movel	%sp@(4),la_w1
	movel	%sp,la_sp
	addql	&1,la_count
	moveal	%sp@(2),%a0
	addql	&8,%sp
	jmp	%a0@(2)

| aline_do(): CCR 0x15, one A-line trap; returns the sp before it
	.globl	aline_do
	.globl	aline_at
aline_do:
	movel	%sp,%d0
	movew	&0x15,%ccr
aline_at:
	.word	0xa123
	rts

| Access fault: the handler records the frame's format/vector word and
| fault address and SSW, drops the 60-byte frame and makes buserr_do
| or buserr_st return -1.
	.globl	buserr_h
buserr_h:
	clrl	be_fv
	movew	%sp@(6),be_fv+2
	movel	%sp@(8),be_ea
	clrl	be_ssw
	movew	%sp@(12),be_ssw+2
	addl	&60,%sp
	moveq	&-1,%d0
	rts

| buserr_do(addr): reads a long at addr; 0 when it could
	.globl	buserr_do
buserr_do:
	moveal	%sp@(4),%a0
	movel	%a0@,%d1
	moveq	&0,%d0
	rts

| buserr_st(addr): writes a long at addr; 0 when it could
	.globl	buserr_st
buserr_st:
	moveal	%sp@(4),%a0
	clrl	%a0@
	moveq	&0,%d0
	rts

| aline_n(n): n A-line traps
	.globl	aline_n
aline_n:
	movel	%sp@(4),%d1
	bra.b	Lan1
Lan0:
	.word	0xa000
Lan1:
	subql	&1,%d1
	bpl.b	Lan0
	rts

	.globl	get_sr
get_sr:
	moveq	&0,%d0
	.word	0x40c0			| move sr,d0
	rts

	.globl	ipl7
ipl7:
	.word	0x007c, 0x0700		| ori #$700,sr
	rts

	.globl	ipl0
ipl0:
	.word	0x027c, 0xf8ff		| andi #$f8ff,sr
	rts

	.globl	set_sr
set_sr:
	movel	%sp@(4),%d0
	.word	0x46c0			| move d0,sr
	rts

	.globl	eor_sr
eor_sr:
	movew	&0,%ccr
	.word	0x0a7c, 0x001f		| eori #$1f,sr
	.word	0x42c0			| move ccr,d0
	andil	&0x1f,%d0
	rts

| move sr,(a0)+ and move (a0),sr: memory operands
	.globl	sr_mem
sr_mem:
	moveal	%sp@(4),%a0
	.word	0x40d8			| move sr,(a0)+
	movel	%a0,%d0
	rts

	.globl	get_cacr
get_cacr:
	.word	0x4e7a, 0x0002		| movec cacr,d0
	rts

	.globl	set_cacr
set_cacr:
	movel	%sp@(4),%d0
	.word	0x4e7b, 0x0002		| movec d0,cacr
	rts

	.globl	get_vbr
get_vbr:
	moveq	&-1,%d0
	.word	0x4e7a, 0x0801		| movec vbr,d0
	rts

| usp_rt(v): move a0,usp; move usp,a1; returns a1
	.globl	usp_rt
usp_rt:
	moveal	%sp@(4),%a0
	.word	0x4e60			| move a0,usp
	subal	%a1,%a1
	.word	0x4e69			| move usp,a1
	movel	%a1,%d0
	rts

| do_rte(): a format-0 frame with SR $2004 back to Lrte; returns the CCR
	.globl	do_rte
do_rte:
	movew	&0,%sp@-		| format 0, vector 0
	pea	Lrte
	movew	&0x2004,%sp@-
	.word	0x4e73			| rte
Lrte:
	.word	0x42c0			| move ccr,d0
	andil	&0x1f,%d0
	rts

	.globl	do_cpush
do_cpush:
	.word	0xf4f8			| cpusha bc
	rts

	.globl	do_reset
do_reset:
	.word	0x4e70			| reset: not emulated
	rts

| reset at IPL 7: the SIGILL is not held
	.globl	do_reset7
do_reset7:
	.word	0x007c, 0x0700		| ori #$700,sr
	.word	0x4e70			| reset
	rts

| rte with format 7 (040 access error): refused
	.globl	do_rte7
do_rte7:
	movew	&0x7008,%sp@-
	pea	Lrte
	movew	&0x2000,%sp@-
	.word	0x4e73
	rts
