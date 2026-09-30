| basestock.s -- stands in for the 040 layer when the base is the stock kernel.
	.data
	.globl	kptr040
	.globl	kroot040
	.globl	mac_vmready
kptr040:	.long	0
kroot040:	.long	0
mac_vmready:	.long	0
	.balign	4
