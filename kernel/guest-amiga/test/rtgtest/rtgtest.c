/*
 * rtgtest: the display modes to SYS:T/rtgmodes.txt (id, depth, name);
 * then, for each display-sized mode, a screen of black, red, green, blue
 * and white bars for 20 s, its id and depth appended to SYS:T/rtgstep.txt
 * when it opens.
 */
typedef unsigned int U;
typedef unsigned char B;
extern U call(void *, int, U *);
#define EXEC execbase()
static void *execbase(void) { void *b; __asm__("move.l 4.w,%0" : "=a"(b)); return b; }
#define D0 0
#define D1 1
#define D2 2
#define D3 3
#define A0 4
#define A1 5
static U r[7];
static B line[128], buf[64];
static void *dos, *gfx, *intui;
static U ids[8], depths[8];

static U ex(void *base, int lvo) { U v = call(base, lvo, r); int i; for (i = 0; i < 7; i++) r[i] = 0; return v; }
static void *lib(const char *name) { r[A1] = (U)name; r[D0] = 39; return (void *)ex(EXEC, -552); }
static U n;
static void add(const char *s) { while (*s && n < sizeof line - 2) line[n++] = *s++; }
static void hex(U v) { int i; for (i = 7; i >= 0; i--) line[n++] = "0123456789abcdef"[v >> (4 * i) & 15]; }
static void dec(U v) { B d[12]; int i = 0; do d[i++] = '0' + v % 10; while (v /= 10); while (i) line[n++] = d[--i]; }
static U fopen(const char *name, U mode) { r[D1] = (U)name; r[D2] = mode; return ex(dos, -30); }
static void flush(U f) { line[n++] = '\n'; r[D1] = f; r[D2] = (U)line; r[D3] = n; ex(dos, -48); n = 0; }
static void fclose(U f) { r[D1] = f; ex(dos, -36); }

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

static void bars(U id, U depth)
{
	static U tags[12];
	static const U rgb[5] = { 0, 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };
	B *scr;
	U f, w, h, i;
	tags[0] = 0x80000032; tags[1] = id;		/* SA_DisplayID */
	tags[2] = 0x80000025; tags[3] = depth;	/* SA_Depth */
	tags[4] = 0x80000038; tags[5] = 1;		/* SA_Quiet */
	tags[6] = 0x80000036; tags[7] = 0;		/* SA_ShowTitle */
	tags[8] = 0;
	r[A0] = 0; r[A1] = (U)tags;
	scr = (B *)ex(intui, -612);
	if ((f = fopen("SYS:T/rtgstep.txt", 1004)) != 0) {
		r[D1] = f; r[D2] = 0; r[D3] = 1; ex(dos, -66);	/* to the end */
		hex(id); add(" "); dec(depth); add(scr ? " open" : " failed");
		flush(f);
		fclose(f);
	}
	if (!scr) return;
	w = *(unsigned short *)(scr + 12); h = *(unsigned short *)(scr + 14);
	for (i = 0; i < 5; i++) {
		r[A0] = (U)(scr + 44); r[D0] = i;
		r[D1] = (rgb[i] >> 16 & 255) * 0x01010101U;
		r[D2] = (rgb[i] >> 8 & 255) * 0x01010101U;
		r[D3] = (rgb[i] & 255) * 0x01010101U;
		ex(gfx, -852);
		r[A1] = (U)(scr + 84); r[D0] = i; ex(gfx, -342);
		r[A1] = (U)(scr + 84); r[D0] = w * i / 5; r[D1] = 0; r[D2] = w * (i + 1) / 5 - 1; r[D3] = h - 1;
		ex(gfx, -306);
	}
	r[D1] = 50 * 20; ex(dos, -198);
	r[A0] = (U)scr; ex(intui, -66);
}

void rtgtest(void)
{
	U f, id = 0xffffffffU, k = 0, i;
	dos = lib("dos.library");
	gfx = lib("graphics.library");
	intui = lib("intuition.library");
	if (!dos || !gfx || !intui || !(f = fopen("SYS:T/rtgmodes.txt", 1006)))
		return;
	for (;;) {
		r[D0] = id;
		if ((id = ex(gfx, -732)) == 0xffffffffU) break;
		r[A0] = 0; r[A1] = (U)buf; r[D0] = sizeof buf; r[D1] = 0x80001000; r[D2] = id;
		if (!ex(gfx, -756)) continue;
		hex(id); add(" "); dec(*(unsigned short *)(buf + 16)); add(" ");
		i = *(unsigned short *)(buf + 16);
		r[A0] = 0; r[A1] = (U)buf; r[D0] = sizeof buf; r[D1] = 0x80003000; r[D2] = id;
		if (ex(gfx, -756)) {
			buf[sizeof buf - 1] = 0;
			add((char *)buf + 16);
			if (has(buf + 16, "Display") && k < 8)
				ids[k] = id, depths[k++] = i;
		}
		flush(f);
	}
	fclose(f);
	for (i = 0; i < k; i++)
		bars(ids[i], depths[i]);
}
