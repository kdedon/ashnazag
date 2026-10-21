#ifndef MIG_HOSTCALL_H
#define MIG_HOSTCALL_H
/*
 * Host calls: subroutines in the boot extension's last page, 8 bytes
 * apart; each traps to the host and returns with every register kept.
 */
#define MIG_HOST_PAGE 0x00f7f080UL
#define MIG_HOST_REQUEST (MIG_HOST_PAGE + 0)	/* a SYS: request: back once answered */
#define MIG_HOST_RING (MIG_HOST_PAGE + 8)	/* wake the helpers */
#define MIG_HOST_SOUND (MIG_HOST_PAGE + 16)	/* wake the sound helper */
/*
 * Patches for code loaded from disk, written by the host when it patched
 * the Kickstart the same way: n, n words found, n words put in their place,
 * ..., then n = 0.  Absent without the magic.
 */
#define MIG_PVSEG 0x00f7f200UL
#define MIG_PVVEC 0x00f7ef00UL	/* the boot extension's line, rectangle, BltClear entries */
#define MIG_PVSEG_MAGIC 0x50565347UL
#define MIG_PVSEG_MAX 0x200
#define MIG_HOSTCALL(a) ((void (*)(void))(a))()
#endif
