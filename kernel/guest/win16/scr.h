/*
 * scr.h -- the display and input device under our USER and GDI: the
 * screen surface (8 bits a pixel, system palette), input events and
 * the pointer.  scr_fb.c is the display service (/dev/fb, /dev/kbd,
 * /dev/mouse); scr_null.c draws into memory for tests, with input from
 * a script, and writes the screen as a PPM file.
 */
#ifndef SCR_H
#define SCR_H

struct ev {
	int	type;
	int	vk;		/* EV_KEY: virtual key; scan code in sc */
	int	sc;
	int	down;		/* EV_KEY, EV_BTN */
	int	x, y;		/* EV_MOVE: absolute; EV_BTN too */
	int	btn;		/* EV_BTN: 0 left, 1 right, 2 middle */
	int	ch;		/* EV_CHAR: a character typed (scripts) */
};
#define	EV_KEY		1
#define	EV_MOVE		2
#define	EV_BTN		3
#define	EV_QUIT		4	/* the session is asked to end */
#define	EV_HIDDEN	5	/* our session left the front (display service) */
#define	EV_SHOWN	6	/* and came back: everything repaints */

struct cursor {
	int	w, h, hx, hy;
	unsigned char *and;	/* 1 keeps the screen */
	unsigned char *xor;	/* 0 black, 1 white where and is 0; 1 inverts where and is 1 */
};

extern int scr_open();		/* (width, height, flags): 0 ok */
extern int scr_poll();		/* (ev, timeout ms, -1 forever): 1 an event, 0 none */
extern void scr_flush();	/* the dirty part to the device, and the pointer */
extern void scr_close();
extern void scr_setcursor();	/* (cursor or 0 hidden) */
extern void scr_warp();		/* (x, y) */
extern void scr_beep();
extern int scr_mx, scr_my;	/* the pointer */
extern char *scr_shot;		/* scr_null: write the screen here at the end */
extern char *scr_script;	/* scr_null: the input script */

#endif
