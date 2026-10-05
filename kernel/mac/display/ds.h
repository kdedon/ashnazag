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

#ifdef DS_ATARI
#define DS_PGSHIFT	11	/* 68030 kernel: 2 KB pages */
#else
#define DS_PGSHIFT	12
#endif
#define DS_PGSIZE	(1 << DS_PGSHIFT)
#define DS_PGOFF	(DS_PGSIZE - 1)

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
#ifdef DS_ATARI
#define DS_HI		6	/* IKBD and the MFP */
/* 68030: clear the logically addressed data cache */
#define DS_CPUSHA()	__asm__ __volatile__(".word 0x4e7a,0x0002,0x0040,0x0800,0x4e7b,0x0002" \
	: : : "d0", "memory")
#else
#define DS_HI		2	/* ADB and clock (1), VBL (2) */
#define DS_CPUSHA()	__asm__ __volatile__(".word 0xf478" : : : "memory")	/* cpusha dc */
#endif

#ifdef DS_ATARI
/* Videl state: registers $FF8200-$FF82C3 and the palette */
struct dsvid {
	unsigned char	v_reg[0xC4];
	unsigned long	v_pal[256];
	int		v_st;		/* the ST shift mode was set last */
	unsigned char	v_blt[0x3E];	/* blitter registers $FF8A00-$FF8A3D */
};

/* one blitter run: a line or part of one, at physical addresses */
struct dsbrun {
	unsigned short	r_ht[16];	/* halftone */
	short		r_sxi, r_syi, r_dxi, r_dyi;
	unsigned long	r_sa, r_da;
	unsigned short	r_em[3];	/* end masks */
	unsigned short	r_xn;
	unsigned char	r_hop, r_op;
	unsigned char	r_ctl;		/* smudge and line number */
	unsigned char	r_skew;		/* FXSR, NFSR, skew */
};
#define DS_VPOOL	0x80000		/* ST-RAM kept for the screen */
#endif

struct dssess {
	int		s_used;
	long		s_id;		/* creation serial; 0 console */
	long		s_uid;
	char		s_name[16];
	caddr_t		s_mem;		/* shadow as allocated */
	unsigned long	s_memsize;
	caddr_t		s_shadow;	/* page-aligned shadow of the mapped region */
	unsigned long	*s_pfn;		/* its page frames */
	unsigned long	s_size;		/* bytes mapped */
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
#ifdef DS_ATARI
	struct dsvid	*s_vid;		/* owns the Videl: its state */
	struct proc	*s_vproc;	/* passes the guest's Videl writes */
	pid_t		s_vpid;
	unsigned long	s_vwin;		/* where s_vproc maps the region */
	unsigned long	s_vgbase;	/* screen address the guest set */
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

/* ds.c */
int	ds_init();		/* () -> errno */
struct dssess *ds_newsess();	/* (uid, name, errp) */
struct dssess *ds_mksess();	/* (uid, name, errp, videl) */
void	ds_endsess();		/* (s) */
void	ds_sessgc();		/* (s) */
int	ds_switch();		/* (s) -> errno; safe context only */
struct dssess *ds_byid();	/* (id) */
void	ds_setcmap();		/* (s, start, n, r, g, b) */
void	ds_setblank();		/* (s, on) */
void	ds_note();		/* (s, type) */
void	ds_now();		/* (sec, usec) */
void	ds_vblintr();
#ifdef DS_ATARI
void	ds_relmouse();		/* (buttons, dx, dy) */
int	ds_vidpass();		/* (s, window) -> errno */
void	ds_vidput();		/* (addr, size, value) */
void	ds_bltgo();		/* (registers) */
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
