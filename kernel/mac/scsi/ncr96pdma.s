| ncr96pdma.s -- 53C96 pseudo-DMA chunks that rely on the bus-cycle stall.
|
| int ncr_blind(char *buf, int writing)
|
| Moves one 256-byte chunk through the chip's DMA port, as A/UX does:
| wait for DREQ, move one word, wait for DREQ, then move the other 127
| words back to back; the glue stalls each cycle until the FIFO has data
| (reads) or room (writes).  The caller has loaded TC = 256 and issued
| DMA + transfer information.
|
| Returns 256 when the chunk moved; -1 when DREQ did not come (the chip
| interrupted first, or the wait timed out); -2 on a bus error.  After
| -1 or -2 the caller takes the position from the chip's counters.
|
| A stalled cycle ends in a bus error.  k_trap then finds u_nofault
| (u+0x374) armed, discards the access-fault frame (a pending write is
| not replayed) and resumes at the pad with the registers and SP of the
| faulting instruction.  The pad checks that SP to be sure the fault was
| ours and not one taken by an interrupt handler nested inside the loop.
| The outer u_nofault is kept: this may run in an interrupt taken inside
| copyin/copyout.

	.set	DREQWAIT,1000000
	.data
	.align	2
ncr_padsp:
	.long	0
	.text
Lbl_foreign:
	.asciz	"ncr96: bus error in a nested handler"
	.align	2
	.globl	ncr_blind
ncr_blind:
	linkw	%fp,&0
	movel	u+0x374,%sp@-		| fp-4: outer u_nofault
	moveml	%d2/%a2,%sp@-		| fp-12: d2 a2
	moveal	%fp@(8),%a1		| buffer
	moveal	&0x50f10100,%a0		| DMA port
	moveal	&0x50f03a00,%a2		| DREQ
	movel	%sp,ncr_padsp
	movel	&Lbl_fault,u+0x374
	movel	&DREQWAIT,%d2
Lbl_w0:
	movel	%a2@,%d0
	btst	&0,%d0
	bnew	Lbl_g0
	btst	&7,0x50f10040		| chip interrupt: phase change
	bnew	Lbl_nodreq
	subql	&1,%d2
	bnew	Lbl_w0
	braw	Lbl_nodreq
Lbl_g0:
	tstl	%fp@(12)
	bnew	Lbl_out
| read: one word, DREQ, 127 words
	movew	%a0@,%a1@+
	movel	&DREQWAIT,%d2
Lbl_w1:
	movel	%a2@,%d0
	btst	&0,%d0
	bnew	Lbl_g1
	btst	&7,0x50f10040		| chip interrupt: phase change
	bnew	Lbl_nodreq
	subql	&1,%d2
	bnew	Lbl_w1
	braw	Lbl_nodreq
Lbl_g1:
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	moveq	&14,%d1			| 15 x 8 = 120 more
Lbl_in8:
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	dbra	%d1,Lbl_in8
	braw	Lbl_ok
| write: one word, DREQ, 127 words
Lbl_out:
	movew	%a1@+,%a0@
	movel	&DREQWAIT,%d2
Lbl_w2:
	movel	%a2@,%d0
	btst	&0,%d0
	bnew	Lbl_g2
	btst	&7,0x50f10040		| chip interrupt: phase change
	bnew	Lbl_nodreq
	subql	&1,%d2
	bnew	Lbl_w2
	braw	Lbl_nodreq
Lbl_g2:
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	moveq	&14,%d1
Lbl_out8:
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	dbra	%d1,Lbl_out8
Lbl_ok:
	movel	&256,%d0
	braw	Lbl_ret
Lbl_nodreq:
	moveq	&-1,%d0
Lbl_ret:
	movel	%fp@(-4),u+0x374
	moveml	%fp@(-12),%d2/%a2
	unlk	%fp
	rts

| DREQ is bit 0 of a long read at a2; the waits are inline so that SP
| stays what the pad expects.

| Bus error: registers and SP as at the faulting instruction.
Lbl_fault:
	cmpl	ncr_padsp,%sp
	bnew	Lbl_notours
	movel	%fp@(-4),u+0x374
	moveq	&-2,%d0
	moveml	%fp@(-12),%d2/%a2
	unlk	%fp
	rts
Lbl_notours:
	movel	&Lbl_foreign,%sp@-
	jsr	panic

| int ncr_xfer(char **pp, uint n, int writing)
|
| Moves n bytes (even) for a whole-phase transfer command: wait for
| DREQ, then back-to-back words to the end.  Returns 0 when all moved,
| -1 when DREQ did not come (chip interrupt or timeout), -2 on a bus
| error.  *pp is left at the next word: exact on reads, since the
| faulting read has not stored; on writes the 040 reports the fault
| after later stores, so the caller takes the position from the chip.

	.globl	ncr_xfer
ncr_xfer:
	linkw	%fp,&0
	movel	u+0x374,%sp@-		| fp-4: outer u_nofault
	moveml	%d2/%a2,%sp@-		| fp-12: d2 a2
	moveal	%fp@(8),%a1
	moveal	%a1@,%a1		| buffer
	moveal	&0x50f10100,%a0		| DMA port
	moveal	&0x50f03a00,%a2		| DREQ
	movel	%sp,ncr_padsp
	movel	&Lx_fault,u+0x374
	movel	&DREQWAIT,%d2
Lx_w:
	movel	%a2@,%d0
	btst	&0,%d0
	bnew	Lx_go
	btst	&7,0x50f10040		| chip interrupt: phase change
	bnew	Lx_nodreq
	subql	&1,%d2
	bnew	Lx_w
	braw	Lx_nodreq
Lx_go:
	movel	%fp@(12),%d1
	lsrl	&1,%d1
	moveq	&15,%d0
	andl	%d1,%d0			| odd words first
	lsrl	&4,%d1			| then blocks of 16
	tstl	%fp@(16)
	bnew	Lx_out1e
	braw	Lx_in1e
Lx_in1:
	movew	%a0@,%a1@+
Lx_in1e:
	dbra	%d0,Lx_in1
	braw	Lx_in16e
Lx_in16:
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
	movew	%a0@,%a1@+
Lx_in16e:
	dbra	%d1,Lx_in16
	braw	Lx_ok
Lx_out1:
	movew	%a1@+,%a0@
Lx_out1e:
	dbra	%d0,Lx_out1
	braw	Lx_out16e
Lx_out16:
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
	movew	%a1@+,%a0@
Lx_out16e:
	dbra	%d1,Lx_out16
Lx_ok:
	moveq	&0,%d0
	braw	Lx_ret
Lx_nodreq:
	moveq	&-1,%d0
Lx_ret:
	moveal	%fp@(8),%a0
	movel	%a1,%a0@
	movel	%fp@(-4),u+0x374
	moveml	%fp@(-12),%d2/%a2
	unlk	%fp
	rts

| Bus error: registers and SP as at the faulting instruction.
Lx_fault:
	cmpl	ncr_padsp,%sp
	bnew	Lbl_notours
	moveq	&-2,%d0
	braw	Lx_ret
