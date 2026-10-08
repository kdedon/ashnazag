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
	/* .FON fonts only (0 in the built-in ones) */
	int	f_res;			/* the display's dpi it is for */
	int	f_leading, f_extlead;	/* internal and external leading */
	int	f_charset, f_default, f_break;
	/* TrueType (ttf.c): advances here, glyphs rendered when first drawn */
	struct ttsize *f_tt;
	unsigned short *f_adv;
};

/* a TrueType glyph as drawn: bw pixels wide, from ox left of the pen */
struct ttglyph {
	unsigned char *bits;		/* (bw+7)/8 bytes a row, f_height rows */
	short	bw, ox;
};

extern struct ttglyph *ttf_glyph();	/* (font, character - f_first) */
extern struct bfont *ttf_font();	/* (face, bold, italic, height, width) */
extern char *ttf_face();		/* (name): a TrueType face of that name, or 0 */
extern char *ttf_family();		/* (FF_ family, fixed): a TrueType face for it, or 0 */
extern int ttf_add();			/* (host path of a .TTF) */
extern struct bfont *ttf_enum();	/* (i, face or 0): for EnumFonts */

extern struct bfont bfonts[];		/* built in */
extern struct bfont *fontlist;		/* Windows' (fontfile.c), then the built-in ones */
extern int nfontlist;
extern void font_init();		/* (host SYSTEM directory) */
extern int font_haveface();

#endif
