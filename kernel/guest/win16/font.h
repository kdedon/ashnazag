/*
 * font.h -- bitmap fonts: the built-in ones (fonts.c) and .FNT/.FON
 * fonts loaded from the Windows directory.
 */
#ifndef FONT_H
#define FONT_H

struct bfont {
	char	*f_face;
	int	f_points;
	int	f_height, f_ascent, f_descent;
	int	f_weight, f_italic, f_pitch, f_family;	/* pitch: 1 fixed */
	int	f_avgw, f_maxw;
	int	f_first, f_last;
	unsigned char *f_w;		/* advance of each character */
	unsigned short *f_off;		/* its rows in f_bits */
	unsigned char *f_bits;		/* (w+7)/8 bytes a row, f_height rows */
};

extern struct bfont bfonts[];

#endif
