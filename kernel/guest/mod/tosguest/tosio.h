/*
 * tosio.h -- /dev/tos: an Atari TOS container.
 *
 * The launcher maps ST-RAM at 0, the ROM and the cartridge image, then
 * TOSIOC_ENTER makes it the guest; it jumps to the ROM's reset code.
 * The display process of the container reads the video state with
 * TOSIOC_VIDEO and posts keys and mouse motion with TOSIOC_INPUT.
 */

#ifndef _TOSIO_H
#define _TOSIO_H

#define	TOS_MAJOR	56
#define	TOS_SIG		17		/* SIGUSR2: carries virtual interrupts */

#define	TOSIOC(n)	(('T' << 8) | (n))
#define	TOSIOC_ENTER	TOSIOC(1)	/* in struct tosenter */
#define	TOSIOC_INPUT	TOSIOC(2)	/* in struct tosinput */
#define	TOSIOC_VIDEO	TOSIOC(3)	/* out struct tosvideo */
#define	TOSIOC_STAT	TOSIOC(4)	/* out struct tosstat */
#define	TOSIOC_OWNER	TOSIOC(5)	/* out struct tosowner: who runs the container */

struct tosenter {
	unsigned long	te_ramsize;	/* ST-RAM at 0 */
	unsigned long	te_flags;	/* TEF_* */
};

#define	TEF_MONO	0x01		/* monochrome monitor */

struct tosowner {
	long		to_pid;		/* 0: free */
	long		to_uid;
};

struct tosinput {
	int		ti_n;
	unsigned char	ti_b[60];	/* IKBD bytes */
	short		ti_dx, ti_dy;	/* mouse motion since the last post */
	short		ti_btn;		/* mouse buttons: 1 right, 2 left */
};

struct tosvideo {
	unsigned long	tv_gen;		/* bumped by every video register write */
	unsigned long	tv_base;	/* screen memory */
	unsigned short	tv_stmode;	/* $FF8260 */
	unsigned short	tv_ttmode;	/* $FF8262 */
	unsigned short	tv_stpal[16];	/* $FF8240 */
	unsigned short	tv_ttpal[256];	/* $FF8400 */
	unsigned long	tv_vbl;
};

struct tosstat {
	unsigned long	ts_pid;
	unsigned long	ts_ints[16];	/* MFP interrupts delivered, by channel */
	unsigned long	ts_vbl;		/* VBL interrupts delivered */
	unsigned long	ts_io;		/* emulated register accesses */
	unsigned long	ts_absent;	/* bus errors from absent registers */
	unsigned long	ts_berr;	/* other bus errors reflected */
	unsigned long	ts_refl[64];	/* exceptions reflected, by vector */
	unsigned long	ts_priv, ts_mmu;
	unsigned long	ts_kbin;	/* IKBD bytes queued */
	unsigned long	ts_kbrd;	/* IKBD bytes the guest read */
	unsigned long	ts_kbcmd;	/* IKBD command bytes */
	unsigned long	ts_lastpc;	/* guest PC at the last kernel entry */
	unsigned long	ts_lastio;	/* last unlisted address, and its PC */
	unsigned long	ts_lastiopc;
	unsigned long	ts_sys;		/* host system calls (trap #0) */
	unsigned long	ts_stop;	/* stop instructions (the guest idles) */
	unsigned long	ts_cache;	/* cache instructions and CACR writes */
	unsigned long	ts_slept;	/* clock ticks asleep in stop */
};

#endif	/* _TOSIO_H */
