/*
 * w3dtest: Warp3D on the deepest display-sized mode (an 8-bit one gets a
 * W3D_Bitmap, shown through the palette): a navy clear, a red triangle
 * at (20,20)-(200,20)-(20,200), a 2x2 checker texture on the square
 * (260,20)-(388,148) (white top left) and, with the Z buffer, a green
 * triangle in front of a blue one drawn after it, at
 * (20,220)-(200,220)-(20,400).  Then 20 such frames, timed, and one more.
 * Results to a console window; the screen stays 10 s after "done", the
 * window 100 s after a failure.  Arguments: 16 or 32 takes the deepest
 * mode of that depth instead; "hold" then draws a frame every 0.1 s for
 * 60 s and counts the frames drawn and those refused as not visible.
 */
typedef unsigned int U;
typedef unsigned char B;
extern U call(void *, int, U *);
static void *execbase(void) { void *b; __asm__("move.l 4.w,%0" : "=a"(b)); return b; }
#define EXEC execbase()
#define D0 0
#define D1 1
#define D2 2
#define D3 3
#define A0 4
#define A1 5
#define A2 6
#define LVO(n) (-30 - 6 * (n))
static U r[7];
static B line[160], buf[64];
static void *dos, *gfx, *intui, *w3d;
static U n, out;

static U ex(void *base, int lvo) { U v = call(base, lvo, r); int i; for (i = 0; i < 7; i++) r[i] = 0; return v; }
static void *lib(const char *name, U v) { r[A1] = (U)name; r[D0] = v; return (void *)ex(EXEC, -552); }
static void add(const char *s) { while (*s && n < sizeof line - 2) line[n++] = *s++; }
static void dec(int v) { B d[12]; int i = 0; U u = v < 0 ? -v : v; if (v < 0) line[n++] = '-'; do d[i++] = '0' + u % 10; while (u /= 10); while (i) line[n++] = d[--i]; }
static void hex(U v) { int i; for (i = 7; i >= 0; i--) line[n++] = "0123456789abcdef"[v >> (4 * i) & 15]; }
static void flush(void) { line[n++] = '\n'; r[D1] = out; r[D2] = (U)line; r[D3] = n; ex(dos, -48); n = 0; }
static int has(const B *s, const char *w)
{
	int i, j;
	for (i = 0; s[i]; i++) {
		for (j = 0; w[j] && s[i + j] == (B)w[j]; j++)
			;
		if (!w[j]) return 1;
	}
	return 0;
}
static U ticks(void) { U ds[3]; r[D1] = (U)ds; ex(dos, -192); return ds[1] * 3000 + ds[2]; }

/* W3D_Vertex: 64 bytes */
struct vtx { float x, y; double z; float w, u, v, tex3d, c[4], spec[3], l; };
struct tri { struct vtx v[3]; void *tex; void *st; };
struct fan { int n; struct vtx *v; void *tex; void *st; };

static void set(struct vtx *v, int x, int y, double z, U argb, int u, int vv)
{
	v->x = x; v->y = y; v->z = z; v->w = 1;
	v->u = u; v->v = vv;
	v->c[0] = (argb >> 16 & 255) / 255.0f; v->c[1] = (argb >> 8 & 255) / 255.0f;
	v->c[2] = (argb & 255) / 255.0f; v->c[3] = 1;
}

static U ctx, tex;
/* the CLUT screen's way: a W3D_Bitmap of MW x MH, its colours pens 1-7 */
#define MW 400
#define MH 420
static U wbm[5];
static const U pal[7] = { 0x000080, 0xff0000, 0xffffff, 0x000000, 0x00ff00, 0x0000ff, 0xff00ff };

static U pen(U argb)
{
	U i;
	for (i = 0; i < 6 && pal[i] != (argb & 0xffffff); i++)
		;
	return i + 1;
}

/* the bitmap onto the screen's rastport, a run of one pen at a time */
static void show(B *rp)
{
	U *p = (U *)wbm[4], x, y, x0, c;
	for (y = 0; y < MH; y++)
		for (x = 0; x < MW; x = x0) {
			c = pen(p[y * MW + x]);
			for (x0 = x + 1; x0 < MW && pen(p[y * MW + x0]) == c; x0++)
				;
			r[A1] = (U)rp; r[D0] = c; ex(gfx, -342);	/* SetAPen */
			r[A1] = (U)rp; r[D0] = x; r[D1] = y; r[D2] = x0 - 1; r[D3] = y;
			ex(gfx, -306);					/* RectFill */
		}
}
static struct tri red, green, blue;
static struct vtx quad[4];
static struct fan qf;
static int zok;
static U W3D(int fn) { return ex(w3d, LVO(fn)); }

