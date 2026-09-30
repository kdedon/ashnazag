ENTRY(_start)
SECTIONS
{
	.text 0x10a8 : { *(.text) *(.rodata) *(.rodata1) *(.init) *(.fini) }
	.data 0x400000 : { *(.data) *(.data1) *(.sdata) }
	.bss : { *(.sbss) *(.bss) *(COMMON) }
	/DISCARD/ : { *(.comment) *(.note) }
}
