/*
 * An RTG card as the Amiga host's display: its module hands this to
 * am_rtgattach before the display service first opens.  The chipset
 * keeps the console and still supplies keyboard, mouse and VBL.
 */

#ifndef _SYS_AMRTG_H
#define _SYS_AMRTG_H

struct am_rtg {
	unsigned long	r_base;		/* first pixel, kernel address */
	int		r_width, r_height;
	int		r_depth;	/* 8: packed, CLUT */
	int		r_row;		/* bytes */
	char		*r_name;
	void		(*r_clut)();	/* (index, r, g, b), 8 bits a gun */
	void		(*r_show)();	/* (on): the card's picture, else the chipset's */
	/*
	 * Optional deeper modes at the same size: r_depths has 1 for 8 bits,
	 * 2 for 16 and 4 for 32; r_rgb the red, green and blue masks of a
	 * pixel read as a big-endian word, for 16 and 32 bits.
	 */
	int		r_depths;
	unsigned long	r_rgb[2][3];
	void		(*r_setdepth)();	/* (bits): the card's pixels from now */
	/*
	 * Optional: with r_pan, each session may own a frame in the r_vram
	 * bytes from r_base's page, and the card shows the one at the
	 * offset it is given.  r_phys: r_base's physical address, if not
	 * r_base.
	 */
	void		(*r_pan)();	/* (offset from r_base's page) */
	unsigned long	r_vram;
	unsigned long	r_phys;
};

extern int am_rtgattach();	/* (struct am_rtg *) 0, or EBUSY once in use */
extern int am_rtgbusy();	/* () 0, or EBUSY once a card or the chipset has it */
extern int am_rtgdetach();	/* (struct am_rtg *) 0, or EBUSY while in use */

#endif
