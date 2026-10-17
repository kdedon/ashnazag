#ifndef NEXT_PROFILE_H
#define NEXT_PROFILE_H
#define NX_MAJOR 67
#define NX_ENTER 0x4e580001	/* struct nx_enter: the caller becomes the machine */
#define NX_INFO 0x4e580002	/* struct nx_info */
#define NX_ROM 0x00000000UL	/* the ROM, and again at NX_ROM2 */
#define NX_ROM2 0x01000000UL
#define NX_ROM_SIZE 0x20000UL
#define NX_IO 0x02000000UL	/* the board's registers */
/*
 * The I/O pages the kernel itself uses (interrupts, SCR1/2, the keyboard unit's, brightness, timer, event counter, the
 * turbo's memory controller and cache): memory holding constants, in the RAM object after the host
 * filesystem's region.  Devices' pages are absent: their accesses are bus errors.
 */
#define NX_WINDOWS { { 0x02007000UL, 0x1000UL }, { 0x0200c000UL, 0x3000UL }, { 0x02010000UL, 0x1000UL }, \
	{ 0x02016000UL, 0x1000UL }, { 0x0201a000UL, 0x1000UL }, { 0x02200000UL, 0x20000UL }, { 0x03e00000UL, 0x20000UL } }
#define NX_WINSIZE 0x47000UL
#define NX_RAM 0x04000000UL	/* bank 0; banks are 32 MB apart */
#define NX_BANK 0x02000000UL
#define NX_VRAM 0x0c000000UL
#define NX_VRAM_SIZE 0x100000UL
#define NX_LIMIT 0x20000000UL	/* local bus; above it nothing answers */
#define NX_TOP 0x3ffe0000UL	/* the guest's virtual space, the host's first gigabyte: its tasks load libraries up to 0x33000000 */
#define NX_PHYS 0xa0000000UL	/* physical address p is also at NX_PHYS + p, for the shadow MMU and the host side */
#define NX_RAMNEW 0x4e580003	/* unsigned long: the RAM object (the host filesystem region and the I/O window after it), mmap offsets from 0 */
#define NXF_CENSUS 1		/* count accesses to addresses nothing backs */
#define NXF_HOST 2		/* code above the bus is the host's own: its faults stay signals */
#define NX_NVRAM 64
#define NX_NCENSUS 96
struct nx_enter {
	unsigned long flags, ramsize;
	unsigned long width, height;	/* the screen, from the host's */
	unsigned char nvram[NX_NVRAM];
};
struct nx_census { unsigned long addr, pc, value; unsigned char size, write, pad[2]; unsigned long count, lastpc; };
struct nx_info {
	unsigned long ticks, ints, io, faults, nvseq, level, stat, mask, ncensus, lasta, lastpc, monexit;
	unsigned long mmu[5];	/* MMU fills, faults, space switches, flushes; privileged instructions */
	unsigned long utraps;	/* trap instructions from the guest's user mode */
	unsigned long utrap[16][8];	/* the latest: vector, d0, pc, URP, d5, three return addresses up the frame chain */
	unsigned long hcalls[16];	/* host calls by service */
	unsigned char nvram[NX_NVRAM];
	struct nx_census census[NX_NCENSUS];
};
#endif
