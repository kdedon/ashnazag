/*
 * win.h -- our USER and GDI: what their parts share.  The Windows
 * constants are written out here (values only, as the Win16 API fixes
 * them).
 */
#ifndef WIN_H
#define WIN_H

#include "w16.h"
#include "font.h"

/* ---- geometry ---- */

struct rect {
	int	l, t, r, b;
};

struct rgn {
	int	n, max;
	struct rect *r;		/* disjoint, sorted by top then left */
	struct rect box;
};

extern void rgn_init();		/* (rgn) empty */
extern void rgn_free();		/* (rgn) */
extern void rgn_set();		/* (rgn, rect) */
extern void rgn_copy();		/* (dst, src) */
extern void rgn_and();		/* (dst, a, b) */
extern void rgn_or();		/* (dst, a, b) */
extern void rgn_diff();		/* (dst, a, b) */
extern void rgn_xor();		/* (dst, a, b) */
extern void rgn_andrect();	/* (rgn, rect) in place */
extern void rgn_subrect();	/* (rgn, rect) in place */
extern void rgn_addrect();	/* (rgn, rect) in place */
extern void rgn_offset();	/* (rgn, dx, dy) */
extern int rgn_ptin();		/* (rgn, x, y) */
extern int rgn_rectin();	/* (rgn, rect): any overlap */
extern int rgn_equal();		/* (a, b) */
extern int rgn_kind();		/* (rgn): NULLREGION, SIMPLEREGION, COMPLEXREGION */

#define	NULLREGION	1
#define	SIMPLEREGION	2
#define	COMPLEXREGION	3

#define	R_EMPTY(p)	((p)->l >= (p)->r || (p)->t >= (p)->b)
extern int r_and();		/* (dst, a, b): nonempty */
extern void r_set();		/* (r, l, t, r, b) */
extern void r_get();		/* (rect, guest RECT linear address) */
extern void r_put();		/* (guest RECT linear address, rect) */

/* ---- surfaces: 8 bits a pixel ---- */

struct surf {
	int	w, h, rowb;
	u8	*pix;
	int	mono;		/* a monochrome bitmap: pixels 0 (black) or 1 (white) */
};

extern struct surf screen;	/* the display */
extern void scr_dirty();	/* (rect) on the screen */

/* ---- colours ---- */

typedef u32 COLORREF;		/* 0x00bbggrr; 0x01000000 | index for PALETTEINDEX */
#define	RGB(r, g, b)	((r) | (g) << 8 | (u32)(b) << 16)
#define	CR_R(c)		((c) & 0xff)
#define	CR_G(c)		((c) >> 8 & 0xff)
#define	CR_B(c)		((c) >> 16 & 0xff)

extern u8 syspal[256][3];
extern int pal_index();		/* (COLORREF): nearest system palette index */
extern void pal_init();
extern int pal_mono();		/* (COLORREF): 0 black, 1 white */

/* ---- GDI objects ---- */

#define	OBJ_PEN		1
#define	OBJ_BRUSH	2
#define	OBJ_DC		3
#define	OBJ_METADC	4
#define	OBJ_PAL		5
#define	OBJ_FONT	6
#define	OBJ_BITMAP	7
#define	OBJ_RGN		8
#define	OBJ_METAFILE	9
#define	OBJ_MEMDC	10

struct pen {
	int	style, width;
	COLORREF color;
};

struct brush {
	int	style, hatch;
	COLORREF color;
	u16	bitmap;		/* pattern: a copy */
	u8	pat[64];	/* 8x8 pixels: palette indexes, or 0/1 for a mono pattern */
	int	monopat;	/* pat holds 0/1, drawn in text and background colours */
};

struct logfont {
	int	height, width, escapement, orientation, weight;
	int	italic, underline, strikeout, charset;
	int	outprec, clipprec, quality, pitchfam;
	char	face[32];
};

struct font {
	struct logfont lf;
	struct bfont *bf;	/* the bitmap font it maps to */
	int	bold;		/* drawn twice, one pixel apart */
};

struct bitmap {
	struct surf s;
	int	bpp;		/* as the program sees it: 1 or 8 */
	int	planes;
	u16	seldc;		/* the memory DC it is selected into */
	int	dimx, dimy;	/* SetBitmapDimension */
};

