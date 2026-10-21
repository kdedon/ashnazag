/*
 * lisaio.h -- /dev/lisa: an Apple Lisa running its own boot ROM.
 *
 * The launcher asks for physical RAM (LISAIOC_RAM), maps it and the
 * ROM copy with mmap (RAM at offset 0, ROM at LISA_ROMOFF), fills the
 * ROM, then LISAIOC_ENTER makes it the guest and it jumps to the ROM's
 * reset code.  The module maps the Lisa's logical space at 0 from the
 * segment registers; the launcher's patches keep the guest out of its
 * I/O space.
 */

#ifndef _LISAIO_H
#define _LISAIO_H

#define	LISA_MAJOR	68

#define	LISAIOC(n)	(('L' << 8) | (n))
#define	LISAIOC_RAM	LISAIOC(1)	/* value: bytes of physical RAM */
#define	LISAIOC_ENTER	LISAIOC(2)	/* in struct lisaenter */
#define	LISAIOC_STAT	LISAIOC(3)	/* out struct lisastat */
#define	LISAIOC_MEM	LISAIOC(4)	/* in/out struct lisamem: guest memory at a logical address */
#define	LISAIOC_PATCH	LISAIOC(5)	/* in struct lisapatch: write guest RAM, push the caches */
#define	LISAIOC_SEG	LISAIOC(6)	/* in struct lisaseg: load segment registers, setup mode off */
#define	LISAIOC_IRQ	LISAIOC(7)	/* value LIRQ_ON, or LIRQ_TAKE: returns a level 1 source, 0 none */
#define	LISAIOC_VIDEO	LISAIOC(8)	/* value: the video page, 32 KB units of RAM */
#define	LISAIOC_INPUT	LISAIOC(9)	/* value: an input event to queue (level 2), or 0: take one, 0 none */
#define	LISAIOC_CTX	LISAIOC(10)	/* value: the user-mode context (SEG1, SEG2) */
#define	LISAIOC_ALINE	LISAIOC(11)	/* value: the launcher's user-mode A-line entry */

/* input events */
#define	LIN_KEY		0x100		/* | the key byte: code, bit 7 down */
#define	LIN_MOUSE	0x20000		/* | dx << 8 | dy, signed bytes */

#define	LIRQ_TAKE	0
#define	LIRQ_ON		1		/* retrace (each host tick) and the 20 ms timer start */
#define	LIRQ_RETRACE	1
#define	LIRQ_TIMER	2

#define	LISA_ROMOFF	0x1000000	/* mmap offset of the ROM copy */
#define	LISA_ROMSZ	0x4000
#define	LISA_RAMMAX	0x200000

struct lisaenter {
	unsigned long	le_phys;	/* where the launcher mapped RAM */
	unsigned long	le_rom;		/* and the ROM */
	long		le_tzoff;	/* local time - UTC, seconds */
	unsigned long	le_hostlo, le_hosthi;	/* the launcher's code: host calls run there */
};

/* ls_flags: how far the boot got */
#define	LSF_VIDEO	0x02		/* video page chosen */
#define	LSF_FAULT	0x10		/* a bus error went to the guest */
#define	LSF_PFREAD	0x20		/* a volume block was read */
#define	LSF_BOOT	0x40		/* the ROM ran code it loaded */
#define	LSF_SHARED	0x80		/* a window shared its top byte with the launcher */

struct lisapatch {
	unsigned long	lp_addr;	/* physical RAM offset */
	int		lp_len;		/* up to 16 */
	unsigned char	lp_data[16];
};

/* lm_flags */
#define	LM_WRITE	0x01		/* lm_data to the guest, else from it */
#define	LM_BLOCK	0x02		/* counts volume block lm_blk as read */
#define	LM_FLUSH	0x04		/* then push the caches: the data is code */

struct lisamem {
	unsigned long	lm_addr;	/* logical */
	int		lm_len;		/* up to 532 */
	int		lm_flags;
	unsigned long	lm_blk;
	unsigned char	lm_data[532];
};

struct lisaseg {
	int		lg_ctx;		/* context the registers belong to */
	int		lg_first, lg_n;	/* segments */
	int		lg_final;	/* context selected afterwards */
	unsigned short	lg_slr[128], lg_sor[128];
};

struct lisastat {
	long		ls_pid;
	unsigned long	ls_flags;
	unsigned long	ls_remap, ls_berr;
	unsigned long	ls_berraddr, ls_berrpc;
	unsigned long	ls_intr;	/* interrupts taken */
	unsigned long	ls_pfreads, ls_pfwrites, ls_pfblock;	/* ProFile blocks, the last one */
	unsigned long	ls_bootpc;	/* where loaded code first ran */
	unsigned char	ls_vid;		/* video page: 32 KB units of RAM */
	unsigned long	ls_hwfault;	/* faults on I/O and unmappable RAM pages */
	unsigned long	ls_hwaddr, ls_hwpc;	/* the last one */
	unsigned long	ls_ctxsw;	/* context switches on S changes */
	unsigned long	ls_aline;	/* user-mode A-lines taken through supervisor mode */
	unsigned long	ls_alias;	/* windows mapped above 16 MB */
	unsigned long	ls_cpush;	/* cache pushes after code was written */
};

#endif
