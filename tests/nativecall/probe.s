	.text
	.globl ncf_private
ncf_private:
	movel &0x76543210,%d0
	trap &0
	rts