struct palette {
	int	n;
	u8	ent[256][4];	/* r, g, b, flags */
	u8	map[256];	/* to system palette indexes */
};

struct dcstate {
	u16	pen, brush, font, bitmap, pal;
	COLORREF text, bk;
	int	bkmode, rop2, polyfill, stretch, align, relabs;
	int	curx, cury;
	int	mapmode, wox, woy, wex, wey, vox, voy, vex, vey;
	int	bx, by;		/* brush origin */
	int	extra, breakext, breakcnt;
	struct rgn *clip;	/* the program's clip region, in device units from the DC origin; 0 none */
};

struct dc {
	struct dcstate st;
	struct surf *s;		/* what it draws on */
	int	ox, oy;		/* the DC origin on the surface */
	struct rgn vis;		/* visible region, surface coordinates */
	struct rgn eff;		/* vis and the clip region */
	int	effok;
	u16	hwnd;
	int	kind;		/* DCK_* */
	int	inuse;
	u16	h;		/* its handle */
	struct dcstate *saved[16];
	int	nsaved;
	int	ownwindow;	/* CS_OWNDC */
	void	*priv_bm;	/* a memory DC's own 1x1 bitmap */
	int	epoch;		/* window DCs: vis_epoch when vis was made */
	struct rgn *paint;	/* BeginPaint: the update region, surface coordinates */
};
extern int vis_epoch;		/* windows moved, shown or hidden since */
extern void dc_refresh();	/* (dc) a window DC's visible region again */
#define	DCK_WINDOW	1	/* a window's client area */
#define	DCK_WINDOWNC	2	/* a window, frame included */
#define	DCK_MEMORY	3
#define	DCK_SCREEN	4	/* the whole display (CreateDC "DISPLAY") */
#define	DCK_INFO	5	/* CreateIC, a printer we don't have */

struct gobj {
	int	type;
	int	stock;
	union {
		struct pen pen;
		struct brush brush;
		struct font font;
		struct bitmap *bm;
		struct rgn rgn;
		struct palette *pal;
		struct dc *dc;
	} u;
};

extern struct gobj *gobj();	/* (handle, type or 0) */
extern u16 gobj_new();		/* (type): handle */
extern void gobj_delete();	/* (handle) */
extern u16 stockobj[];		/* by GetStockObject index */
extern struct dc *dc_get();	/* (hdc) */
extern u16 dc_new();		/* (kind) */
extern void dc_free();		/* (hdc) */
extern void dc_setwindow();	/* (dc, hwnd, kind) visible region and origin from the window */
extern void dc_reset();		/* (dc) attributes to defaults */
extern struct rgn *dc_clip();	/* (dc): the region drawing goes through */
extern void gdi_init();
extern struct bfont *font_of();	/* (dc) */
extern int text_width();	/* (bfont, s, n) */
extern void text_draw();	/* (dc, x, y, s, n, clip rect or 0, opaque rect or 0, dx array or 0) device coords */
extern void lp2dp();		/* (dc, &x, &y) */
extern void dp2lp();		/* (dc, &x, &y) */
extern int lx2dx();		/* (dc, n) a width */
extern int ly2dy();
extern struct bfont *font_pick();	/* (logfont) */

/* drawing in device (surface) coordinates, through the DC's region */
extern void d_fill();		/* (dc, rect, brush handle, rop3 pattern code) */
extern void d_fillcolor();	/* (dc, rect, palette index) */
extern void d_line();		/* (dc, x0, y0, x1, y1): with the pen, last point excluded */
extern void d_pixel();		/* (dc, x, y, index) */
extern void d_blt();		/* (dst dc, dx, dy, w, h, src dc or 0, sx, sy, rop) */
extern void d_stretch();	/* (dst dc, dx, dy, dw, dh, src dc, sx, sy, sw, sh, rop) */
extern void d_poly();		/* (dc, pts, n, fill, outline) */
extern void d_ellipse();	/* (dc, rect, fill, outline) */
extern void d_frame();		/* (dc, rect, index) one pixel */
extern void d_invert();		/* (dc, rect) */
extern int brush_index();	/* (brush handle): a solid brush's palette index, -1 patterned */

