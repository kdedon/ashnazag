OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(stext)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS
{
	. = 0;
	.text : { *(.text) . = ALIGN(4); } :text
	etext = .;
	.data : { *(.data) } :data
	edata = .;
	.bss : { *(.bss) *(COMMON) } :data
	. = ALIGN(4);
	end = .;
	/DISCARD/ : { *(.comment) *(.note*) }
}
