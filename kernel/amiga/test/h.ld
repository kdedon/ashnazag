/* the harness image: flat, from 0x10000 (run.py loads it there) */
SECTIONS
{
	. = 0x10000;
	.text : { *(.text.start) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
	_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
