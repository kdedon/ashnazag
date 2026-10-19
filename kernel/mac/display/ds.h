/*
 * ds.h -- display service, kernel side.
 *
 * One display (the screen fbcons found), sessions that own it in turn,
 * the input devices routed to the session in front.  Pages are the
 * MMU's: 4 KB on the 68040 whatever the stock headers say, 2 KB on the
 * 68030.
 */
#ifndef DS_H
#define DS_H

#include "dsio.h"

#if defined(DS_ATARI) && !defined(ATA060)
#define DS_PGSHIFT	11	/* 68030 kernel: 2 KB pages */
#else
#define DS_PGSHIFT	12
#endif
#define DS_PGSIZE	(1 << DS_PGSHIFT)
#define DS_PGOFF	(DS_PGSIZE - 1)
#define DS_IDMAX	0x40000000UL	/* end of the kernel's identity map of RAM */

#define DS_NSESS	9	/* user sessions: hotkeys 1..9 */
#define DS_NFBH		16	/* open /dev/fbN files */
#define DS_NEVH		16	/* open /dev/kbd and /dev/mouse files */
#define DS_NSEG		64	/* mappings of sessions */
#define DS_QLEN		256	/* events per input file */
#define DS_NNOTE	8	/* notes per session */

#define DS_FBMAJ	51
#define DS_KBDMAJ	52
#define DS_MSMAJ	53

#define DAFB_REG	0xF9800000
#define DAFB_VRAM	0xF9000000
#define DAFB_INTMASK	0x104
#define DAFB_INTCLEAR	0x10C
#define DAFB_CLUTADDR	0x200
#define DAFB_CLUTDATA	0x213
#define DAFB_VBL	0x04

#ifndef VOL
#define VOL	__volatile__
#endif

/* raise the IPL to at least n; returns the old SR */
#define DS_SPL(n) ({ int __s, __t; \
	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (__s) : : "memory"); \
	__t = (__s & 0x700) < ((n) << 8) ? (__s & ~0x700) | ((n) << 8) : __s; \
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (__t) : "memory"); __s; })
#define DS_SPLX(s)	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory")
#ifdef DS_AMIGA
#define DS_HI 3
#elif defined(DS_ATARI)
#define DS_HI		6	/* IKBD and the MFP */
#else
#define DS_HI		2	/* ADB and clock (1), VBL (2) */
#endif
#if defined(DS_ATARI) && !defined(ATA060)
/* 68030: clear the logically addressed data cache */
#define DS_CPUSHA()	__asm__ __volatile__(".word 0x4e7a,0x0002,0x0040,0x0800,0x4e7b,0x0002" \
	: : : "d0", "memory")
#else
#define DS_CPUSHA()	__asm__ __volatile__(".word 0xf478" : : : "memory")	/* cpusha dc */
#endif

#ifdef DS_ATARI
/* Videl state: registers $FF8200-$FF82C3 and the palette */
struct dsvid {
	unsigned char	v_reg[0xC4];
	unsigned long	v_pal[256];
	int		v_st;		/* the ST shift mode was set last */
};
#endif

struct dssess {
	int		s_used;
	long		s_id;		/* creation serial; 0 console */
	long		s_uid;
	char		s_name[16];
	unsigned long	*s_pfn;		/* shadow page frames of the mapped region */
	unsigned long	s_npg;		/* frames owned: the first s_npg entries */
	unsigned long	s_size;		/* bytes mapped */
	unsigned long	s_kept;		/* bytes the shadow keeps; the rest share a page */
	int		s_dead;		/* released: maps fault */
	int		s_cache;	/* FBC_* */
	int		s_blank;
	unsigned short	s_cmap[3][256];
	int		s_dlo, s_dhi;	/* CLUT entries not yet in the hardware */
	unsigned char	s_keys[16];	/* keys delivered down */
	int		s_btn;		/* button delivered down */
	struct fbnote	s_note[DS_NNOTE];
	int		s_nput, s_nget;
	struct pollhead	s_ph;
	void		(*s_kin)();	/* in-kernel reader: (s, type, code, value) */
	unsigned long	s_rqw, s_rqh;	/* FBIOREQMODE's size in its mode, 0 the mode's */
	unsigned long	s_vorg;		/* bytes from the mode's first pixel to the screen's */
#ifdef DS_ATARI
	struct dsvid	*s_vid;		/* the Videl state of its mode */
	int		s_vmode;	/* FBIOSMODE's mode, index + 1; 0 none */
#endif
#ifdef DS_AMIGA
	int		s_vmode;	/* FBIOSMODE's mode, index + 1; 0 the display's */
	unsigned long	s_voff;		/* its own VRAM, from d_page; 0 none */
	unsigned long	s_vpan;		/* shown from there into it */
	unsigned long	s_ext;		/* bytes of its region touched so far */
#endif
};

