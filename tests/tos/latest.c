/*
 * latest.c -- the Line-A calls on the fVDI screen, run from C:\AUTO
 * after FVDI.PRG.  Each call draws into a cleared area and the pixels
 * are read back; "PASS name" or "FAIL name value" go to U:\LINEA.TXT,
 * with "INFO name value" timings in microseconds per call.
 */

extern long trap1(), lacall(), logbase(), hz200(), setscreen();
extern long lafonts;

static short w[8];
static int nw;
static char out[2048];
static int on;
static char *la;
static unsigned char *scr;
static long wrap;

#define	V(o)	(*(short *)(la + (o)))
#define	VL(o)	(*(long *)(la + (o)))
#define	PX(x, y) scr[(long)(y) * wrap + (x)]

static void pw(v) { w[nw++] = v; }
static void pl(v) long v; { w[nw++] = v >> 16; w[nw++] = v; }
static long go() { int n = nw; nw = 0; return trap1(w, n); }
static long Fcreate(n, a) char *n; { pw(0x3c); pl((long)n); pw(a); return go(); }
static long Fclose(h) { pw(0x3e); pw(h); return go(); }
static long Fwrite(h, c, b) long c; char *b; { pw(0x40); pw(h); pl(c); pl((long)b); return go(); }
static long Cconws(b) char *b; { pw(9); pl((long)b); return go(); }

static void
put(s)
	char *s;
{
	while (*s && on < sizeof out - 1)
		out[on++] = *s++;
}

static void
num(v)
	long v;
{
	char n[12];
	int i = 11;
	unsigned long u = v < 0 ? -v : v;

	n[i] = 0;
	do
		n[--i] = '0' + u % 10;
	while ((u /= 10) != 0);
	if (v < 0)
		n[--i] = '-';
	put(n + i);
}

static void
check(name, ok, v)
	char *name;
	long v;
{
	put(ok ? "PASS " : "FAIL ");
	put(name);
	if (!ok) {
		put(" ");
		num(v);
	}
	put("\r\n");
}

static void
info(name, v)
	char *name;
	long v;
{
	put("INFO ");
	put(name);
	put(" ");
	num(v);
	put("\r\n");
}

static char own[16 * 2048 + 16];	/* a program's own screen: its address only */
static short contrl[12], intin[40], ptsin[40], intout[8], ptsout[8];
static unsigned short solid[1] = { 0xffff };

static void
setcol(c)
{
	V(24) = c & 1; V(26) = c >> 1 & 1; V(28) = c >> 2 & 1; V(30) = c >> 3 & 1;
}

static void
lan(n)
{
	lacall((long)n, 0L, 0L, 0L, 0L, 0L);
}

/* x1..x2, y1..y2 in colour c, replace mode */
static void
box(x1, y1, x2, y2, c)
{
	setcol(c);
	V(36) = 0;
	V(38) = x1; V(40) = y1; V(42) = x2; V(44) = y2;
	VL(46) = (long)solid;
	V(50) = 0;
	V(52) = 0;
	V(54) = 0;
	lan(5);
}

/* pixels of x1..x2, y1..y2 not c: 0 when all are */
static long
bad(x1, y1, x2, y2, c)
{
	long n = 0;
	int x, y;

	for (y = y1; y <= y2; y++)
		for (x = x1; x <= x2; x++)
			n += PX(x, y) != c;
	return n;
}

static unsigned short glyph[32] = {
	0xffff, 0x8001, 0xbffd, 0xa005, 0xa7e5, 0xa425, 0xa5a5, 0xa5a5,
	0xa5a5, 0xa5a5, 0xa425, 0xa7e5, 0xa005, 0xbffd, 0x8001, 0xffff
};

