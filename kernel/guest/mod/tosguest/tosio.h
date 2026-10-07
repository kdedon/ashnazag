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
#define	TOSIOC_SOCK	TOSIOC(7)	/* in/out struct tossock: the guest's sockets */
#define	TOSIOC_MAPROM	TOSIOC(8)	/* the machine's ROM, read-only at TOS_ROMBASE; returns its size */
#define	TOSIOC_HALT	TOSIOC(9)	/* the guest: halt the machine, root only */
#define	TOSIOC_SND	TOSIOC(10)	/* in/out struct tossndio: the sound pump */
#define	TOSIOC_STRAM	TOSIOC(11)	/* value: bytes of contiguous ST-RAM, then mmap at 0 */

#define	TOS_ROMBASE	0xe00000L

struct tosenter {
	unsigned long	te_ramsize;	/* ST-RAM at 0 */
	unsigned long	te_flags;	/* TEF_* */
	unsigned short	te_w, te_h;	/* TEF_FALCON: the host screen, TOS's boot mode */
	unsigned short	te_depth, te_pad;
};

#define	TEF_MONO	0x01		/* monochrome monitor */
#define	TEF_NOMACH	0x02		/* a lone program: no machine, ST-RAM is the caller's */
#define	TEF_FALCON	0x04		/* a Falcon: Videl, its palette, sound and DSP registers */

struct tosowner {
	long		to_pid;		/* 0: free */
	long		to_uid;
};

/*
 * A BSD socket call by the container itself, on its own descriptors:
 * AF_INET, SVR4 errnos, nonblocking as the descriptor's O_NDELAY says.
 */
struct tossock {
	long		so_op;		/* TSO_* */
	long		so_fd;
	long		so_arg;		/* type, backlog, flags or peer */
	char		*so_buf;
	long		so_len;
	long		so_rv;		/* descriptor, count or connect state */
	char		so_addr[16];	/* sockaddr_in, in or out */
	long		so_alen;	/* 0: no address in */
	char		*so_gap;	/* 576 bytes of scratch */
};

#define	TSO_SOCKET	1		/* so_arg type -> so_rv descriptor */
#define	TSO_BIND	2
#define	TSO_CONNECT	3
#define	TSO_LISTEN	4		/* so_arg backlog */
#define	TSO_ACCEPT	5		/* -> so_rv descriptor, so_addr */
#define	TSO_SEND	6		/* so_buf, so_len, so_arg flags -> so_rv */
#define	TSO_RECV	7		/* so_buf, so_len, so_arg flags -> so_rv, so_addr */
#define	TSO_CONNWAIT	8		/* -> so_rv: -1 pending, else the connect's errno */
#define	TSO_NAME	9		/* so_arg 1 peer, 0 local -> so_addr */
#define	TSO_GETOPT	10		/* so_arg level << 16 | name, so_buf, so_len -> so_rv length */
#define	TSO_SETOPT	11		/* so_arg level << 16 | name, so_buf, so_len */
#define	TSO_SHUTDOWN	12		/* so_arg how */

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
	unsigned long	tv_vblheld;	/* ts_vblheld */
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
	unsigned long	ts_vblheld;	/* ticks the guest ran with a VBL masked */
	unsigned long	ts_held;	/* requests held at an emulation tail */
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
	unsigned long	pv_la;		/* guest: its Line A base */
};

#define	PV_MOUSE	1
#define	PV_KEYS		2

#define	PE_KEY		1		/* scan code, $80 on release */
#define	PE_BTN		2		/* buttons: 1 right, 2 left */

/*
 * The sound pump (TOSIOC_SND): signals buffer ends and serves the STE
 * DMA sound registers, a page of memory at $FF8900.  The caller becomes
 * the pump: a control register write sends it SIGUSR1.
 */
struct tossndio {
	unsigned long	sn_ends;	/* in: a buffer ended, TSE_* inputs */
	long		sn_ctl;		/* in: new $FF8901, -1 none */
	unsigned long	sn_pos;		/* in: the frame counter $FF8909, 0 none */
	unsigned long	sn_gen;		/* out: control register writes */
	unsigned char	sn_reg[0x40];	/* out: $FF8900-$FF893F */
};

#define	TSE_TIMERA	1		/* Timer A's event input */
#define	TSE_GPIP7	2		/* MFP input 7 */

/*
 * Falcon XBIOS sound state, kept by the cartridge's calls and played by
 * the pump; shared like struct tospv, in the same page.
 */
#define	TOSSND		(TOSPV + 0xc00)

struct tossnd {
	unsigned long	sd_gen;		/* guest: bumped after each change */
	unsigned long	sd_ctl;		/* guest: Buffoper's SB_* */
	unsigned long	sd_beg, sd_end;	/* guest: Setbuffer's play buffer */
	unsigned long	sd_mode;	/* guest: Setmode, 0 stereo 8, 1 stereo 16, 2 mono 8 */
	unsigned long	sd_tracks;	/* guest: play tracks - 1 */
	unsigned long	sd_mon;		/* guest: the track heard */
	unsigned long	sd_rate;	/* guest: 16.16 Hz */
	unsigned long	sd_irq;		/* guest: TSE_* raised at each buffer end */
	unsigned long	sd_seen;	/* pump: the sd_gen acted on */
	unsigned long	sd_play;	/* pump: playing */
	unsigned long	sd_pos;		/* pump: the address playing now */
	unsigned long	sd_ends;	/* pump: buffer ends so far */
	long		sd_bell;	/* the pump's doorbell, a descriptor; -1 none */
};

#define	SB_PLAY		1
#define	SB_REPEAT	2

#endif	/* _TOSIO_H */
