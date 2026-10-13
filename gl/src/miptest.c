/*
 * miptest.c -- mipmapped texture uploads the way Quake makes them.
 *
 *   miptest [passes [format ...]]
 *
 * For each internal format Quake uses (3, 4, luminance, intensity, alpha,
 * luminance-alpha) and each size from 1x1 to 256x256, powers of two in
 * both directions, uploads every mip level down to 1x1, each level one
 * solid colour, then draws each level at its own size, nearest-mipmap-
 * nearest, and reads back its centre.  Level 0 is then half replaced
 * with glTexSubImage2D, as lightmaps are, and read again.  Textures are
 * kept, so later passes fill texture memory.  A pixel may be found
 * mirrored top to bottom.  One line per format and pass (with MIPV set,
 * per size too), then PASS or FAIL; a hang leaves the last line printed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include <GL/glx.h>

#define WW	640
#define WH	480
#define TOL	24

static struct fmt {
	char *name;
	GLint internal;
	GLenum format;
	int bpp;
} fmts[] = {
	{ "rgb", 3, GL_RGBA, 4 },
	{ "rgba", 4, GL_RGBA, 4 },
	{ "lum", GL_LUMINANCE, GL_LUMINANCE, 1 },
	{ "int", GL_INTENSITY, GL_LUMINANCE, 1 },
	{ "alpha", GL_ALPHA, GL_ALPHA, 1 },
	{ "lumalpha", GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, 2 },
};
#define NFMT	(sizeof fmts / sizeof fmts[0])

/* level colours: no black, which is the background */
static unsigned char rgb[][3] = {
	{ 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 },
	{ 0, 255, 255 }, { 255, 0, 255 }, { 255, 255, 255 }, { 255, 128, 0 },
	{ 0, 128, 255 }, { 128, 255, 128 },
};

static unsigned char tex[256 * 256 * 4];
static int fails, shown, verbose;

static int
gray(int c)
{
	return 40 + c * 21;
}

/* colour c as the texel bytes and as the pixel it should draw */
static void
texel(struct fmt *f, int c, unsigned char *t, unsigned char *want)
{
	int g = gray(c);

	switch (f->bpp) {
	case 4:
		t[0] = want[0] = rgb[c][0];
		t[1] = want[1] = rgb[c][1];
		t[2] = want[2] = rgb[c][2];
		t[3] = 255;
		break;
	case 2:
		t[0] = g; t[1] = 255;
		want[0] = want[1] = want[2] = g;
		break;
	default:
		t[0] = want[0] = want[1] = want[2] = g;
	}
}

static void
fill(struct fmt *f, int w, int h, int c)
{
	unsigned char t[4], want[3];
	int i, j;

	texel(f, c, t, want);
	for (i = 0; i < w * h; i++)
		for (j = 0; j < f->bpp; j++)
			tex[i * f->bpp + j] = t[j];
}

/* MIPQ: texture coordinates with q, as projected textures send them */
static void
rect(int x, int y, int w, int h)
{
	static int q = -1;

	if (q < 0)
		q = getenv("MIPQ") != NULL;
	glBegin(GL_QUADS);
	if (q) glTexCoord4f(0, 0, 0, 1); else glTexCoord2f(0, 0);
	glVertex2i(x, y);
	if (q) glTexCoord4f(1, 0, 0, 1); else glTexCoord2f(1, 0);
	glVertex2i(x + w, y);
	if (q) glTexCoord4f(1, 1, 0, 1); else glTexCoord2f(1, 1);
	glVertex2i(x + w, y + h);
	if (q) glTexCoord4f(0, 1, 0, 1); else glTexCoord2f(0, 1);
	glVertex2i(x, y + h);
	glEnd();
}

static void
check(struct fmt *f, int w, int h, int level, int x, int y, int c)
{
	unsigned char p[4], t[4], want[3];
	int i, bad = 0;

	texel(f, c, t, want);
	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
	for (i = 0; i < 3; i++)
		if (p[i] + TOL < want[i] || p[i] > want[i] + TOL)
			bad = 1;
	if (!bad)
		return;
	glReadPixels(x, WH - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, t);
	for (bad = i = 0; i < 3; i++)
		if (t[i] + TOL < want[i] || t[i] > want[i] + TOL)
			bad = 1;
	if (!bad)
		return;
	fails++;
	if (verbose || shown++ < 20)
		printf("miptest: %s %dx%d level %d at %d,%d is %d %d %d, want %d %d %d\n",
		    f->name, w, h, level, x, y, p[0], p[1], p[2], want[0], want[1], want[2]);
}

