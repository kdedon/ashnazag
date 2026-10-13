#ifndef VOYAGE_PROFILE_H
#define VOYAGE_PROFILE_H
#define TI_MAJOR 65
#define TI_ENTER 0x544e0001	/* struct ti_enter: the caller becomes the calculator */
#define TI_INFO 0x544e0003	/* struct ti_info */
#define TI_KEYS 0x544e0006	/* struct ti_keys */
#define TI_RAM_SIZE 0x40000UL	/* mirrored up to TI_FLASH */
#define TI_FLASH 0x200000UL
#define TI_FLASH_SIZE 0x400000UL
#define TI_IO 0x600000UL	/* $600000 and $700000 register pages */
#define TI_IO_END 0x800000UL
#define TI_ALIAS 0x10200000UL	/* Flash again, writable: program and erase land here */
#define TIF_CENSUS 1		/* each register's first access on the console */
struct ti_enter { unsigned long flags; };
struct ti_info {
	unsigned long lcd, ticks, io, flash, ints, faults;
	unsigned char contrast, pad[3];
};
/* rows[n] bit m: key at row n, column m is down */
struct ti_keys { unsigned char rows[10], on, pad; };
#endif