/* ROPs */
#define	R2_BLACK	1
#define	R2_NOTMERGEPEN	2
#define	R2_MASKNOTPEN	3
#define	R2_NOTCOPYPEN	4
#define	R2_MASKPENNOT	5
#define	R2_NOT		6
#define	R2_XORPEN	7
#define	R2_NOTMASKPEN	8
#define	R2_MASKPEN	9
#define	R2_NOTXORPEN	10
#define	R2_NOP		11
#define	R2_MERGENOTPEN	12
#define	R2_COPYPEN	13
#define	R2_MERGEPENNOT	14
#define	R2_MERGEPEN	15
#define	R2_WHITE	16

#define	SRCCOPY		0x00cc0020
#define	SRCPAINT	0x00ee0086
#define	SRCAND		0x008800c6
#define	SRCINVERT	0x00660046
#define	SRCERASE	0x00440328
#define	NOTSRCCOPY	0x00330008
#define	NOTSRCERASE	0x001100a6
#define	MERGECOPY	0x00c000ca
#define	MERGEPAINT	0x00bb0226
#define	PATCOPY		0x00f00021
#define	PATPAINT	0x00fb0a09
#define	PATINVERT	0x005a0049
#define	DSTINVERT	0x00550009
#define	BLACKNESS	0x00000042
#define	WHITENESS	0x00ff0062

#define	TRANSPARENT	1
#define	OPAQUE		2

#define	MM_TEXT		1
#define	MM_LOMETRIC	2
#define	MM_HIMETRIC	3
#define	MM_LOENGLISH	4
#define	MM_HIENGLISH	5
#define	MM_TWIPS	6
#define	MM_ISOTROPIC	7
#define	MM_ANISOTROPIC	8

#define	PS_SOLID	0
#define	PS_DASH		1
#define	PS_DOT		2
#define	PS_DASHDOT	3
#define	PS_DASHDOTDOT	4
#define	PS_NULL		5
#define	PS_INSIDEFRAME	6

#define	BS_SOLID	0
#define	BS_NULL		1
#define	BS_HATCHED	2
#define	BS_PATTERN	3
#define	BS_DIBPATTERN	5

#define	TA_UPDATECP	0x0001
#define	TA_RIGHT	0x0002
#define	TA_CENTER	0x0006
#define	TA_BOTTOM	0x0008
#define	TA_BASELINE	0x0018

#define	WHITE_BRUSH	0
#define	LTGRAY_BRUSH	1
#define	GRAY_BRUSH	2
#define	DKGRAY_BRUSH	3
#define	BLACK_BRUSH	4
#define	NULL_BRUSH	5
#define	WHITE_PEN	6
#define	BLACK_PEN	7
#define	NULL_PEN	8
#define	OEM_FIXED_FONT	10
#define	ANSI_FIXED_FONT	11
#define	ANSI_VAR_FONT	12
#define	SYSTEM_FONT	13
#define	DEVICE_DEFAULT_FONT 14
#define	DEFAULT_PALETTE	15
#define	SYSTEM_FIXED_FONT 16
#define	NSTOCK		17

/* ---- USER ---- */

struct cls {
	char	name[64];
	u16	atom;
	u32	style;
	u32	proc;		/* far pointer: a thunk for ours */
	int	clsextra, wndextra;
	u16	hinst;		/* 0 for the system's */
	u16	icon, cursor, bg;
	u32	menuname;	/* far pointer or integer, as given */
	char	menustr[64];
	u8	*extra;
	int	global;
	struct cls *next;
};

struct prop {
	u16	atom;
	char	*name;
	u16	val;
	struct prop *next;
};

struct sbinfo {
	int	min, max, pos;
	int	disabled;
	int	shown;
	int	pressed;	/* the arrow held: 1 or 5 */
};

