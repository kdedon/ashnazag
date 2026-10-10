	.text
	.globl otr_sys_read,otr_sys_write,otr_sys_open,otr_sys_close,otr_sys_ioctl,otr_sys_select
otr_sys_read:
	moveq #3,%d0
	bra.s sys
otr_sys_write:
	moveq #4,%d0
	bra.s sys
otr_sys_open:
	moveq #5,%d0
	bra.s sys
otr_sys_close:
	moveq #6,%d0
	bra.s sys
otr_sys_ioctl:
	moveq #54,%d0
	bra.s sys
otr_sys_select:
	moveq #82,%d0
sys:
	trap #0
	bcc.s success
	neg.l %d0
success:
	rts
