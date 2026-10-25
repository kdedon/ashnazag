/*
 * tosio.h -- /dev/tos: an Atari TOS container.
 *
 * The launcher maps ST-RAM at 0, the I/O pages, the ROM and the
 * cartridge image, then TOSIOC_ENTER makes it the guest; it jumps to
 * the ROM's reset code.
 */

#ifndef _TOSIO_H
#define _TOSIO_H

#define	TOS_MAJOR	56
#define	TOS_SIG		17		/* SIGUSR2: carries virtual interrupts */

#define	TOSIOC(n)	(('T' << 8) | (n))
#define	TOSIOC_ENTER	TOSIOC(1)	/* in struct tosenter */
#define	TOSIOC_VIDEO	TOSIOC(3)	/* out struct tosvideo */
#define	TOSIOC_STAT	TOSIOC(4)	/* out struct tosstat */
#define	TOSIOC_OWNER	TOSIOC(5)	/* out struct tosowner: who runs the container */
#define	TOSIOC_PAUSE	TOSIOC(6)	/* value: 1 the guest sleeps, 0 it runs */
#define	TOSIOC_SOCK	TOSIOC(7)	/* in/out struct tossock: the guest's sockets */
#define	TOSIOC_HALT	TOSIOC(9)	/* the guest: halt the machine, root only */
#define	TOSIOC_SND	TOSIOC(10)	/* in/out struct tossndio: the sound pump */
#define TOSIOC_RAM	TOSIOC(14)	/* value: bytes of scattered, fault-allocated RAM and I/O pages */
#define	TOSIOC_STRAM	TOSIOC(11)	/* value: bytes of contiguous ST-RAM, then mmap at 0 */
#define	TOSIOC_TIMER	TOSIOC(16)	/* value: Xbtimer's timer << 16 | control << 8 | data */
#define	TOSIOC_INTEN	TOSIOC(17)	/* value: MFP channel << 8 | 1 enable, 0 disable */


struct tosenter {
	unsigned long	te_ramsize;	/* ST-RAM at 0 */
	unsigned long	te_flags;	/* TEF_* */
	unsigned short	te_w, te_h;	/* the host screen */
	unsigned short	te_depth, te_pad;
	unsigned long	te_hostlo, te_hosthi;	/* the launcher's code: host calls run there */
	unsigned long	te_stklo, te_stkhi;	/* the host calls' stack */
};

#define	TEF_MONO	0x01		/* monochrome monitor */
#define	TEF_NOMACH	0x02		/* a lone program: no machine, ST-RAM is the caller's */
#define	TEF_TCEOI	0x04		/* Timer C's handler clears pv_tcsvc at its end */

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

/* the guest's VBLs, taken and held off */
struct tosvideo {
	unsigned long	tv_vbl;
	unsigned long	tv_vblheld;	/* ts_vblheld */
};

struct tosstat {
	unsigned long	ts_pid;
	unsigned long	ts_ints[16];	/* MFP interrupts delivered, by channel */
	unsigned long	ts_vbl;		/* VBL interrupts delivered */
	unsigned long	ts_absent;	/* bus errors in the I/O windows */
	unsigned long	ts_top;		/* the last page's PTEs reloaded */
	unsigned long	ts_berr;	/* other bus errors reflected */
	unsigned long	ts_refl[64];	/* exceptions reflected, by vector */
	unsigned long	ts_priv, ts_mmu;
	unsigned long	ts_lastpc;	/* guest PC at the last kernel entry */
	unsigned long	ts_sys;		/* host system calls (trap #0) */
	unsigned long	ts_stop;	/* stop instructions (the guest idles) */
	unsigned long	ts_cache;	/* cache instructions and CACR writes */
	unsigned long	ts_slept;	/* clock ticks asleep in stop */
	unsigned long	ts_idle;	/* input polls slept on */
	unsigned long	ts_vblheld;	/* ticks the guest ran with a VBL masked */
	unsigned long	ts_held;	/* requests held at an emulation tail */
	unsigned long	ts_fpriv, ts_ftrap, ts_linea;	/* the gates' fast paths, all guests */
	unsigned long	ts_iow;		/* writes that faulted on the I/O pages */
	unsigned long	ts_iowpc;	/* the last one's pc */
	unsigned long	ts_isp;		/* movep emulated (68060) */
	unsigned long	ts_tcheld;	/* Timer C held: its handler still ran */
};

/*
 * Input the cartridge hands straight to TOS's own handlers each VBL:
 * the last page of the cartridge, shared by the guest and the display
 * process.
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
	unsigned long	pv_la;		/* guest: its Line A base */
	unsigned long	pv_tcsvc;	/* nonzero while Timer C's handler runs */
};

/*
 * The screen as TOS set it through its XBIOS and VBL handler, kept by
 * the launcher's host calls and shown by the display process; in the
 * same page.  A TT shifter: the ST palette is the current bank of 16.
 */
#define	TOSVID		(TOSPV + 0x840)

struct tosvid {
	unsigned long	vd_seq;		/* bumped after each change; 0 until TOS set a screen */
	unsigned long	vd_base;
	unsigned short	vd_mode;	/* TT shifter: mode << 8 | bank */
	unsigned short	vd_pal[256];	/* TT colours */
};

/*
 * The YM2149 as TOS's XBIOS sets it, played by the sound pump; in the
 * same page.  Dosound sequences run in the pump at 50 Hz.
 */
#define	TOSYM		(TOSPV + 0xa80)

struct tosym {
	unsigned long	ym_tgen;	/* bumped by each Dosound */
	unsigned long	ym_table;	/* Dosound's sequence */
	unsigned long	ym_seq;		/* the sequence's next command, 0 none */
	unsigned long	ym_egen;	/* bumped by each envelope shape write */
	unsigned char	ym_reg[16];
};

#define	PV_MOUSE	1
#define	PV_KEYS		2

#define	PE_KEY		1		/* scan code, $80 on release */
#define	PE_BTN		2		/* buttons: 1 right, 2 left */

/* the sound pump (TOSIOC_SND): buffer ends */
struct tossndio {
	unsigned long	sn_ends;	/* TSE_* inputs */
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

/* the shared page's structures, in order, within the page */
extern char tos_pvfits[sizeof (struct tospv) <= TOSVID - TOSPV &&
    TOSVID + sizeof (struct tosvid) <= TOSYM && TOSYM + sizeof (struct tosym) <= TOSSND &&
    TOSSND + sizeof (struct tossnd) <= TOSPV + 0x1000 ? 1 : -1];

#endif	/* _TOSIO_H */
