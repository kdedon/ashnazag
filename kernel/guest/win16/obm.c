/*
 * obm.c -- the system's bitmaps, LoadBitmap(NULL, OBM_...): in Windows
 * the display driver's resources, so ours to supply.  Drawn here at
 * the VGA sizes with the routines that draw the frame, scroll bars and
 * buttons, so they match what is on the screen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "win.h"

#define	OBM_LFARROWI	32734
#define	OBM_RGARROWI	32735
#define	OBM_DNARROWI	32736
#define	OBM_UPARROWI	32737
#define	OBM_COMBO	32738
#define	OBM_MNARROW	32739
#define	OBM_LFARROWD	32740
#define	OBM_RGARROWD	32741
#define	OBM_DNARROWD	32742
#define	OBM_UPARROWD	32743
#define	OBM_RESTORED	32744
#define	OBM_ZOOMD	32745
#define	OBM_REDUCED	32746
#define	OBM_RESTORE	32747
#define	OBM_ZOOM	32748
#define	OBM_REDUCE	32749
#define	OBM_LFARROW	32750
#define	OBM_RGARROW	32751
#define	OBM_DNARROW	32752
#define	OBM_UPARROW	32753
#define	OBM_CLOSE	32754
#define	OBM_OLD_RESTORE	32755
#define	OBM_OLD_ZOOM	32756
#define	OBM_OLD_REDUCE	32757
#define	OBM_BTNCORNERS	32758
#define	OBM_CHECKBOXES	32759
#define	OBM_CHECK	32760
#define	OBM_BTSIZE	32761
#define	OBM_OLD_LFARROW	32762
#define	OBM_OLD_RGARROW	32763
#define	OBM_OLD_DNARROW	32764
#define	OBM_OLD_UPARROW	32765
#define	OBM_SIZE	32766
#define	OBM_OLD_CLOSE	32767

extern void draw_3dbox(), draw_arrow(), draw_sysbox(), draw_capbutton();
extern u16 gdi_drawon(), gdi_newbitmap();
extern void gdi_drawn();

static void
fill(dc, l, t, r, b, idx)
	struct dc *dc;
	int l, t, r, b, idx;
{
	struct rect x;

	r_set(&x, l, t, r, b);
	d_fillcolor(dc, &x, idx);
}

static int
sysidx(c)
	int c;
{
	return pal_index(sys_color(c));
}

/* a scroll bar arrow button, s square: dir 0 up 1 down 2 left 3 right; how 0 up, 1 pressed, 2 inactive */
static void
arrowbtn(dc, x, y, s, dir, how)
	struct dc *dc;
	int x, y, s, dir, how;
{
	struct rect r;

	r_set(&r, x, y, x + s, y + s);
	d_frame(dc, &r, 0);
	r.l++, r.t++, r.r--, r.b--;
	draw_3dbox(dc, &r, how == 1);
	draw_arrow(dc, (r.l + r.r) / 2 + (how == 1), (r.t + r.b) / 2 + (how == 1), 4, dir,
	    how == 2 ? sysidx(COLOR_GRAYTEXT) : 0);
}

/* Windows 2's flat arrows: black on white in a black square */
static void
oldarrow(dc, x, y, s, dir)
	struct dc *dc;
	int x, y, s, dir;
{
	struct rect r;

	r_set(&r, x, y, x + s, y + s);
	d_fillcolor(dc, &r, 255);
	d_frame(dc, &r, 0);
	draw_arrow(dc, x + s / 2, y + s / 2, s / 2 - 2, dir, 0);
}

