/*
 * tosfb.h -- the session's frame buffer as TOS sees it.
 *
 * The launcher maps the frame buffer into the guest and describes it
 * in the cartridge.  The fVDI driver draws there and publishes its
 * palette in ST-RAM under the cookie TFB_COOKIE; the display process
 * loads that palette into the session's CLUT and stops converting the
 * ST screen while fs_on is set.
 */
#ifndef _TOSFB_H
#define _TOSFB_H

#define	TFB_CART	0xfa0068	/* struct tfbcart */
#define	TFB_COOKIE	0x41736846L	/* "AshF" */
#define	TFB_MAGIC	0x7f8e0001L

struct tfbcart {
	unsigned long	fc_addr;	/* 0: no frame buffer */
	unsigned short	fc_width, fc_height;
	unsigned long	fc_rowbytes;
	unsigned short	fc_depth, fc_pad;
};

struct tfbshare {
	unsigned long	fs_magic;
	unsigned long	fs_on;		/* the driver owns the screen */
	unsigned long	fs_seq;		/* bumped after each palette change */
	unsigned short	fs_pal[256][3];	/* 16-bit components by pixel value */
};

#endif