struct wnd {
	struct task *task;	/* the task that made it: its messages are that task's */
	u16	h;
	struct cls *cls;
	u32	style, exstyle;
	struct wnd *parent, *owner;
	struct wnd *child;	/* topmost child */
	struct wnd *next;	/* next sibling below */
	struct rect wr, cr;	/* window and client rectangles, screen coordinates */
	char	*text;
	u32	proc;
	u16	hinst;
	u16	id;		/* child id, or the menu of a top-level window */
	u8	*extra;
	struct rgn upd;		/* needing paint, screen coordinates */
	int	erase;		/* WM_ERASEBKGND owed */
	int	ncpaint;	/* WM_NCPAINT owed */
	int	flags;		/* WF_* */
	struct prop *props;
	struct sbinfo sb[2];	/* horizontal, vertical */
	u16	sysmenu;
	u16	owndc;
	struct rect normal;	/* restored position */
	u16	lastfocus;	/* dialogs: the control that had the focus */
	int	dlgresult;
	u32	dlgproc;	/* dialogs: the program's dialog procedure */
	u16	dlgfont;
	u16	hicon;
	u32	user;		/* our controls' state */
	void	*priv;
	struct rect priv_track;	/* the outline while moved or sized */
};
#define	WF_DESTROYING	0x0001
#define	WF_DIALOG	0x0002
#define	WF_DLGEND	0x0004
#define	WF_PAINTING	0x0008
#define	WF_CREATED	0x0010
#define	WF_INTERNALPAINT 0x0020
#define	WF_ACTIVE	0x0040
#define	WF_MENUOWNED	0x0080
#define	WF_NEEDSIZE	0x0100	/* made hidden: WM_SIZE and WM_MOVE come when first shown */

extern struct wnd *desktop;
extern struct wnd *wnd_get();		/* (hwnd) */
extern struct wnd *wnd_focus, *wnd_active, *wnd_capture;
extern u32 wnd_send();			/* (wnd, msg, wparam, lparam) */
extern u32 wnd_sendh();			/* (hwnd, msg, wparam, lparam) */
extern int wnd_post();			/* (hwnd, msg, wparam, lparam) */
extern u32 wnd_call();			/* (proc, hwnd, msg, wparam, lparam) */
extern void wnd_invalidate();		/* (wnd, rect in screen coordinates or 0, erase) and children */
extern void wnd_visrgn();		/* (wnd, rgn out, client) visible region in screen coordinates */
extern void wnd_redrawframe();		/* (wnd) */
extern void wnd_update();		/* (wnd) paint it and its children now */
extern int wnd_visible();		/* (wnd) it and its parents shown */
extern struct wnd *wnd_create();	/* (exstyle, class, title, style, x, y, w, h, parent, menu, hinst, param) */
extern void wnd_destroy();
extern void wnd_setpos();		/* (wnd, after, x, y, w, h, flags) */
extern void wnd_show();
extern void wnd_setfocus();
extern void wnd_activate();
extern struct wnd *wnd_frompoint();	/* (x, y, &hittest) */
extern struct wnd *wnd_toplevel();	/* (wnd) */
extern struct cls *cls_find();		/* (name far pointer or atom, hinst) */
extern struct cls *cls_register();	/* (name, style, proc, clsextra, wndextra, hinst, icon, cursor, bg, menu, global) */
extern u32 sys_color();			/* (COLOR_*) */
extern int sys_metric();		/* (SM_*) */
extern u16 sys_brush();			/* (COLOR_*): a brush of that colour */
extern void user_init();
extern void user_yield();
extern int user_messagebox();		/* (hwnd, text, caption, style) */
extern u32 user_defproc();		/* (wnd, msg, wparam, lparam) */
extern void user_drawnc();		/* (wnd) */
extern int user_nchittest();		/* (wnd, x, y) */
extern int msgloop();			/* (hwnd or 0, filter or 0 = until WM_QUIT or the window is gone) modal */
extern int user_getmessage();		/* (msg linear, hwnd, min, max, remove, wait) */
extern u32 user_dispatch();		/* (msg linear) */
extern u16 user_dc();			/* (wnd, kind, clip rgn or 0) a DC of the cache */
extern void user_releasedc();		/* (hdc) */
extern void caret_hide(), caret_show();
extern u16 cur_arrow;
extern void cur_set();			/* (hcursor) */
extern u16 cur_load();			/* (hinst, id or name far pointer) */
extern u16 icon_load();			/* (hinst, id or name far pointer) */
extern void icon_draw();		/* (dc, hicon, x, y) surface coordinates */
extern u16 sysmenu_of();		/* (wnd) its system menu */
extern void wnd_calcclient();		/* (wnd, window rect, client rect out) */
extern int frame_width();
extern void draw_arrow();

/* our own window procedures */
extern u32 button_proc(), static_proc(), edit_proc(), listbox_proc(), combo_proc(), scroll_proc();
extern u32 dlg_defproc(), mdiclient_proc(), desktop_proc(), menu_popupproc();
extern u32 native_wndproc();		/* (fn, wnd, msg, wp, lp) */

