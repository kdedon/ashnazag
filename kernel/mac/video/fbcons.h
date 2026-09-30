/*
 * fbcons.h -- frame-buffer text console: renderer, VT100 subset, and
 * the hooks shared with the console tty and a keyboard driver.
 *
 * Everything the renderer keeps is initialised data, so it works from
 * aux_entry on, with the MMU off and before BSS is cleared.  The frame
 * buffer is addressed VA = PA: physical with the MMU off, through DTT1
 * (supervisor, cache-inhibited, 0x80000000-0xFFFFFFFF) with it on.
 */
#ifndef FBCONS_H
#define FBCONS_H

#ifndef VOL
#define VOL	__volatile__
#endif

#define FB_CW	8		/* cell width, pixels */
#define FB_CH	16		/* cell height, pixels */
#define FB_NPAR	8		/* CSI parameters kept */
#define FB_NPEND 512		/* bytes queued by nested callers */

/* what the renderer draws on */
struct fbmode {
	unsigned long	fm_base;	/* first pixel */
	unsigned long	fm_row;		/* bytes per scan line */
	unsigned long	fm_depth;	/* 1, 2, 4, 8, 16 or 32 */
	unsigned long	fm_width;	/* visible pixels */
	unsigned long	fm_height;
};

/* one VT100 parser; kernel printf and the console tty have their own */
struct fbvt {
	int	v_state;
	int	v_npar;
	int	v_par[FB_NPAR];
	int	v_priv;			/* '?' after CSI */
	int	v_g[2];			/* G0/G1: 0 ASCII, 1 DEC graphics */
	int	v_gl;			/* 1 after SO */
	int	v_kern;			/* LF implies CR; never answers */
};

struct fbcons {
	int		fc_on;
	struct fbmode	fc_m;
	int		fc_cols, fc_rows;
	int		fc_x, fc_y;
	int		fc_wrap;	/* pending wrap after the last column */
	int		fc_top, fc_bot;	/* scroll region */
	int		fc_inv, fc_bold;
	int		fc_curs;	/* cursor enabled (DECTCEM) */
	int		fc_cdrawn;
	int		fc_awm;		/* autowrap (DECAWM) */
	int		fc_sx, fc_sy, fc_sinv, fc_sbold;
	unsigned long	fc_fg, fc_bg;	/* pixel values replicated to 32 bits */
	unsigned long	fc_exp[16][4];	/* nibble -> pixel mask at fm_depth */
	int		fc_busy;
	unsigned short	fc_pend[FB_NPEND];
	int		fc_pput, fc_pget;
	unsigned long	fc_lost;	/* nested bytes dropped */
};

extern struct fbcons fbcons;
extern struct fbvt fbvt_kern, fbvt_tty;
extern unsigned char fb_font[256][16];

/* renderer (fbcons.c) */
int	fbcons_attach();	/* (struct fbmode *): 1 if the mode is usable */
void	fbcons_kputc();		/* (c): kernel printf mirror */
void	fbcons_putc();		/* (c): console tty output, VT100 subset */
void	fbcons_write();		/* (buf, n): same, a block */
int	fbcons_active();	/* 1 while the screen is the console */
void	fbcons_unlock();	/* mac_stop: draw what an interrupted owner queued */

/* mode discovery (fbprobe.c) */
int	fbcons_auxinit();	/* (info): aux_entry, A/UX low memory */
int	fbcons_biinit();	/* (bootinfo): mac_shim_main, boot record */
void	fbcons_report();	/* config(): what was found, on both consoles */

/*
 * Keyboard side.  The keyboard driver calls fbcons_input() with every
 * byte it produces (ASCII, or an escape sequence for special keys), at
 * any IPL.  The bytes go to the console tty (SCC channel A stream, as
 * if typed on the serial line) and are dropped while it is closed.
 * Terminal answers (DA, DSR) use the same path.
 */
void	fbcons_input();		/* (c) */

#endif
