#ifndef VOYAGE_PROFILE_H
#define VOYAGE_PROFILE_H
#define TI_MAJOR 65
#define TI_ENTER 0x544e0001	/* struct ti_enter: the caller becomes the calculator */
#define TI_INFO 0x544e0003	/* struct ti_info */
#define TI_KEYS 0x544e0006	/* struct ti_keys */
#define TI_RAM_SIZE 0x40000UL	/* mirrored up to TI_FLASH */
#define TI_FLASH 0x200000UL
#define TI_FLASH_SIZE 0x400000UL
#define TI_IO 0x600000UL	/* $600000 and $700000 register pages: memory */
#define TI_OPEN 0xc00000UL	/* open bus AMS posts keys to: memory */
/* the pages above Flash the host maps */
#define TI_MAPPED(a) (((a) & ~0xfffUL) == TI_IO || ((a) & ~0xfffUL) == TI_IO + 0x100000UL || \
	((a) & ~0xfffUL) == TI_OPEN || ((a) & ~0xfffUL) == TI_HOST)
#define TI_HOST 0x1000000UL	/* service stubs: illegal; rts, 8 bytes apart */
#define TI_HOST_SIZE 0x1000UL
#define TI_ALIAS 0x10200000UL	/* Flash again, writable: the services program and erase here */
#define TIF_CENSUS 1		/* interrupts and exceptions on the console */
#define TI_LCD 0x4c00UL
/* services, by stub */
#define TS_SCAN 1		/* the key scan: AMS's key position word, d0 */
#define TS_MODS 2		/* the modifier keys: d0 */
#define TS_ROWS 3		/* all rows, pressed bits, into the buffer at 4(sp) */
#define TS_HALT 4		/* sleep until an interrupt of the mask in d0 */
#define TS_PROG 5		/* store d4 words from a2 at a3 */
#define TS_ERASE 6		/* free the block at 4(sp) */
#define TS_SIZE 7		/* the Flash size probe */
#define TS_WRITE 8		/* store 12(sp) bytes from 4(sp) at 8(sp) */
#define TS_MOVE 9		/* the block at 4(sp) to the block in a1, then free it */
#define TS_DARK 10		/* LCD off */
#define TS_LIT 11		/* LCD on */
#define TS_N 12
struct ti_enter {
	unsigned long flags;
	unsigned long scanout;	/* where AMS keeps the key position word */
	unsigned long modtab;	/* AMS's word table: row 0 modifiers to key position */
};
struct ti_info {
	unsigned long lcd, ticks, io, flash, ints, faults;
	unsigned char contrast, dark, pad[2];	/* dark: AMS turned the LCD off */
};
/* rows[n] bit m: key at row n, column m is down */
struct ti_keys { unsigned char rows[10], on, pad; };
#endif