/* menus */
extern u16 menu_load();			/* (hinst, name far pointer) */
extern u16 menu_fromtemplate();		/* (linear address) */
extern int menu_barheight();		/* (wnd) */
extern void menu_drawbar();		/* (wnd, dc) */
extern int menu_track();		/* (hmenu, flags, x, y, owner, bar item or -1) */
extern int menu_barhit();		/* (wnd, x, y): item or -1 */
extern void menu_trackbar();		/* (wnd, item, key) */
extern void menu_destroy();
extern int menu_key();			/* (wnd, ch): a menu bar mnemonic */

/* dialogs */
extern u16 dlg_create();		/* (hinst, template linear, owner, proc, param, modal) */
extern int dlg_run();			/* (hwnd) modal loop: the result */
extern int dlg_ismsg();			/* (dlg, msg linear) */

/* controls helpers */
extern void draw_3dbox();		/* (dc, rect, pressed) */
extern int draw_text();		/* (dc, text, n, rect, DT_ flags): the height */
extern int draw_textw();		/* (bfont, text, n) */

/* ---- messages ---- */

#define	WM_NULL		0x0000
#define	WM_CREATE	0x0001
#define	WM_DESTROY	0x0002
#define	WM_MOVE		0x0003
#define	WM_SIZE		0x0005
#define	WM_ACTIVATE	0x0006
#define	WM_SETFOCUS	0x0007
#define	WM_KILLFOCUS	0x0008
#define	WM_ENABLE	0x000a
#define	WM_SETREDRAW	0x000b
#define	WM_SETTEXT	0x000c
#define	WM_GETTEXT	0x000d
#define	WM_GETTEXTLENGTH 0x000e
#define	WM_PAINT	0x000f
#define	WM_CLOSE	0x0010
#define	WM_QUERYENDSESSION 0x0011
#define	WM_QUIT		0x0012
#define	WM_QUERYOPEN	0x0013
#define	WM_ERASEBKGND	0x0014
#define	WM_SYSCOLORCHANGE 0x0015
#define	WM_ENDSESSION	0x0016
#define	WM_SHOWWINDOW	0x0018
#define	WM_CTLCOLOR	0x0019
#define	WM_WININICHANGE	0x001a
#define	WM_ACTIVATEAPP	0x001c
#define	WM_FONTCHANGE	0x001d
#define	WM_CANCELMODE	0x001f
#define	WM_SETCURSOR	0x0020
#define	WM_MOUSEACTIVATE 0x0021
#define	WM_CHILDACTIVATE 0x0022
#define	WM_GETMINMAXINFO 0x0024
#define	WM_ICONERASEBKGND 0x0027
#define	WM_NEXTDLGCTL	0x0028
#define	WM_DRAWITEM	0x002b
#define	WM_MEASUREITEM	0x002c
#define	WM_DELETEITEM	0x002d
#define	WM_VKEYTOITEM	0x002e
#define	WM_CHARTOITEM	0x002f
#define	WM_SETFONT	0x0030
#define	WM_GETFONT	0x0031
#define	WM_QUERYDRAGICON 0x0037
#define	WM_COMPAREITEM	0x0039
#define	WM_WINDOWPOSCHANGING 0x0046
#define	WM_WINDOWPOSCHANGED 0x0047
#define	WM_NCCREATE	0x0081
#define	WM_NCDESTROY	0x0082
#define	WM_NCCALCSIZE	0x0083
#define	WM_NCHITTEST	0x0084
#define	WM_NCPAINT	0x0085
#define	WM_NCACTIVATE	0x0086
#define	WM_GETDLGCODE	0x0087
#define	WM_NCMOUSEMOVE	0x00a0
#define	WM_NCLBUTTONDOWN 0x00a1
#define	WM_NCLBUTTONUP	0x00a2
#define	WM_NCLBUTTONDBLCLK 0x00a3
#define	WM_NCRBUTTONDOWN 0x00a4
#define	WM_NCRBUTTONUP	0x00a5
#define	WM_KEYDOWN	0x0100
#define	WM_KEYUP	0x0101
#define	WM_CHAR		0x0102
#define	WM_DEADCHAR	0x0103
#define	WM_SYSKEYDOWN	0x0104
#define	WM_SYSKEYUP	0x0105
#define	WM_SYSCHAR	0x0106
#define	WM_INITDIALOG	0x0110
#define	WM_COMMAND	0x0111
#define	WM_SYSCOMMAND	0x0112
#define	WM_TIMER	0x0113
#define	WM_HSCROLL	0x0114
#define	WM_VSCROLL	0x0115
#define	WM_INITMENU	0x0116
#define	WM_INITMENUPOPUP 0x0117
#define	WM_MENUSELECT	0x011f
#define	WM_MENUCHAR	0x0120
#define	WM_ENTERIDLE	0x0121
#define	WM_MOUSEMOVE	0x0200
#define	WM_LBUTTONDOWN	0x0201
#define	WM_LBUTTONUP	0x0202
#define	WM_LBUTTONDBLCLK 0x0203
#define	WM_RBUTTONDOWN	0x0204
#define	WM_RBUTTONUP	0x0205
#define	WM_RBUTTONDBLCLK 0x0206
#define	WM_MBUTTONDOWN	0x0207
#define	WM_MBUTTONUP	0x0208
#define	WM_PARENTNOTIFY	0x0210
#define	WM_ENTERMENULOOP 0x0211
#define	WM_EXITMENULOOP	0x0212
#define	WM_MDICREATE	0x0220
#define	WM_MDIDESTROY	0x0221
#define	WM_MDIACTIVATE	0x0222
#define	WM_MDIRESTORE	0x0223
#define	WM_MDINEXT	0x0224
#define	WM_MDIMAXIMIZE	0x0225
#define	WM_MDITILE	0x0226
#define	WM_MDICASCADE	0x0227
#define	WM_MDIICONARRANGE 0x0228
#define	WM_MDIGETACTIVE	0x0229
#define	WM_MDISETMENU	0x0230
#define	WM_DROPFILES	0x0233
#define	WM_CUT		0x0300
#define	WM_COPY		0x0301
#define	WM_PASTE	0x0302
#define	WM_CLEAR	0x0303
#define	WM_UNDO		0x0304
#define	WM_QUERYNEWPALETTE 0x030f
#define	WM_PALETTECHANGED 0x0311
#define	WM_USER		0x0400

