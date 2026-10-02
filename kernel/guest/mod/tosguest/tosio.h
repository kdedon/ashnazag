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
#define	TOSIOC_PAUSE	TOSIOC(6)	/* value: 1 the guest sleeps, 0 it runs */

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
	unsigned long	ts_idle;	/* input polls slept on */
};

/*
 * Input the cartridge hands straight to TOS's own handlers each VBL:
 * the last page of the cartridge, shared by the guest and the display
 * process.  pv_on says which events the guest takes here; the rest go
 * through the IKBD (TOSIOC_INPUT).
 */
#define	TOSPV		0xfbf000L
#define	PV_NEV		256

struct tospv {
	unsigned long	pv_on;		/* guest: PV_* */
	unsigned long	pv_head;	/* events posted */
	unsigned long	pv_tail;	/* events taken */
	unsigned long	pv_xy;		/* motion posted: x << 16 | y, 16-bit sums */
	unsigned long	pv_cxy;		/* motion taken */
	unsigned long	pv_btn;		/* buttons sent */
	unsigned long	pv_npkt;	/* mouse packets to mousevec */
	unsigned long	pv_nkey;	/* keys to kbdvec */
	struct {
		unsigned short	e_ev;	/* PE_* << 8 | IKBD code */
		unsigned short	e_pad;
		unsigned long	e_xy;	/* pv_xy when posted */
	}		pv_ev[PV_NEV];
	unsigned long	pv_vbl;		/* guest: VBLs run */
	unsigned long	pv_drop;	/* events before this went by the IKBD */
};

#define	PV_MOUSE	1
#define	PV_KEYS		2

#define	PE_KEY		1		/* scan code, $80 on release */
#define	PE_BTN		2		/* buttons: 1 right, 2 left */

#endif	/* _TOSIO_H */