static void
one(struct fmt *f, int w, int h)
{
	GLuint name;
	int lw, lh, level, x, n;

	if (verbose) {
		printf("miptest: %s %dx%d\n", f->name, w, h);
		fflush(stdout);
	}
	glGenTextures(1, &name);
	glBindTexture(GL_TEXTURE_2D, name);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	lw = w; lh = h;
	for (level = 0; ; level++) {
		fill(f, lw, lh, level);
		glTexImage2D(GL_TEXTURE_2D, level, f->internal, lw, lh, 0, f->format,
		    GL_UNSIGNED_BYTE, tex);
		if (lw == 1 && lh == 1)
			break;
		lw = lw > 1 ? lw / 2 : 1;
		lh = lh > 1 ? lh / 2 : 1;
	}
	n = level + 1;

	glClear(GL_COLOR_BUFFER_BIT);
	x = 0;
	lw = w; lh = h;
	for (level = 0; level < n; level++) {
		rect(x, 0, lw, lh);
		x += lw + 2;
		lw = lw > 1 ? lw / 2 : 1;
		lh = lh > 1 ? lh / 2 : 1;
	}
	glFinish();
	x = 0;
	lw = w; lh = h;
	for (level = 0; level < n; level++) {
		check(f, w, h, level, x + lw / 2, lh / 2, level);
		x += lw + 2;
		lw = lw > 1 ? lw / 2 : 1;
		lh = lh > 1 ? lh / 2 : 1;
	}

	/* the top half of level 0, as a lightmap update */
	if (h > 1) {
		fill(f, w, h - h / 2, n);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, h / 2, w, h - h / 2, f->format,
		    GL_UNSIGNED_BYTE, tex);
		glClear(GL_COLOR_BUFFER_BIT);
		rect(0, 0, w, h);
		glFinish();
		check(f, w, h, 0, w / 2, h / 4, 0);
		check(f, w, h, 0, w / 2, h - 1 - h / 4, n);
	}
}

int
main(int argc, char **argv)
{
	static int attr[] = { GLX_RGBA, GLX_RED_SIZE, 1, GLX_GREEN_SIZE, 1,
	    GLX_BLUE_SIZE, 1, GLX_DOUBLEBUFFER, None };
	Display *dpy;
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	XEvent ev;
	Window win;
	GLXContext ctx;
	int passes, pass, i, j, w, h, before;

	passes = argc > 1 ? atoi(argv[1]) : 1;
	verbose = getenv("MIPV") != NULL;
	if ((dpy = XOpenDisplay(NULL)) == NULL) {
		printf("miptest: no display\n");
		return 1;
	}
	if ((vi = glXChooseVisual(dpy, DefaultScreen(dpy), attr)) == NULL) {
		printf("miptest: no visual\n");
		return 1;
	}
	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, WW, WH, 0, vi->depth,
	    InputOutput, vi->visual, CWColormap | CWBorderPixel, &swa);
	XSelectInput(dpy, win, StructureNotifyMask);
	XMapWindow(dpy, win);
	/* a drawable has no clip rectangles, so draws nothing, until mapped */
	do
		XNextEvent(dpy, &ev);
	while (ev.type != MapNotify);
	ctx = glXCreateContext(dpy, vi, NULL, True);
	glXMakeCurrent(dpy, win, ctx);
	XSync(dpy, False);
	printf("miptest: %s, %s\n", glGetString(GL_RENDERER),
	    glXIsDirect(dpy, ctx) ? "direct" : "indirect");
	fflush(stdout);

	glViewport(0, 0, WW, WH);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, WW, 0, WH, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	glClearColor(0, 0, 0, 1);
	glEnable(GL_TEXTURE_2D);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	for (pass = 0; pass < passes; pass++)
		for (i = 0; i < NFMT; i++) {
			struct fmt *f = &fmts[i];

			for (j = 2; j < argc && strcmp(argv[j], f->name); j++)
				;
			if (argc > 2 && j == argc)
				continue;
			before = fails;
			/* alpha alone shows as white faded by it */
			if (f->internal == GL_ALPHA) {
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ZERO);
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			} else {
				glDisable(GL_BLEND);
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
			}
			glColor3f(1, 1, 1);
			for (w = 1; w <= 256; w *= 2)
				for (h = 1; h <= 256; h *= 2)
					one(f, w, h);
			printf("miptest: pass %d %s: %d wrong\n", pass, f->name, fails - before);
			fflush(stdout);
		}
	glXSwapBuffers(dpy, win);
	printf("miptest: %s\n", fails ? "FAIL" : "PASS");
	return fails != 0;
}