/* WM_SYSCOMMAND */
#define	SC_SIZE		0xf000
#define	SC_MOVE		0xf010
#define	SC_MINIMIZE	0xf020
#define	SC_MAXIMIZE	0xf030
#define	SC_NEXTWINDOW	0xf040
#define	SC_CLOSE	0xf060
#define	SC_VSCROLL	0xf070
#define	SC_HSCROLL	0xf080
#define	SC_MOUSEMENU	0xf090
#define	SC_KEYMENU	0xf100
#define	SC_RESTORE	0xf120
#define	SC_TASKLIST	0xf130

/* hit tests */
#define	HTERROR		(-2)
#define	HTTRANSPARENT	(-1)
#define	HTNOWHERE	0
#define	HTCLIENT	1
#define	HTCAPTION	2
#define	HTSYSMENU	3
#define	HTSIZE		4
#define	HTMENU		5
#define	HTHSCROLL	6
#define	HTVSCROLL	7
#define	HTMINBUTTON	8
#define	HTMAXBUTTON	9
#define	HTLEFT		10
#define	HTRIGHT		11
#define	HTTOP		12
#define	HTTOPLEFT	13
#define	HTTOPRIGHT	14
#define	HTBOTTOM	15
#define	HTBOTTOMLEFT	16
#define	HTBOTTOMRIGHT	17
#define	HTBORDER	18

/* styles */
#define	WS_OVERLAPPED	0x00000000
#define	WS_POPUP	0x80000000
#define	WS_CHILD	0x40000000
#define	WS_MINIMIZE	0x20000000
#define	WS_VISIBLE	0x10000000
#define	WS_DISABLED	0x08000000
#define	WS_CLIPSIBLINGS	0x04000000
#define	WS_CLIPCHILDREN	0x02000000
#define	WS_MAXIMIZE	0x01000000
#define	WS_CAPTION	0x00c00000
#define	WS_BORDER	0x00800000
#define	WS_DLGFRAME	0x00400000
#define	WS_VSCROLL	0x00200000
#define	WS_HSCROLL	0x00100000
#define	WS_SYSMENU	0x00080000
#define	WS_THICKFRAME	0x00040000
#define	WS_GROUP	0x00020000
#define	WS_TABSTOP	0x00010000
#define	WS_MINIMIZEBOX	0x00020000
#define	WS_MAXIMIZEBOX	0x00010000
#define	WS_EX_DLGMODALFRAME 0x00000001
#define	WS_EX_NOPARENTNOTIFY 0x00000004
#define	WS_EX_TOPMOST	0x00000008
#define	WS_EX_ACCEPTFILES 0x00000010
#define	WS_EX_TRANSPARENT 0x00000020

