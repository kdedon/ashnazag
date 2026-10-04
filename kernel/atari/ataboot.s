| ataboot.s -- first bytes of the linked image: the loader jumps to the
| load address, not to the ELF entry.
	.text
	.globl	ata_boot
ata_boot:
	jmp	atari_entry
