/*
 * uinter.h -- /dev/uinter0, the A/UX user-interface device: the Mac
 * environment's layer, ROM and kernel low-memory values.
 */

#ifndef _UINTER_H
#define _UINTER_H

#include "sys/types.h"
#include "sys/conf.h"		/* before moddefs.h: struct mod_drv_data */
#include "auxcore.h"

#define	UI_MAJOR	54
#define	UI_VERSION	5
#define	UI_UIPSIZE	0x1000		/* the ui page */
#define	UI_LOWSIZE	0x3000		/* UI_COPY_OUT range */
#define	UI_PRODSIZE	0x34		/* ProductInfo record */
#define	UI_ROMBASE	0x40800000	/* where the Mac side maps the ROM */
#define	UI_PMSTACK	0x3fff0000	/* Process Manager's own stack */
#define	UI_PMSTKSZ	0x10000

/* ioctl encoding (BSD) */
#define	UIOC_NUM(c)	((c) & 0xff)
#define	UIOC_SIZE(c)	(((c) >> 16) & 0xff)
#define	UIOC_IN		0x80000000
#define	UIOC_OUT	0x40000000
#define	UIOC_VOID	0x20000000
#define	UIOC_GROUP	0x5100		/* 'Q' */

/* a Unix process of the layer; [0] made it */
struct uitask {
	struct proc	*t_proc;	/* 0: free */
	struct guest_proc *t_gp;
	k_sigset_t	t_hold;		/* its own mask, while t_held */
	int		t_held;
	int		t_tick;		/* takes the tick (UI_TIMER) */
	long		t_left;		/* A/UX 2: ticks to its one tick, 0 none */
};
#define	UI_NTASK	16

/* the one layer */
struct uilayer {
	int		l_state;	/* LS_* */
	struct proc	*l_proc;	/* its first task */
	struct guest_proc *l_gp;
	struct uitask	l_task[UI_NTASK];
	struct guest_proc *l_active;	/* the one task that runs Mac code */
	int		l_shmid;
	int		l_tid;		/* tick callout, 0 = off */
	caddr_t		l_uip;		/* ui page, user address */
	struct proc	*l_uiproc;	/* who mapped it */
	caddr_t		l_romaddr;	/* 0 = no ROM mapping */
	struct proc	*l_romproc;
	long		l_coffid;
	char		l_coffname[128];
	short		l_coffnamelen;
	unsigned char	l_pram[256];	/* XPRAM */
	unsigned char	l_kchr[0xc00];
	unsigned short	l_evmask;
	int		l_kchrlen;
	int		l_lapchk;	/* the AppleTalk queue calls looked at */
	int		l_sdchk;	/* 1 + (real uid != 0) at the last Shut Down patch */
};
#define	LS_EMPTY	0
#define	LS_INUSE	2

/* the ROM object (uirom.c) */
struct uirom {
	struct vnode	*r_vp;		/* held while loaded */
	unsigned long	r_size;
	unsigned short	r_version;
	unsigned long	r_sum;
	long		r_prodoff;	/* ProductInfo in the image, -1 = none */
	unsigned char	r_prod[UI_PRODSIZE];
	unsigned char	*r_low;		/* kernel low memory, UI_LOWSIZE */
	int		r_box;		/* box flag asked for, -1: pickbox */
	int		r_aux2;		/* loaded for an A/UX 2 Mac */
};

extern struct uirom ui_rom;

/* the last calls, always kept: 'Q' ioctls, 'S' Slot Manager selectors */
struct uicall {
	char		c_kind;
	unsigned char	c_num;
	short		c_res;
	long		c_n;		/* repeats */
};
#define	UI_NCALL	16
extern void ui_note();
extern struct uilayer ui;
extern struct uitask *ui_task();
extern caddr_t ui_uipof();
extern int uinter_trace;
extern int ui_slotmgr(), ui_screens(), ui_video();
extern void ui_unscreen();
extern void ui_kin(), ui_evtick(), ui_inscreen(), ui_inreset(), ui_inflags();
extern void ui_update(), ui_flushevents();
extern int ui_getosevent(), ui_postevent(), ui_getkeys();
#define	IN_DEV		1	/* input flags: UI_DEVICES */
#define	IN_CUR		2	/* UI_CURSOR */
extern char uinter_rom[];
extern int uinter_boxflag;
extern char uinter_model[];
extern int ui_romload();
extern void ui_romfree(), ui_romrel();
extern int ui_rombusy();
extern int ui_rommap(), ui_romunmap();
extern int valid_usr_range();

static __inline__ int
splhi_()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s;
}

static __inline__ void
splx_(s)
	int s;
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}

#endif	/* _UINTER_H */
