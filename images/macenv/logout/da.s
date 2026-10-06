| Log Out, a desk accessory: Open ends the session by _AUXDispatch(12, nil).
	.text
	.short	0x4000, 0, 0, 0		| dNeedLock
	.short	open, done, done, done, done
	.byte	8
	.ascii	"\0Log Out"
	.even
open:	subq.l	#4,%sp
	move.w	#12,-(%sp)
	clr.l	-(%sp)
	.short	0xabf9
	addq.l	#4,%sp
done:	moveq	#0,%d0
	rts
