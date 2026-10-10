| Guest processes hold guestcore through their profile dependency.
| Push the hook target to preserve every register and the CPU frame.

	.text
	.globl	guest_gate_c
guest_gate_c:
	tstl	guest_gate_c_hook
	beqw	guest_gate_missing
	movel	guest_gate_c_hook,%sp@-
	rts

	.globl	guest_linea
guest_linea:
	tstl	guest_linea_hook
	beqw	guest_gate_missing
	movel	guest_linea_hook,%sp@-
	rts

	.globl	guest_ftrap
guest_ftrap:
	tstl	guest_ftrap_hook
	beqw	guest_gate_missing
	movel	guest_ftrap_hook,%sp@-
	rts

	.globl	guest_fpriv
guest_fpriv:
	tstl	guest_fpriv_hook
	beqw	guest_gate_missing
	movel	guest_fpriv_hook,%sp@-
	rts

guest_gate_missing:
	movel	%a0,%sp@-
	movel	%d0,%sp@-
	movew	%sp@(14),%d0
	andiw	&0x0ffc,%d0
	lea	guest_chain,%a0
	movel	%a0@(0,%d0:w),%sp@-
	movel	%sp@(4),%d0
	movel	%sp@(8),%a0
	movel	%sp@,%sp@(8)
	lea	%sp@(8),%sp
	rts
