/* Fully linked Mac kernel: identity-mapped, loaded at PA 0x10000. */
OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(mac_entry)

PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0x10000;
	.text : { *(.text) . = ALIGN(4); } :text	/* launch: sections back to back */
	etext = .;
	. = ALIGN(16);
	.data : { *(.data) } :data
	edata = .;
	.bss : { *(.bss) *(COMMON) } :data
	. = ALIGN(4);
	end = .;
	/DISCARD/ : { *(.comment) *(.note*) }
}