/* a check box or radio button image (13 square), as the button control draws them */
static void
checkimg(dc, x, y, radio, checked, pushed)
	struct dc *dc;
	int x, y, radio, checked, pushed;
{
	struct rect b, t;
	int i;

	r_set(&b, x, y, x + 13, y + 13);
	if (radio) {
		u16 keepb = dc->st.brush, keepp = dc->st.pen;

		dc->st.brush = pushed ? sys_brush(COLOR_BTNSHADOW) : stockobj[WHITE_BRUSH];
		dc->st.pen = stockobj[BLACK_PEN];
		d_ellipse(dc, &b, 1, 1);
		if (checked) {
			r_set(&t, b.l + 3, b.t + 3, b.r - 3, b.b - 3);
			dc->st.brush = stockobj[BLACK_BRUSH];
			d_ellipse(dc, &t, 1, 1);
		}
		dc->st.brush = keepb;
		dc->st.pen = keepp;
		return;
	}
	d_fillcolor(dc, &b, pushed ? sysidx(COLOR_BTNSHADOW) : 255);
	d_frame(dc, &b, 0);
	if (checked)
		for (i = 2; i < 11; i++) {
			fill(dc, b.l + i, b.t + i, b.l + i + 1, b.t + i + 1, checked == 2 ? sysidx(COLOR_GRAYTEXT) : 0);
			fill(dc, b.r - 1 - i, b.t + i, b.r - i, b.t + i + 1, checked == 2 ? sysidx(COLOR_GRAYTEXT) : 0);
		}
}