struct dsfbh {			/* one open of /dev/fbN */
	int		h_used;
	struct dssess	*h_sess;
};

struct dsevh {			/* one open of /dev/kbd or /dev/mouse */
	int		e_used;
	int		e_mouse;
	struct dssess	*e_sess;	/* bound session, 0 none */
	struct inev	*e_q;
	int		e_put, e_get, e_drop;
	struct pollhead	e_ph;
};

/* the display, as found at boot */
struct dsdisp {
	int		d_on;
	int		d_dafb;		/* built-in DAFB: CLUT and VBL */
	unsigned long	d_base;		/* first pixel */
	unsigned long	d_page;		/* page-aligned start of the mapped region */
	unsigned long	d_vsize;	/* VRAM a session may map */
	struct fbinfo	d_info;
};

extern struct dsdisp ds_disp;
extern struct dssess ds_sess[];		/* [0] console */
extern struct dssess *ds_front;
extern struct dsfbh ds_fbh[];
extern struct dsevh ds_evh[];
extern unsigned long ds_gen, ds_serial, ds_vblcount;
extern unsigned long ds_nhwvbl, ds_nswvbl;
extern void (*ds_frontfn)();	/* (s): s came to the front; at DS_HI */
extern int (*ds_leavefn)();	/* (s, 0): s may leave the front, else EBUSY; (s, 1): it left */

/* ds.c */
int	ds_init();		/* () -> errno */
struct dssess *ds_newsess();	/* (uid, name, errp) */
void	ds_endsess();		/* (s) */
void	ds_sessgc();		/* (s) */
int	ds_switch();		/* (s) -> errno; safe context only */
struct dssess *ds_byid();	/* (id) */
void	ds_setcmap();		/* (s, start, n, r, g, b) */
void	ds_setblank();		/* (s, on) */
void	ds_note();		/* (s, type) */
void	ds_now();		/* (sec, usec) */
void	ds_vblintr();
void	ds_sinfo();		/* (s, fi): what FBIOGINFO reports for s */
int	ds_ncmap();		/* (s) -> its colour table's entries */
int	ds_reqmode();		/* (s, struct fbreq *) -> errno */
int	ds_modeok();		/* (s, id) -> errno */
#if defined(DS_ATARI) || defined(DS_AMIGA)
int	ds_setmode();		/* (s, id) -> errno */
#endif
#ifdef DS_AMIGA
int	ds_setpan();		/* (s, offset) -> errno */
int	ds_vsize();		/* (s, bytes) -> errno */
/*
 * On a card with panning a user session keeps its whole region at full
 * depth (DS_FULL): in front at d_page, hidden in VRAM of its own while
 * a frame is spare (DS_OWNV: mapped there, shown or not, no copy), else
 * in its shadow.
 */
extern unsigned long am_dsphys, am_dsvram, am_vslot(), am_vheld();
#define DS_PHYS(va)	((va) + am_dsphys)
#define DS_VOFF(s)	((s)->s_voff)
#define DS_OWNV(s)	((s)->s_voff != 0)
#define DS_FULL(s)	(am_dsvram != 0 && (s) != &ds_sess[0])
#define DS_VSPAN	(am_dsvram > ds_disp.d_vsize ? am_dsvram : ds_disp.d_vsize)
/* panned p bytes, the whole mode fi stays within size bytes */
#define DS_PANOK(p, fi, size)	(!((p) & 15) && (p) <= (size) && \
	(fi)->fi_offset + (fi)->fi_rowbytes * (fi)->fi_height <= (size) - (p))
#else
#define DS_PHYS(va)	(va)
#define DS_VOFF(s)	0
#define DS_OWNV(s)	0
#define DS_FULL(s)	0
#define DS_VSPAN	ds_disp.d_vsize
#endif
#ifdef DS_ATARI
void	ds_relmouse();		/* (buttons, dx, dy) */
#endif

/* dsseg.c */
int	ds_segmap();		/* d_segmap */
void	ds_unloadsess();	/* (s) */
int	ds_segcount();		/* (s) */

/* dsdev.c */
void	ds_evpost();		/* (s, mouse, type, code, value, sec, usec) */
int	ds_mayfront();		/* (cr) -> may bring a session to the front */
int	ds_owns();		/* (s) -> the calling process may use s: owner or privileged */

extern void wakeup(), bzero();
extern int timeout(), sleep(), copyin(), copyout(), drv_priv(), printf();
extern unsigned long vtop();

#endif
