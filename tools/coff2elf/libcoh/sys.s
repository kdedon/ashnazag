# sys.s -- i386 Linux syscall stubs for libcoh-linux.
# The bottom of the Coherent libc retargeted to Linux: each C-callable entry traps
# to the host kernel via int 0x80 with the Linux i386 call number, so MWC-compiled
# code that calls write()/open()/... runs natively on the host.  cdecl: args on the
# stack, return in %eax.  Replaces the Coherent `sys N` stubs.
#
# BOOTSTRAP FORM: GNU as (AT&T) syntax; the self-hosted toolchain re-expresses these
# in the MWC i386 assembler (as386).

# sc3 name, nr: a stub taking up to 3 args, preserving %ebx (the 1st-arg register).
	.macro	sc3 name, nr
	.globl	\name
\name:
	pushl	%ebx
	movl	$\nr, %eax
	movl	8(%esp), %ebx		# arg1 (after pushed %ebx)
	movl	12(%esp), %ecx		# arg2
	movl	16(%esp), %edx		# arg3
	int	$0x80
	popl	%ebx
	ret
	.endm

	.text
	sc3	read,   3
	sc3	write,  4
	sc3	open,   5
	sc3	close,  6
	sc3	creat,  8
	sc3	unlink, 10
	sc3	lseek,  19
	sc3	_lbrk,  45		# Linux brk; _lbrk(addr) -> resulting break

	.globl	_exit			# _exit(code)
_exit:
	movl	4(%esp), %ebx
	movl	$1, %eax		# __NR_exit
	int	$0x80
# end of sys.s
