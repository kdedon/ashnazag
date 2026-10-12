	.text
	.globl nh_native_begin
nh_native_begin:
	movel &0x4e480001,%d0
	trap &13
	rts
	.globl nh_native_end
nh_native_end:
	movel &0x4e480002,%d0
	trap &13
	rts
	.globl nh_native_depth
nh_native_depth:
	movel &0x4e480003,%d0
	trap &13
	rts
