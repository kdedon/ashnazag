	.text
	.globl lisa_start
lisa_start:
	movel %sp@(4),%a0
	movel %sp@(8),%a1
	movel %a1,%sp
	movel %a1,%a5
	leal %a1@(-4),%a6
	clrl %d0
	clrl %d1
	clrl %d2
	clrl %d3
	clrl %d4
	clrl %d5
	clrl %d6
	clrl %d7
	jmp %a0@
	.globl lisa_service_entry
lisa_service_entry:
	movel %d2,%sp@-
	movel %d1,%sp@-
	movel %d0,%sp@-
	jsr lisa_dispatch
	movel &0x4c4e0010,%d0
	trap &13
	illegal
	.globl lisa_signal_return
lisa_signal_return:
	jsr lisa_default_terminate
	illegal
