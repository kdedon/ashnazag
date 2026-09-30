# crt0.s -- i386 Linux C startup for the x86-target self-host (libcoh-linux).
# The minimal "own OS interface": _start sets up the call to main and turns
# main's return value into a Linux exit(2).  argc/argv/envp are on the stack
# per the Linux i386 SysV entry; we pass argc + &argv to main (cdecl).
#
# BOOTSTRAP FORM: GNU as (AT&T) syntax -- assembled by the host toolchain while
# bringing the x86 target up.  The self-hosted toolchain will re-express this in
# the MWC i386 assembler syntax (`/` comments, intel-ish), built by `as386`.
	.text
	.globl	_start
_start:
	movl	(%esp), %eax		# argc
	leal	4(%esp), %edx		# argv
	pushl	%edx			# main(argc, argv)
	pushl	%eax
	call	main
	addl	$8, %esp
	pushl	%eax			# exit(main's return) -- flushes stdio
	call	exit			# (via libc exit -> _finish -> _exit)
	movl	$1, %eax		# safety: if exit ever returns, _exit
	movl	$0, %ebx
	int	$0x80
# end of crt0.s
