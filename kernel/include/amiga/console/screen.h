/* Console font format. */
#ifndef _AMIGA_SCREEN_H
#define _AMIGA_SCREEN_H

struct charent {
	unsigned char	ce_height;
	unsigned char	ce_width;
	unsigned char	ce_basel;	/* pixels above the baseline */
	unsigned char	ce_move;	/* cursor advance */
	unsigned char	ce_bpl;		/* bytes per bitmap line */
	unsigned char	_pad;
	unsigned short	ce_data;	/* bitmap offset in the font */
};

struct font {
	unsigned short	f_magic;
	unsigned short	f_flags;
	unsigned short	f_count;
	unsigned short	_pad0;
	unsigned long	f_length;	/* bytes, bitmaps included */
	unsigned char	f_height;
	unsigned char	f_width;
	unsigned char	f_baseline;
	unsigned char	_pad1;
	struct charent	f_ctable[256];
};

#endif