#define	CS_VREDRAW	0x0001
#define	CS_HREDRAW	0x0002
#define	CS_DBLCLKS	0x0008
#define	CS_OWNDC	0x0020
#define	CS_CLASSDC	0x0040
#define	CS_PARENTDC	0x0080
#define	CS_NOCLOSE	0x0200
#define	CS_SAVEBITS	0x0800
#define	CS_GLOBALCLASS	0x4000

#define	CW_USEDEFAULT	((short)0x8000)

#define	SW_HIDE		0
#define	SW_SHOWNORMAL	1
#define	SW_SHOWMINIMIZED 2
#define	SW_SHOWMAXIMIZED 3
#define	SW_MAXIMIZE	3
#define	SW_SHOWNOACTIVATE 4
#define	SW_SHOW		5
#define	SW_MINIMIZE	6
#define	SW_SHOWMINNOACTIVE 7
#define	SW_SHOWNA	8
#define	SW_RESTORE	9

#define	SWP_NOSIZE	0x0001
#define	SWP_NOMOVE	0x0002
#define	SWP_NOZORDER	0x0004
#define	SWP_NOREDRAW	0x0008
#define	SWP_NOACTIVATE	0x0010
#define	SWP_FRAMECHANGED 0x0020
#define	SWP_SHOWWINDOW	0x0040
#define	SWP_HIDEWINDOW	0x0080
#define	SWP_NOCOPYBITS	0x0100
#define	SWP_NOOWNERZORDER 0x0200

/* system colours */
#define	COLOR_SCROLLBAR		0
#define	COLOR_BACKGROUND	1
#define	COLOR_ACTIVECAPTION	2
#define	COLOR_INACTIVECAPTION	3
#define	COLOR_MENU		4
#define	COLOR_WINDOW		5
#define	COLOR_WINDOWFRAME	6
#define	COLOR_MENUTEXT		7
#define	COLOR_WINDOWTEXT	8
#define	COLOR_CAPTIONTEXT	9
#define	COLOR_ACTIVEBORDER	10
#define	COLOR_INACTIVEBORDER	11
#define	COLOR_APPWORKSPACE	12
#define	COLOR_HIGHLIGHT		13
#define	COLOR_HIGHLIGHTTEXT	14
#define	COLOR_BTNFACE		15
#define	COLOR_BTNSHADOW		16
#define	COLOR_GRAYTEXT		17
#define	COLOR_BTNTEXT		18
#define	COLOR_INACTIVECAPTIONTEXT 19
#define	COLOR_BTNHIGHLIGHT	20
#define	NSYSCOLOR		21

/* system metrics */
#define	SM_CXSCREEN	0
#define	SM_CYSCREEN	1
#define	SM_CXVSCROLL	2
#define	SM_CYHSCROLL	3
#define	SM_CYCAPTION	4
#define	SM_CXBORDER	5
#define	SM_CYBORDER	6
#define	SM_CXDLGFRAME	7
#define	SM_CYDLGFRAME	8
#define	SM_CYVTHUMB	9
#define	SM_CXHTHUMB	10
#define	SM_CXICON	11
#define	SM_CYICON	12
#define	SM_CXCURSOR	13
#define	SM_CYCURSOR	14
#define	SM_CYMENU	15
#define	SM_CXFULLSCREEN	16
#define	SM_CYFULLSCREEN	17
#define	SM_CYKANJIWINDOW 18
#define	SM_MOUSEPRESENT	19
#define	SM_CYVSCROLL	20
#define	SM_CXHSCROLL	21
#define	SM_DEBUG	22
#define	SM_SWAPBUTTON	23
#define	SM_CXMIN	28
#define	SM_CYMIN	29
#define	SM_CXSIZE	30
#define	SM_CYSIZE	31
#define	SM_CXFRAME	32
#define	SM_CYFRAME	33
#define	SM_CXMINTRACK	34
#define	SM_CYMINTRACK	35
#define	SM_CXDOUBLECLK	36
#define	SM_CYDOUBLECLK	37
#define	SM_CXICONSPACING 38
#define	SM_CYICONSPACING 39
#define	SM_MENUDROPALIGNMENT 40

