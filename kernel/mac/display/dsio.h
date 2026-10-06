/*
 * dsio.h -- display service interface: /dev/fbN, /dev/kbd, /dev/mouse.
 *
 * Shared by the kernel (K&R, -traditional) and user programs.  Ioctl
 * arguments are either a value (noted "value") or a pointer to the
 * structure named; the driver copies them in and out.
 */
#ifndef DSIO_H
#define DSIO_H

#define FBIOC(n)	(('F' << 8) | (n))
#define FBIOGINFO	FBIOC(1)	/* out struct fbinfo */
#define FBIOGMODES	FBIOC(2)	/* in/out struct fbmodes */
#define FBIOSMODE	FBIOC(3)	/* value: mode id (ENXIO: no mode set yet) */
#define FBIOGETCMAP	FBIOC(4)	/* in/out struct fbcmap */
#define FBIOPUTCMAP	FBIOC(5)	/* in struct fbcmap */
#define FBIOACQUIRE	FBIOC(6)	/* in/out struct fbacq */
#define FBIORELEASE	FBIOC(7)
#define FBIOSWITCH	FBIOC(8)	/* value: session id, 0 = console */
#define FBIOGSTATE	FBIOC(9)	/* out struct fbstate */
#define FBIOVBLWAIT	FBIOC(10)	/* value: VBLs to wait, 1..FB_MAXVBLWAIT */
#define FBIOGVBL	FBIOC(11)	/* out unsigned long: VBL count */
#define FBIOBLANK	FBIOC(12)	/* value: 1 blank, 0 unblank */
#define FBIOCACHE	FBIOC(13)	/* value: FBC_WT or FBC_CI, for the next mmap */
#define FBIOVIDEL	FBIOC(14)	/* value: where the caller maps a FBA_VIDEL session */

#define EVIOC(n)	(('E' << 8) | (n))
#define EVIOCGINFO	EVIOC(1)	/* out struct evinfo */
#define EVIOCBIND	EVIOC(2)	/* value: fd of a /dev/fbN session */
#define EVIOCGKEYS	EVIOC(3)	/* out unsigned char[16]: keys down, by code */
#define EVIOCSLED	EVIOC(4)	/* value: EVL_* mask */

#define FB_MAXVBLWAIT	600

/* fi_type */
#define FBT_DAFB	1
#define FBT_NUBUS	2
#define FBT_VIDEL	6
#define FBT_SVIDEL	7
/* fi_layout */
#define FBL_PACKED	1
#define FBL_IPLAN2	4	/* interleaved bitplanes, 16-pixel words */
/* fi_visual */
#define FBV_MONO	1	/* pixel 0 white */
#define FBV_PSEUDO	2
#define FBV_TRUE	3
/* fi_flags */
#define FBF_VBL		0x01	/* hardware VBL interrupt */
#define FBF_BLANK	0x02
#define FBF_SETMODE	0x04
#define FBF_CMAP	0x08	/* CLUT can be written */

struct fbinfo {
	unsigned long	fi_type;
	unsigned long	fi_layout;
	unsigned long	fi_width, fi_height;
	unsigned long	fi_depth;	/* bits per pixel */
	unsigned long	fi_rowbytes;
	unsigned long	fi_planebytes;
	unsigned long	fi_visual;
	unsigned long	fi_rmask, fi_gmask, fi_bmask;
	unsigned long	fi_cmapsize, fi_cmapbits;
	unsigned long	fi_offset;	/* first pixel, from mmap offset 0 */
	unsigned long	fi_size;	/* bytes to map */
	unsigned long	fi_mode;	/* current mode id */
	unsigned long	fi_flags;
	unsigned long	fi_mmwidth, fi_mmheight;
	char		fi_name[16];
};

struct fbmodeinfo {
	unsigned long	mi_id;
	unsigned long	mi_width, mi_height;
	unsigned long	mi_depth;
	unsigned long	mi_rowbytes;
	unsigned long	mi_offset;	/* first pixel from the device base */
	unsigned long	mi_flags;	/* FBM_CURRENT */
};
#define FBM_CURRENT	0x01

struct fbmodes {
	unsigned long	ms_count;	/* in: room in ms_modes; out: modes offered */
	struct fbmodeinfo *ms_modes;
};

/* 16-bit components; the hardware keeps the top fi_cmapbits */
struct fbcmap {
	unsigned short	cm_start, cm_count;
	unsigned short	*cm_red, *cm_green, *cm_blue;
};

struct fbacq {
	unsigned long	fa_kind;	/* FBK_USER */
	unsigned long	fa_flags;	/* FBA_FRONT */
	unsigned long	fa_id;		/* out: session id */
	char		fa_name[16];
};
#define FBK_USER	1
#define FBA_FRONT	0x01
#define FBA_VIDEL	0x02	/* owns the video hardware; fi_size grows to the pool */

struct fbstate {
	long		st_session;	/* caller's session id, -1 none */
	long		st_front;	/* session in front, 0 console */
	unsigned long	st_serial;	/* switches so far */
};

/* read() on a session's fb fd */
struct fbnote {
	unsigned long	fn_type;
	unsigned long	fn_serial;
};
#define FBN_HIDDEN	1
#define FBN_SHOWN	2
#define FBN_MODE	3

#define FBC_WT		1	/* write-through (default) */
#define FBC_CI		2	/* cache-inhibited */

struct inev {			/* 16 bytes */
	unsigned char	ie_type;
	unsigned char	ie_unit;
	unsigned short	ie_code;	/* key: native code; rel: axis; btn: 1..n */
	long		ie_value;	/* key/btn: 1 down, 0 up; rel: delta, y down positive */
	long		ie_sec, ie_usec;
};
#define IE_KEY		1
#define IE_REL		2
#define IE_BTN		3
#define IE_ABS		4
#define IE_SYN		5
#define IE_DROP		6
#define IE_RELX		0
#define IE_RELY		1

struct evinfo {
	unsigned long	ei_kset;	/* EVK_* */
	unsigned long	ei_id;		/* ADB: keyboard handler id */
	unsigned long	ei_flags;	/* EVF_* */
};
#define EVK_ADB		1
#define EVK_AMIGA	2
#define EVK_IKBD	3
#define EVF_CAPSLATCH	0x01

#define EVL_NUM		0x01
#define EVL_CAPS	0x02
#define EVL_SCROLL	0x04

#endif