static void state(U s, U on) { r[A0] = ctx; r[D0] = s; r[D1] = on ? 1 : 2; W3D(3); }

static int frame(void)
{
	int e;
	double one = 1.0;
	r[A0] = ctx; if ((e = W3D(5)) != 0) return e;			/* LockHardware */
	r[A0] = ctx; r[D0] = 0xff000080; e = W3D(70);			/* ClearDrawRegion */
	state(1 << 8, 0);
	state(1 << 11, 0);
	r[A0] = ctx; r[A1] = (U)&red; e |= W3D(22);			/* DrawTriangle */
	state(1 << 8, 1);
	r[A0] = ctx; r[A1] = (U)&qf; e |= W3D(23);			/* DrawTriFan */
	state(1 << 8, 0);
	if (zok) {
		r[A0] = ctx; r[A1] = (U)&one; e |= W3D(33);		/* ClearZBuffer */
		state(1 << 11, 1);
		r[A0] = ctx; r[A1] = (U)&green; e |= W3D(22);
		r[A0] = ctx; r[A1] = (U)&blue; e |= W3D(22);
	}
	r[A0] = ctx; W3D(6);						/* UnLockHardware */
	r[A0] = ctx; W3D(67);						/* FlushFrame */
	return e;
}

static int word(const B *s, const char *w)
{
	int i;
	for (; *s && *s != '\n'; s++) {
		for (i = 0; w[i] && s[i] == (B)w[i]; i++)
			;
		if (!w[i]) return 1;
	}
	return 0;
}

