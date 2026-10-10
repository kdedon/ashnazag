	.section .a5init,"ax"
	.globl __aslm_entry
__aslm_entry:
	moveml %d0-%d7/%a0-%a6,-(%sp)
	lea -64(%sp),%sp
	movel 128(%sp),%d0
	lea prefix(%pc),%a0
	movel %sp,%a1
1:	moveb (%a0)+,%d1
	beqs 2f
	moveb %d1,(%a1)+
	bras 1b
2:	lea digits(%pc),%a2
	moveq #7,%d2
3:	movel %d0,%d1
	lsrl #8,%d1
	lsrl #8,%d1
	lsrl #8,%d1
	lsrl #4,%d1
	moveb (%a2,%d1.l),(%a1)+
	lsll #4,%d0
	dbra %d2,3b
	moveb #10,(%a1)+
	movel %sp,%a0
	movel %a1,%d0
	subl %a0,%d0
	movel %d0,-(%sp)
	movel %a0,-(%sp)
	pea 1
	clrl -(%sp)
	moveq #4,%d0
	trap #0
	lea 80(%sp),%sp
	moveml (%sp)+,%d0-%d7/%a0-%a6
	bra __aslm_entry_impl
prefix:
	.asciz "otrelay I ASLM selector "
digits:
	.ascii "0123456789abcdef"
	.even