/* DrawText */
#define	DT_TOP		0x0000
#define	DT_LEFT		0x0000
#define	DT_CENTER	0x0001
#define	DT_RIGHT	0x0002
#define	DT_VCENTER	0x0004
#define	DT_BOTTOM	0x0008
#define	DT_WORDBREAK	0x0010
#define	DT_SINGLELINE	0x0020
#define	DT_EXPANDTABS	0x0040
#define	DT_TABSTOP	0x0080
#define	DT_NOCLIP	0x0100
#define	DT_EXTERNALLEADING 0x0200
#define	DT_CALCRECT	0x0400
#define	DT_NOPREFIX	0x0800

/* virtual keys */
#define	VK_LBUTTON	0x01
#define	VK_RBUTTON	0x02
#define	VK_CANCEL	0x03
#define	VK_MBUTTON	0x04
#define	VK_BACK		0x08
#define	VK_TAB		0x09
#define	VK_CLEAR	0x0c
#define	VK_RETURN	0x0d
#define	VK_SHIFT	0x10
#define	VK_CONTROL	0x11
#define	VK_MENU		0x12
#define	VK_PAUSE	0x13
#define	VK_CAPITAL	0x14
#define	VK_ESCAPE	0x1b
#define	VK_SPACE	0x20
#define	VK_PRIOR	0x21
#define	VK_NEXT		0x22
#define	VK_END		0x23
#define	VK_HOME		0x24
#define	VK_LEFT		0x25
#define	VK_UP		0x26
#define	VK_RIGHT	0x27
#define	VK_DOWN		0x28
#define	VK_INSERT	0x2d
#define	VK_DELETE	0x2e
#define	VK_HELP		0x2f
#define	VK_NUMPAD0	0x60
#define	VK_MULTIPLY	0x6a
#define	VK_ADD		0x6b
#define	VK_SEPARATOR	0x6c
#define	VK_SUBTRACT	0x6d
#define	VK_DECIMAL	0x6e
#define	VK_DIVIDE	0x6f
#define	VK_F1		0x70
#define	VK_NUMLOCK	0x90
#define	VK_SCROLL	0x91

/* WM_ACTIVATE */
#define	WA_INACTIVE	0
#define	WA_ACTIVE	1
#define	WA_CLICKACTIVE	2

/* MessageBox */
#define	MB_OK		0x0000
#define	MB_OKCANCEL	0x0001
#define	MB_ABORTRETRYIGNORE 0x0002
#define	MB_YESNOCANCEL	0x0003
#define	MB_YESNO	0x0004
#define	MB_RETRYCANCEL	0x0005
#define	MB_ICONHAND	0x0010
#define	MB_ICONQUESTION	0x0020
#define	MB_ICONEXCLAMATION 0x0030
#define	MB_ICONASTERISK	0x0040
#define	MB_DEFBUTTON2	0x0100
#define	MB_DEFBUTTON3	0x0200
#define	IDOK		1
#define	IDCANCEL	2
#define	IDABORT		3
#define	IDRETRY		4
#define	IDIGNORE	5
#define	IDYES		6
#define	IDNO		7

/* resource types */
#define	RT_CURSOR	1
#define	RT_BITMAP	2
#define	RT_ICON		3
#define	RT_MENU		4
#define	RT_DIALOG	5
#define	RT_STRING	6
#define	RT_FONTDIR	7
#define	RT_FONT		8
#define	RT_ACCELERATOR	9
#define	RT_RCDATA	10
#define	RT_GROUP_CURSOR	12
#define	RT_GROUP_ICON	14

/* guest structures, by offset */
#define	MSG_HWND	0
#define	MSG_MESSAGE	2
#define	MSG_WPARAM	4
#define	MSG_LPARAM	6
#define	MSG_TIME	10
#define	MSG_PT		14
#define	MSG_SIZE	18

#endif
