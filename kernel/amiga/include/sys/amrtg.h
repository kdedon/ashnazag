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
};

extern int am_rtgattach();	/* (struct am_rtg *) 0, or EBUSY once in use */
extern int am_rtgdetach();	/* (struct am_rtg *) 0, or EBUSY while in use */

#endif