static void
tests()
{
	long base, n, t, i, k;
	short bb[38], sb[38], spr[37], save[140];
	short src[5 + 16], *mf;
	unsigned char *f;
	short *fh, ch, *off, fw;

	base = lacall(0L, 0L, 0L, 0L, 0L, 0L);
	la = (char *)base;
	scr = (unsigned char *)logbase();
	wrap = V(2);
	VL(4) = (long)contrl; VL(8) = (long)intin; VL(12) = (long)ptsin;
	VL(16) = (long)intout; VL(20) = (long)ptsout;
	check("la_vars", V(0) == 8 && wrap >= V(-12) && V(-2) == wrap && V(-12) >= 640 &&
	    V(-4) >= 400, (long)V(0) * 10000 + wrap);
	if (V(0) != 8 || V(-12) < 640 || V(-4) < 400)
		return;
	box(0, 0, 639, 399, 0);
	check("la_rect", bad(0, 0, 639, 399, 0) == 0 && (box(100, 100, 149, 139, 2),
	    bad(100, 100, 149, 139, 2) == 0 && PX(99, 100) == 0 && PX(150, 139) == 0),
	    bad(100, 100, 149, 139, 2));
	/* clipped */
	V(54) = 1; V(56) = 0; V(58) = 0; V(60) = 104; V(62) = 104;
	setcol(9); V(38) = 100; V(40) = 100; V(42) = 110; V(44) = 110; lan(5);
	check("la_rect_clip", bad(100, 100, 104, 104, 9) == 0 && PX(105, 105) == 2, (long)PX(105, 105));
	V(54) = 0;
	box(100, 100, 149, 139, 2);
	intin[0] = 5; ptsin[0] = 10; ptsin[1] = 10; lan(1);
	n = lacall(2L, 0L, 0L, 0L, 0L, 0L);
	check("la_putget", PX(10, 10) == 5 && n == 5, n);
	setcol(15); V(34) = 0xffff; V(36) = 0;
	V(38) = 20; V(40) = 20; V(42) = 60; V(44) = 20; lan(3);
	V(38) = 20; V(40) = 22; V(42) = 40; V(44) = 42; lan(3);
	check("la_line", bad(20, 20, 60, 20, 15) == 0 && PX(30, 32) == 15 && PX(40, 42) == 15 &&
	    PX(31, 32) == 0, bad(20, 20, 60, 20, 15));
	setcol(3); V(38) = 20; V(42) = 60; V(40) = 30; VL(46) = (long)solid; V(50) = 0; V(52) = 0;
	lan(4);
	check("la_hline", bad(20, 30, 60, 30, 3) == 0 && PX(61, 30) == 0, bad(20, 30, 60, 30, 3));
	/* a triangle, scan line 60 crosses it from 220 to 240 */
	ptsin[0] = 200; ptsin[1] = 40; ptsin[2] = 260; ptsin[3] = 80; ptsin[4] = 200; ptsin[5] = 80;
	ptsin[6] = 200; ptsin[7] = 40; contrl[1] = 4;
	setcol(6); V(40) = 60; V(54) = 0; lan(6);
	check("la_polygon", bad(201, 60, 228, 60, 6) == 0 && PX(240, 60) == 0, bad(201, 60, 228, 60, 6));
	/* bitblt: the one-plane glyph onto the screen, then screen to screen */
	for (i = 0; i < 38; i++)
		bb[i] = 0;
	bb[0] = 16; bb[1] = 16; bb[2] = 8; bb[3] = 0xff; bb[4] = 0;
	((char *)bb)[10] = ((char *)bb)[11] = ((char *)bb)[12] = ((char *)bb)[13] = 3;
	*(long *)(bb + 9) = (long)glyph; bb[11] = 2; bb[12] = 2; bb[13] = 0;
	bb[14] = 300; bb[15] = 100; *(long *)(bb + 16) = (long)scr; bb[18] = 16; bb[19] = wrap; bb[20] = 2;
	lacall(7L, (long)bb, 0L, 0L, 0L, 0L);
	check("la_bitblt_mono", PX(300, 100) == 0xff && PX(301, 101) == 0 && PX(305, 106) == 0xff &&
	    PX(304, 104) == 0, (long)PX(301, 101));
	bb[0] = 50; bb[1] = 40; bb[7] = 100; bb[8] = 100; *(long *)(bb + 9) = (long)scr;
	bb[11] = 16; bb[12] = wrap; bb[13] = 2; bb[14] = 400; bb[15] = 100;
	lacall(7L, (long)bb, 0L, 0L, 0L, 0L);
	check("la_bitblt_screen", bad(400, 100, 449, 139, 2) == 0 && PX(450, 100) == 0,
	    bad(400, 100, 449, 139, 2));
	/* textblt: a character of the 8x16 system font */
	fh = ((short **)lafonts)[2];
	off = *(short **)(fh + 36);
	f = *(unsigned char **)(fh + 38);
	fw = fh[40];
	ch = 'H' - fh[18];
	VL(84) = (long)f; V(88) = fw; V(72) = off[ch]; V(74) = 0;
	V(80) = off[ch + 1] - off[ch]; V(82) = fh[41];
	V(76) = 500; V(78) = 20; V(90) = 0; V(106) = 255; V(114) = 0; V(36) = 0; V(54) = 0;
	lan(8);
	for (n = 0, t = 0; t < V(82); t++)
		for (i = 0; i < V(80); i++)
			n += (PX(500 + i, 20 + t) != 0) !=
			    ((f[t * fw + ((off[ch] + i) >> 3)] & 0x80 >> ((off[ch] + i) & 7)) != 0);
	check("la_textblt", n == 0 && bad(500, 20, 500 + V(80) - 1, 20 + V(82) - 1, 0) > 4, n);
	/* the pointer: hide and show keep their count */
	n = V(-0x256);
	lan(10);
	t = V(-0x256);
	intin[0] = 1;
	lan(9);
	check("la_mouse", t == n + 1 && V(-0x256) == n, t);
	src[0] = 0; src[1] = 0; src[2] = 1; src[3] = 0; src[4] = 1;
	for (i = 0; i < 37; i++)
		intin[i] = i < 5 ? src[i] : 0x0ff0;
	lan(11);
	check("la_mouse_form", 1, 0L);
	/* a sprite drawn and taken away again */
	spr[0] = 0; spr[1] = 0; spr[2] = 1; spr[3] = 0; spr[4] = 7;
	for (i = 0; i < 16; i++) {
		spr[5 + 2 * i] = 0xffff;
		spr[6 + 2 * i] = 0xff00;
	}
	lacall(13L, 0L, 120L, 110L, (long)spr, (long)save);
	t = PX(120, 110) == 7 && PX(128, 110) == 0 && PX(135, 125) == 0;
	lacall(12L, 0L, 0L, 0L, 0L, (long)save);
	check("la_sprite", t && bad(100, 105, 149, 139, 2) == 0 && PX(128, 110) == 2, t);
	/* copy raster: screen to screen, then the glyph through vrt */
	mf = (short *)(save);
	for (i = 0; i < 20; i++)
		mf[i] = 0;
	mf[10] = 0; mf[11] = 0;
	*(long *)(mf + 10) = (long)glyph; mf[12] = 16; mf[13] = 16; mf[14] = 1; mf[15] = 0; mf[16] = 1;
	*(long *)(contrl + 7) = (long)mf; *(long *)(contrl + 9) = (long)mf;
	ptsin[0] = 100; ptsin[1] = 100; ptsin[2] = 149; ptsin[3] = 139; ptsin[4] = 400; ptsin[5] = 200;
	intin[0] = 3; V(116) = 0;
	lan(14);
	check("la_raster", bad(400, 200, 449, 239, 2) == 0, bad(400, 200, 449, 239, 2));
	*(long *)(contrl + 7) = (long)(mf + 10);
	ptsin[0] = 0; ptsin[1] = 0; ptsin[2] = 15; ptsin[3] = 15; ptsin[4] = 500; ptsin[5] = 200;
	intin[0] = 2; intin[1] = 1; intin[2] = 0; V(116) = 1;
	lan(14);
	check("la_raster_trans", PX(500, 200) == 0xff && PX(501, 201) == 0 && PX(505, 205) == 0xff,
	    (long)PX(500, 200));
	V(116) = 0;
	/* seed fill inside a frame */
	box(300, 300, 360, 340, 15);
	box(301, 301, 359, 339, 0);
	box(330, 301, 330, 320, 15);
	setcol(4); V(36) = 0; V(54) = 0; VL(46) = (long)solid; V(50) = 0; VL(118) = 0;
	intin[0] = -1; ptsin[0] = 310; ptsin[1] = 310;
	lan(15);
	check("la_seedfill", bad(301, 301, 329, 339, 4) == 0 && bad(331, 321, 359, 339, 4) == 0 &&
	    PX(330, 310) == 15 && PX(300, 300) == 15, bad(301, 301, 329, 339, 4));
	/* timings: calls for 40 ticks of the 200 Hz clock */
	bb[0] = 200; bb[1] = 200; bb[7] = 0; bb[8] = 200; bb[14] = 200; bb[15] = 200;
	V(78) = 420;
	for (k = 0; k < 3; k++) {
		t = hz200();
		for (i = 0; i < 20000 && hz200() - t < 40; i++)
			if (k == 0)
				box(0, 200, 199, 399, (int)i & 15);
			else if (k == 1)
				lacall(7L, (long)bb, 0L, 0L, 0L, 0L);
			else {
				V(76) = 8 * (i % 60);
				lan(8);
			}
		t = hz200() - t;
		info(k == 0 ? "la_rect_200x200_us" : k == 1 ? "la_blit_200x200_us" : "la_textblt_char_us",
		    t >= 40 ? t * 5000L / i : -i);
	}
	/* a screen of the program's own at this resolution stands for the frame buffer */
	box(300, 150, 315, 165, 0);
	setscreen((long)own, (long)own);
	t = logbase() == (long)own;
	for (i = 0; i < 38; i++)
		sb[i] = 0;
	sb[0] = 16; sb[1] = 16; sb[2] = 8; sb[3] = 0xff; sb[4] = 0;
	((char *)sb)[10] = ((char *)sb)[11] = ((char *)sb)[12] = ((char *)sb)[13] = 3;
	*(long *)(sb + 9) = (long)glyph; sb[11] = 2; sb[12] = 2; sb[13] = 0;
	sb[14] = 300; sb[15] = 150; *(long *)(sb + 16) = (long)own; sb[18] = 16; sb[19] = wrap; sb[20] = 2;
	lacall(7L, (long)sb, 0L, 0L, 0L, 0L);
	setscreen((long)scr, (long)scr);
	check("la_setscreen_alias", t && PX(300, 150) == 0xff && PX(301, 151) == 0 && PX(305, 156) == 0xff &&
	    logbase() == (long)scr, (long)PX(300, 150) + 1000 * t);
	/* console text and its blinking cursor stay off the screen */
	box(0, 0, V(-12) - 1, 79, 5);
	Cconws("\033E\033e\033Y++########\r\n########");
	for (t = hz200(); hz200() - t < 120; )
		;
	Cconws("\033f");
	check("la_console", bad(0, 0, V(-12) - 1, 79, 5) == 0 && V(0) == 8 && logbase() == (long)scr,
	    bad(0, 0, V(-12) - 1, 79, 5));
}

int
main()
{
	long h;

	tests();
	h = Fcreate("U:\\LINEA.TXT", 0);
	if (h >= 0) {
		Fwrite((int)h, (long)on, out);
		Fclose((int)h);
	}
	return 0;
}
