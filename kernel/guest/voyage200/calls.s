	.data
	.balign 4
v_depth:
	.long 0
v_stacks:
	.space 64
	.text
	.globl v_call
v_call:
	moveml %d2-%d7/%a2-%a6,%sp@-
	movel %sp@(48),%a0
	movel %sp@(52),%a1
	movel %sp@(56),%d1
	movel %sp,%a2
	movel &0x3ffc0,%sp
	bras v_enter
	.globl v_raw
v_raw:
	moveml %d2-%d7/%a2-%a6,%sp@-
	movel %sp@(48),%a0
	movel %sp@(52),%a1
	movel %sp@(56),%d1
	movel %sp,%a2
v_enter:
	movel v_depth,%d0
	cmpil &16,%d0
	bccs v_overflow
	leal v_stacks,%a3
	movel %a2,%a3@(0,%d0:l:4)
	addql &1,v_depth
	subl %d1,%sp
	movel %sp,%a3
	tstl %d1
	beqs v_ready
v_copy:
	moveb %a1@+,%a3@+
	subql &1,%d1
	bnes v_copy
v_ready:
	jsr %a0@
	subql &1,v_depth
	movel v_depth,%d1
	leal v_stacks,%a0
	movel %a0@(0,%d1:l:4),%sp
	moveml %sp@+,%d2-%d7/%a2-%a6
	rts
v_overflow:
	movel %a2,%sp
	moveq &0,%d0
	moveml %sp@+,%d2-%d7/%a2-%a6
	rts
	.globl v_idle
v_idle:
	movew %ccr,%sp@-
	moveml %d0-%d7/%a0-%a6,%sp@-
	jsr v_wait
	moveml %sp@+,%d0-%d7/%a0-%a6
	movew %sp@+,%ccr
	rts