void w3dtest(const B *arg)
{
	static U tags[12], checker[4] = { 0xffffffff, 0xff000000, 0xff000000, 0xffffffff };
	U id = 0xffffffffU, best = 0, bestd = 0, i, t0, t1, err, bm, want, drawn, hidden;
	volatile U *calls = (volatile U *)(0x25000000 + 400);
	B *scr;
	int e;

	dos = lib("dos.library", 39);
	gfx = lib("graphics.library", 39);
	intui = lib("intuition.library", 39);
	if (!dos || !gfx || !intui) return;
	want = word(arg, "16") ? 16 : word(arg, "32") ? 32 : 0;
	r[D1] = (U)"CON:0/360/640/200/w3dtest"; r[D2] = 1006;
	if (!(out = ex(dos, -30))) return;
	if (!(w3d = lib("Warp3D.library", 4))) {
		add("no Warp3D.library"); flush(); add("done"); flush();
		goto close;
	}
	for (;;) {
		r[D0] = id;
		if ((id = ex(gfx, -732)) == 0xffffffffU) break;
		r[A0] = 0; r[A1] = (U)buf; r[D0] = sizeof buf; r[D1] = 0x80001000; r[D2] = id;
		if (!ex(gfx, -756)) continue;
		i = *(unsigned short *)(buf + 16);
		r[A0] = 0; r[A1] = (U)buf; r[D0] = sizeof buf; r[D1] = 0x80003000; r[D2] = id;
		if (ex(gfx, -756) && (buf[sizeof buf - 1] = 0, has(buf + 16, "Display")) && i >= 8 && i > bestd &&
		    (!want || i == want || (want == 32 && i == 24)))
			best = id, bestd = i;
	}
	add("mode "); hex(best); add(" depth "); dec(bestd); flush();
	if (!best) goto fail;
	tags[0] = 0x80000032; tags[1] = best;		/* SA_DisplayID */
	tags[2] = 0x80000025; tags[3] = bestd;		/* SA_Depth */
	tags[4] = 0x80000038; tags[5] = 1;		/* SA_Quiet */
	tags[6] = 0x80000036; tags[7] = 0;		/* SA_ShowTitle */
	tags[8] = 0;
	r[A0] = 0; r[A1] = (U)tags;
	if (!(scr = (B *)ex(intui, -612))) { add("screen failed"); flush(); goto fail; }
	bm = *(U *)(scr + 84 + 4);			/* RastPort.BitMap */
	if (bestd == 8) {
		/* a CLUT screen: draw into memory, shown through the palette */
		for (i = 0; i < 7; i++) {
			r[A0] = (U)(scr + 44); r[D0] = i + 1;
			r[D1] = (pal[i] >> 16 & 255) * 0x01010101U;
			r[D2] = (pal[i] >> 8 & 255) * 0x01010101U;
			r[D3] = (pal[i] & 255) * 0x01010101U;
			ex(gfx, -852);			/* SetRGB32 */
		}
		r[D0] = MW * MH * 4; r[D1] = 0x10000;
		if (!(wbm[4] = ex(EXEC, -198))) { add("no memory"); flush(); goto screen; }
		wbm[0] = MW * 4; wbm[1] = MW; wbm[2] = MH; wbm[3] = 1 << 11;	/* W3D_FMT_A8R8G8B8 */
		bm = (U)wbm;
	}
	tags[0] = 0x80200000; tags[1] = bm;		/* W3D_CC_BITMAP */
	tags[2] = 0x80200002; tags[3] = 1 << 1;	/* W3D_CC_DRIVERTYPE: best */
	tags[4] = 0x80200003; tags[5] = bestd == 8;	/* W3D_CC_W3DBM */
	tags[6] = 0;
	r[A0] = (U)&err; r[A1] = (U)tags;
	ctx = W3D(0);
	add("context "); dec(ctx != 0); add(" error "); dec(err); flush();
	if (!ctx) {
		if (wbm[4]) { r[A1] = wbm[4]; r[D0] = MW * MH * 4; ex(EXEC, -210); }
		r[A0] = (U)scr; ex(intui, -66);
		goto fail;
	}
	r[A0] = ctx; zok = W3D(31) == 0;				/* AllocZBuffer */
	tags[0] = 0x80201000; tags[1] = (U)checker;	/* W3D_ATO_IMAGE */
	tags[2] = 0x80201001; tags[3] = 6;		/* A8R8G8B8 */
	tags[4] = 0x80201002; tags[5] = 2;
	tags[6] = 0x80201003; tags[7] = 2;
	tags[8] = 0;
	r[A0] = ctx; r[A1] = (U)&err; r[A2] = (U)tags;
	tex = W3D(11);
	add("texture "); dec(tex != 0); add(" error "); dec(err); flush();
	r[A0] = ctx; r[A1] = tex; r[D0] = 1; r[D1] = 1; W3D(15);	/* SetFilter nearest */
	r[A0] = ctx; r[A1] = tex; r[D1] = 1; W3D(16);			/* SetTexEnv replace */
	set(&red.v[0], 20, 20, 0.5, 0xff0000, 0, 0);
	set(&red.v[1], 200, 20, 0.5, 0xff0000, 0, 0);
	set(&red.v[2], 20, 200, 0.5, 0xff0000, 0, 0);
	set(&green.v[0], 20, 220, 0.2, 0x00ff00, 0, 0);
	set(&green.v[1], 200, 220, 0.2, 0x00ff00, 0, 0);
	set(&green.v[2], 20, 400, 0.2, 0x00ff00, 0, 0);
	set(&blue.v[0], 20, 220, 0.8, 0x0000ff, 0, 0);
	set(&blue.v[1], 200, 220, 0.8, 0x0000ff, 0, 0);
	set(&blue.v[2], 20, 400, 0.8, 0x0000ff, 0, 0);
	set(&quad[0], 260, 20, 0.5, 0xffffff, 0, 0);
	set(&quad[1], 388, 20, 0.5, 0xffffff, 2, 0);
	set(&quad[2], 388, 148, 0.5, 0xffffff, 2, 2);
	set(&quad[3], 260, 148, 0.5, 0xffffff, 0, 2);
	qf.n = 4; qf.v = quad; qf.tex = (void *)tex;
	e = frame();
	add("frame "); dec(e); add(" zbuffer "); dec(zok); flush();
	r[A0] = ctx; r[D0] = 0; r[D1] = 0; r[D2] = 0; add("stencil "); dec(W3D(30)); flush();
	t0 = ticks();
	for (i = 0; i < 20; i++) frame();
	t1 = ticks();
	add("frame_ms "); dec((t1 - t0) * 20 / 20); flush();
	frame();
	if (word(arg, "hold")) {
		drawn = hidden = 0;
		t0 = ticks();
		while (ticks() - t0 < 3000) {
			e = frame();
			if (e == -9) hidden++;			/* W3D_NOTVISIBLE */
			else if (e == 0) drawn++;
			r[D1] = 5; ex(dos, -198);
		}
		add("hold drawn "); dec(drawn); add(" notvisible "); dec(hidden); flush();
	}
	add("calls");
	for (i = 0; i < 84; i++)
		if (calls[i]) { add(" "); dec(i); add(":"); dec(calls[i]); }
	flush();
	if (wbm[4]) show(scr + 84);
	add("done"); flush();
	r[D1] = 50 * 10; ex(dos, -198);
	r[A0] = ctx; r[A1] = tex; W3D(12);
	r[A0] = ctx; W3D(1);
screen:
	if (wbm[4]) { r[A1] = wbm[4]; r[D0] = MW * MH * 4; ex(EXEC, -210); }
	r[A0] = (U)scr; ex(intui, -66);
	goto done;
fail:
	r[D1] = 50 * 100; ex(dos, -198);
done:
	r[A1] = (U)w3d; ex(EXEC, -414);
close:
	r[D1] = 50 * 5; ex(dos, -198);
	r[D1] = out; ex(dos, -36);
}
