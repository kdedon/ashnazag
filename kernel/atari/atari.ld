/* Fully linked Falcon kernel: identity-mapped, loaded at PA 0x1000. */
OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(ata_boot)

PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0x1000;
	.text : { *ataboot.o(.text) *(.text) . = ALIGN(4); } :text
	etext = .;
	. = ALIGN(16);
	.data : { *(.data) } :data
	edata = .;
	.bss : { *(.bss) *(COMMON) } :data
	. = ALIGN(4);
	end = .;
	/DISCARD/ : { *(.comment) *(.note*) }
}
