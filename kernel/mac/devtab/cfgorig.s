| The port's config_cachefix ends in "jmp config_orig", the base's own
| name for its Amiga config.  Send it to the Mac config instead.
	.text
	.globl	config_orig
config_orig:
	jmp	config
