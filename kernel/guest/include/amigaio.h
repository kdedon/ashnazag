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
/*
 * AMIGAF_PV: the loaded Kickstart keeps some hardware state in guest
 * memory at AMIGA_PV_BASE, so its hot paths need no trap:
 *   +0 word  INTENA master: $C000 enabled, $4000 disabled
 *   +2 word  nonzero while the module holds an interrupt for the master bit
 *   +4 byte  CIA-A timer B high, +5 low, as of the last trap
 *   +6 byte  reads left before Kickstart reads the real timer again
 */
#define AMIGAF_PV 4
#define AMIGA_PV_BASE 0x20001f00UL
#define AMIGA_FEAT_BOOT 1
#define AMIGA_FEAT_BASE 2
#define AMIGA_FEAT_EXPERIMENTAL 4
/*
 * AMIGAIOC_KICK(pid) raises the guest's INTREQ PORTS; AMIGAIOC_WAIT(pid)
 * sleeps until the guest writes its doorbell (ESRCH: no such guest).
 */
#define AMIGA_FEAT_KICK 8
/* AMIGAIOC_SNDWAIT(pid): AMIGAIOC_WAIT for the sound doorbell */
#define AMIGA_FEAT_SNDBELL 16
#define AMIGAIOC(n) (('A' << 8) | (n))
#define AMIGAIOC_ENTER AMIGAIOC(1)
#define AMIGAIOC_LEAVE AMIGAIOC(2)
#define AMIGAIOC_STAT AMIGAIOC(3)
#define AMIGAIOC_INFO AMIGAIOC(4)
#define AMIGAIOC_KICK AMIGAIOC(5)
#define AMIGAIOC_WAIT AMIGAIOC(6)
#define AMIGAIOC_MAPROM AMIGAIOC(7)	/* the machine's Kickstart, read-only at AMIGA_ROM_BASE */
#define AMIGAIOC_HALT AMIGAIOC(8)	/* the guest: halt the machine, root only */
/* AMIGAIOC_WAIT for several helpers: each keeps its own count of rings heard */
#define AMIGAIOC_WAITN AMIGAIOC(9)	/* in/out struct amigawait */
#define AMIGAIOC_GSTAT AMIGAIOC(10)	/* in aw_pid, out struct amigastat of that guest */
#define AMIGAIOC_SNDWAIT AMIGAIOC(11)

struct amigaenter {
	unsigned long ae_version, ae_chipsize, ae_fastsize, ae_flags;
};
struct amigawait {
	unsigned long aw_pid, aw_heard;
	struct amigastat *aw_stat;
	unsigned long aw_flags;		/* AMIGAW_* */
};
/* this waiter answers the guest's requests: a request rung with 1 blocks the guest until it waits again */
#define AMIGAW_ANSWER 1
struct amigainfo {
	unsigned long ai_version, ai_features;
};
struct amigastat {
	unsigned long as_version, as_pid, as_priv, as_fault, as_intr;
	unsigned long as_lastpc, as_lastaddr, as_stop;
};
#endif