/* the bitmap for OBM id, or 0 */
u16
obm_load(id)
	int id;
{
	int w = 17, h = 17, face = sysidx(COLOR_BTNFACE), i, j;
	u16 hbm, hdc;
	struct dc *dc;

	switch (id) {
	case OBM_CLOSE:
		w = 2 * sys_metric(SM_CXSIZE), h = sys_metric(SM_CYSIZE);
		break;
	case OBM_REDUCE: case OBM_ZOOM: case OBM_RESTORE:
	case OBM_REDUCED: case OBM_ZOOMD: case OBM_RESTORED:
	case OBM_OLD_REDUCE: case OBM_OLD_ZOOM: case OBM_OLD_RESTORE: case OBM_OLD_CLOSE:
		w = sys_metric(SM_CXSIZE), h = sys_metric(SM_CYSIZE);
		break;
	case OBM_UPARROW: case OBM_DNARROW: case OBM_LFARROW: case OBM_RGARROW:
	case OBM_UPARROWD: case OBM_DNARROWD: case OBM_LFARROWD: case OBM_RGARROWD:
	case OBM_UPARROWI: case OBM_DNARROWI: case OBM_LFARROWI: case OBM_RGARROWI:
	case OBM_SIZE: case OBM_BTSIZE:
		w = sys_metric(SM_CXVSCROLL), h = sys_metric(SM_CYHSCROLL);
		break;
	case OBM_OLD_UPARROW: case OBM_OLD_DNARROW: case OBM_OLD_LFARROW: case OBM_OLD_RGARROW:
		w = h = 16;
		break;
	case OBM_COMBO:
		w = sys_metric(SM_CXVSCROLL), h = sys_metric(SM_CYHSCROLL) + 2;
		break;
	case OBM_MNARROW: case OBM_CHECK:
		w = h = 16;
		break;
	case OBM_CHECKBOXES:
		w = 4 * 13, h = 3 * 13;
		break;
	case OBM_BTNCORNERS:
		w = h = 5;
		break;
	default:
		return 0;
	}
	hbm = gdi_newbitmap(w, h);
	if (!hbm)
		return 0;
	hdc = gdi_drawon(hbm);
	dc = dc_get(hdc);
	fill(dc, 0, 0, w, h, face);
	switch (id) {
	case OBM_CLOSE:
		/* the window's box, and the MDI child's beside it */
		draw_sysbox(dc, 0, 0, h);
		draw_sysbox(dc, w / 2, 0, h);
		fill(dc, w / 2 + h - 6, h / 2 - 2, w / 2 + h - 2, h / 2 + 3, face);
		fill(dc, w / 2 + h - 7, h / 2 - 1, w / 2 + h - 6, h / 2 + 3, sysidx(COLOR_BTNSHADOW));
		fill(dc, w / 2 + 3, h / 2 + 2, w / 2 + h - 6, h / 2 + 3, sysidx(COLOR_BTNSHADOW));
		break;
	case OBM_OLD_CLOSE:
		draw_sysbox(dc, 0, 0, h);
		break;
	case OBM_REDUCE: case OBM_REDUCED: case OBM_OLD_REDUCE:
		draw_capbutton(dc, 0, 0, w, 0, id == OBM_REDUCED);
		break;
	case OBM_ZOOM: case OBM_ZOOMD: case OBM_OLD_ZOOM:
		draw_capbutton(dc, 0, 0, w, 1, id == OBM_ZOOMD);
		break;
	case OBM_RESTORE: case OBM_RESTORED: case OBM_OLD_RESTORE:
		draw_capbutton(dc, 0, 0, w, 2, id == OBM_RESTORED);
		break;
	case OBM_UPARROW: case OBM_UPARROWD: case OBM_UPARROWI:
		arrowbtn(dc, 0, 0, w, 0, id == OBM_UPARROWD ? 1 : id == OBM_UPARROWI ? 2 : 0);
		break;
	case OBM_DNARROW: case OBM_DNARROWD: case OBM_DNARROWI:
		arrowbtn(dc, 0, 0, w, 1, id == OBM_DNARROWD ? 1 : id == OBM_DNARROWI ? 2 : 0);
		break;
	case OBM_LFARROW: case OBM_LFARROWD: case OBM_LFARROWI:
		arrowbtn(dc, 0, 0, w, 2, id == OBM_LFARROWD ? 1 : id == OBM_LFARROWI ? 2 : 0);
		break;
	case OBM_RGARROW: case OBM_RGARROWD: case OBM_RGARROWI:
		arrowbtn(dc, 0, 0, w, 3, id == OBM_RGARROWD ? 1 : id == OBM_RGARROWI ? 2 : 0);
		break;
	case OBM_OLD_UPARROW:
		oldarrow(dc, 0, 0, w, 0);
		break;
	case OBM_OLD_DNARROW:
		oldarrow(dc, 0, 0, w, 1);
		break;
	case OBM_OLD_LFARROW:
		oldarrow(dc, 0, 0, w, 2);
		break;
	case OBM_OLD_RGARROW:
		oldarrow(dc, 0, 0, w, 3);
		break;
	case OBM_SIZE:
	case OBM_BTSIZE:
		break;		/* the plain grey corner of 3.1 */
	case OBM_COMBO:
		{
			struct rect r;

			r_set(&r, 0, 0, w, h);
			draw_3dbox(dc, &r, 0);
			draw_arrow(dc, w / 2, h / 2, 4, 1, 0);
		}
		break;
	case OBM_MNARROW:
		fill(dc, 0, 0, w, h, 255);
		draw_arrow(dc, w / 2, h / 2, 4, 3, 0);
		break;
	case OBM_CHECK:
		/* the menu check mark */
		fill(dc, 0, 0, w, h, 255);
		for (i = 0; i < 3; i++) {
			fill(dc, 3 + i, 7 + i, 4 + i, 10 + i, 0);
		}
		for (i = 0; i < 6; i++)
			fill(dc, 6 + i, 8 - i, 7 + i, 11 - i, 0);
		break;
	case OBM_CHECKBOXES:
		/* rows: check boxes, radio buttons, 3-state; columns: off, on, off pushed, on pushed */
		fill(dc, 0, 0, w, h, 255);
		for (j = 0; j < 3; j++)
			for (i = 0; i < 4; i++)
				checkimg(dc, 13 * i, 13 * j, j == 1, (i & 1) ? (j == 2 ? 2 : 1) : 0, i >= 2);
		break;
	case OBM_BTNCORNERS:
		fill(dc, 0, 0, w, h, 255);
		fill(dc, 1, 0, 4, 1, 0);
		fill(dc, 1, 4, 4, 5, 0);
		fill(dc, 0, 1, 1, 4, 0);
		fill(dc, 4, 1, 5, 4, 0);
		break;
	}
	gdi_drawn(hdc);
	return hbm;
}
