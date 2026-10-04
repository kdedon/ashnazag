#ifndef _AMIGAIO_H
#define _AMIGAIO_H

#define AMIGA_MAJOR 57
#define AMIGA_ABI_VERSION 1
#define AMIGA_CHIP_SIZE 0x200000UL
#define AMIGA_FAST_BASE 0x08000000UL
#define AMIGA_FAST_MAX 0x08000000UL
#define AMIGA_ROM_BASE 0x00f80000UL
#define AMIGA_ROM_SIZE 0x80000UL
#define AMIGAF_PAL 1
#define AMIGAF_CENSUS 2
#define AMIGA_FEAT_BOOT 1
#define AMIGA_FEAT_BASE 2
#define AMIGA_FEAT_EXPERIMENTAL 4
#define AMIGAIOC(n) (('A' << 8) | (n))
#define AMIGAIOC_ENTER AMIGAIOC(1)
#define AMIGAIOC_LEAVE AMIGAIOC(2)
#define AMIGAIOC_STAT AMIGAIOC(3)
#define AMIGAIOC_INFO AMIGAIOC(4)

struct amigaenter {
	unsigned long ae_version, ae_chipsize, ae_fastsize, ae_flags;
};
struct amigainfo {
	unsigned long ai_version, ai_features;
};
struct amigastat {
	unsigned long as_version, as_pid, as_priv, as_fault, as_intr;
	unsigned long as_lastpc, as_lastaddr, as_stop;
};
#endif
