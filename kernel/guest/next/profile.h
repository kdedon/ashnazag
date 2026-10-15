#ifndef NEXT_PROFILE_H
#define NEXT_PROFILE_H
#define NX_MAJOR 67
#define NX_ENTER 0x4e580001	/* struct nx_enter: the caller becomes the machine */
#define NX_INFO 0x4e580002	/* struct nx_info */
#define NX_ROM 0x00000000UL	/* the ROM, and again at NX_ROM2 */
#define NX_ROM2 0x01000000UL
#define NX_ROM_SIZE 0x20000UL
#define NX_IO 0x02000000UL
#define NX_RAM 0x04000000UL	/* bank 0; banks are 32 MB apart */
#define NX_BANK 0x02000000UL
#define NX_VRAM 0x0c000000UL
#define NX_VRAM_SIZE 0x40000UL
#define NX_DOORBELL 0x0201f000UL	/* a long store is a host call: the kernel leaving for the monitor, d0 = command line */
#define NX_LIMIT 0x20000000UL	/* local bus; above it nothing answers */
#define NXF_CENSUS 1		/* each register's first access on the console */
#define NX_NVRAM 64
#define NX_NCENSUS 96
struct nx_enter { unsigned long flags, ramsize; unsigned char nvram[NX_NVRAM]; };
struct nx_census { unsigned long addr, pc, value; unsigned char size, write, pad[2]; };
struct nx_info {
	unsigned long ticks, ints, io, faults, nvseq, level, stat, mask, ncensus, lasta, lastpc, monexit;
	unsigned char nvram[NX_NVRAM];
	struct nx_census census[NX_NCENSUS];
};
#endif
