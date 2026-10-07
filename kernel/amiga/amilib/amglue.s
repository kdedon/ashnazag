| amglue.s -- the AmigaOS register convention, in and out.
|
| am_entry      target of every stub "jsr am_entry" in our jump tables.
|               Frame for am_dispatch: d0-d7/a0-a6 (60 bytes), then the
|               stub's return address (stub + 6).  The handler edits the
|               frame; all fifteen registers come back from it, so the
|               caller sees AmigaOS's preserved registers unchanged and
|               the results the handler stored.  The CCR is set from d0,
|               for callers that branch on a call's result directly.
| am_call       long am_call(fn, unsigned long r[15]): d0-a6 from r, call
|               fn, store all fifteen back (RawDoFmt's PutChProc moves
|               a3, for one).  Returns d0 (also in a0, for callers
|               declared to return a pointer).
| am_super      long am_super(unsigned long r[15]): exec Supervisor().
|               A format-0 frame (SR, PC, 0) is pushed and r[a5] is
|               entered with r's registers; it leaves by rte.  The kernel
|               already runs in supervisor state.  Registers back into r
|               as for am_call.
| am_isr        is_Code for C interrupt servers: is_Data (a1) points to a
|               struct whose first long is int (*)(void *), called with
|               is_Data; Z from its result, as exec's server chains want.
| am_hook       h_Entry for C hooks: h_SubEntry is called as
|               long (*)(hook, object, message).

	.text

	.globl	am_entry
am_entry:
	moveml	&0xfffe,%sp@-		| d0-d7/a0-a6
	pea	%sp@
	jsr	am_dispatch
	addql	&4,%sp
	moveml	%sp@+,&0x7fff		| d0-d7/a0-a6
	addql	&4,%sp			| the stub's return
	tstl	%d0
	rts

	.globl	am_call
am_call:
	moveml	&0x3f3e,%sp@-		| d2-d7/a2-a6, 44 bytes
	moveal	%sp@(52),%a0		| r
	movel	%sp@(48),%d0		| fn
	movel	%a0,%sp@-		| keep r
	pea	Lcback
	movel	%d0,%sp@-
	moveml	%a0@,&0x7fff		| d0-d7/a0-a6 from r
	rts				| into fn, which returns to Lcback
Lcback:
	movel	%a0,%sp@-
	moveal	%sp@(4),%a0		| r
	moveml	&0x00ff,%a0@		| d0-d7
	moveml	&0x7e00,%a0@(36)	| a1-a6
	movel	%sp@+,%a0@(32)		| a0
	addql	&4,%sp
	moveml	%sp@+,&0x7cfc		| d2-d7/a2-a6
	moveal	%d0,%a0
	rts

	.globl	am_super
am_super:
	moveml	&0x3f3e,%sp@-		| d2-d7/a2-a6, 44 bytes
	moveal	%sp@(48),%a0		| r
	movel	%a0,%sp@-		| keep r
	clrw	%sp@-			| format 0, vector 0
	pea	Lsback			| PC
	movew	%sr,%sp@-		| SR
	movel	%a0@(52),%sp@-		| r[a5], entered by the rts below
	moveml	%a0@,&0x7fff		| d0-d7/a0-a6 from r
	rts
Lsback:					| its rte popped the frame
	movel	%a0,%sp@-
	moveal	%sp@(4),%a0		| r
	moveml	&0x00ff,%a0@		| d0-d7
	moveml	&0x7e00,%a0@(36)	| a1-a6
	movel	%sp@+,%a0@(32)		| a0
	addql	&4,%sp
	moveml	%sp@+,&0x7cfc		| d2-d7/a2-a6
	moveal	%d0,%a0
	rts

	.globl	am_isr
am_isr:
	movel	%a1,%sp@-
	moveal	%a1@,%a0
	jsr	%a0@
	addql	&4,%sp
	tstl	%d0
	rts

	.globl	am_hook
am_hook:
	movel	%a1,%sp@-		| message
	movel	%a2,%sp@-		| object
	movel	%a0,%sp@-		| hook
	moveal	%a0@(12),%a0		| h_SubEntry
	jsr	%a0@
	lea	%sp@(12),%sp
	rts
