/* glprobe: lightmap texenv and mip LOD probes, readback printed */
#define GL_GLEXT_PROTOTYPES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>

#define WW 640
#define WH 480

static unsigned char tex[64 * 64 * 4];

static void
px(char *what, int x, int y)
{
	unsigned char p[4];

	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
	printf("glprobe: %s = %d %d %d\n", what, p[0], p[1], p[2]);
}

static void
quad2(int x, int y, int w, int h)
{
	glBegin(GL_QUADS);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0, 0);
	glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0, 0);
	glVertex2i(x, y);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 1, 0);
	glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 1, 0);
	glVertex2i(x + w, y);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 1, 1);
	glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 1, 1);
	glVertex2i(x + w, y + h);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0, 1);
	glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0, 1);
	glVertex2i(x, y + h);
	glEnd();
}

/* unit 0 REPLACE base (200,120,40), unit 1 BLEND lightmap, as Quake */
static void
lm(char *name, GLint internal, GLenum format, int bpp, unsigned char *t, int sub, int atest)
{
	GLuint n[2];
	int i, j;
	char buf[80];

	glGenTextures(2, n);
	glActiveTextureARB(GL_TEXTURE0_ARB);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, n[0]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	for (i = 0; i < 64; i++) {
		tex[i * 3] = 200; tex[i * 3 + 1] = 120; tex[i * 3 + 2] = 40;
	}
	glTexImage2D(GL_TEXTURE_2D, 0, 3, 8, 8, 0, GL_RGB, GL_UNSIGNED_BYTE, tex);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	glActiveTextureARB(GL_TEXTURE1_ARB);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, n[1]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	for (i = 0; i < 64; i++)
		for (j = 0; j < bpp; j++)
			tex[i * bpp + j] = sub ? 0 : t[j];
	glTexImage2D(GL_TEXTURE_2D, 0, internal, 8, 8, 0, format, GL_UNSIGNED_BYTE, tex);
	if (sub) {
		for (i = 0; i < 64; i++)
			for (j = 0; j < bpp; j++)
				tex[i * bpp + j] = t[j];
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, format, GL_UNSIGNED_BYTE, tex);
	}
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
	if (atest) {
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.666);
	}
	glClear(GL_COLOR_BUFFER_BIT);
	quad2(10, 10, 64, 64);
	glFinish();
	sprintf(buf, "lm %s%s%s", name, sub ? " sub" : "", atest ? " atest" : "");
	px(buf, 40, 40);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_TEXTURE_2D);
	glActiveTextureARB(GL_TEXTURE0_ARB);
	glDeleteTextures(2, n);
}

static unsigned char lc[][3] = {
	{ 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 },
	{ 0, 255, 255 }, { 255, 0, 255 }, { 255, 255, 255 },
};

/* 64x64 mipmapped, level N drawn at its own size; prints the level seen */
static void
mip(float bias, int q)
{
	GLuint n;
	int w, level, x, i, k;
	unsigned char p[4];

	glGenTextures(1, &n);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, n);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glTexEnvf(GL_TEXTURE_FILTER_CONTROL_EXT, GL_TEXTURE_LOD_BIAS_EXT, bias);
	for (w = 64, level = 0; w >= 1; w /= 2, level++) {
		for (i = 0; i < w * w; i++)
			memcpy(tex + i * 3, lc[level], 3);
		glTexImage2D(GL_TEXTURE_2D, level, 3, w, w, 0, GL_RGB, GL_UNSIGNED_BYTE, tex);
	}
	glClear(GL_COLOR_BUFFER_BIT);
	for (w = 64, x = 0; w >= 1; x += w + 4, w /= 2) {
		glBegin(GL_QUADS);
		/* q != 1 only scales s, t, q together: the same texels */
		glTexCoord4f(0, 0, 0, q); glVertex2i(x, 0);
		glTexCoord4f(q, 0, 0, q); glVertex2i(x + w, 0);
		glTexCoord4f(q, q, 0, q); glVertex2i(x + w, w);
		glTexCoord4f(0, q, 0, q); glVertex2i(x, w);
		glEnd();
	}
	glFinish();
	printf("glprobe: mip bias %g q %d:", bias, q);
	for (w = 64, x = 0, level = 0; w >= 1; x += w + 4, w /= 2, level++) {
		glReadPixels(x + w / 2, w / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
		for (k = 0; k < 7; k++)
			if (abs(p[0] - lc[k][0]) < 24 && abs(p[1] - lc[k][1]) < 24 &&
			    abs(p[2] - lc[k][2]) < 24)
				break;
		if (k < 7)
			printf(" %d", k);
		else
			printf(" ?%d,%d,%d", p[0], p[1], p[2]);
	}
	printf("\n");
	glTexEnvf(GL_TEXTURE_FILTER_CONTROL_EXT, GL_TEXTURE_LOD_BIAS_EXT, 0);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &n);
}

int
main(int argc, char **argv)
{
	static int attr[] = { GLX_RGBA, GLX_RED_SIZE, 1, GLX_GREEN_SIZE, 1,
	    GLX_BLUE_SIZE, 1, GLX_DOUBLEBUFFER, None };
	static unsigned char lum[] = { 64 }, rgba0[] = { 0, 0, 0, 64 },
	    rgba64[] = { 64, 64, 64, 64 }, la[] = { 64, 64 };
	Display *dpy;
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	XEvent ev;
	Window win;
	GLXContext ctx;
	int a;

	if ((dpy = XOpenDisplay(NULL)) == NULL ||
	    (vi = glXChooseVisual(dpy, DefaultScreen(dpy), attr)) == NULL) {
		printf("glprobe: no display\n");
		return 1;
	}
	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, WW, WH, 0, vi->depth,
	    InputOutput, vi->visual, CWColormap | CWBorderPixel, &swa);
	XSelectInput(dpy, win, StructureNotifyMask);
	XMapWindow(dpy, win);
	do
		XNextEvent(dpy, &ev);
	while (ev.type != MapNotify);
	ctx = glXCreateContext(dpy, vi, NULL, True);
	glXMakeCurrent(dpy, win, ctx);
	printf("glprobe: %s, %s\n", glGetString(GL_RENDERER),
	    glXIsDirect(dpy, ctx) ? "direct" : "indirect");
	glViewport(0, 0, WW, WH);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, WW, 0, WH, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	glClearColor(0, 0, 0, 1);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	printf("glprobe: base 200 120 40; lum 64 wants 150 90 30, rgba0 wants 200 120 40\n");
	for (a = 0; a < 2; a++) {
		lm("lum", GL_LUMINANCE, GL_LUMINANCE, 1, lum, 0, a);
		lm("lum", GL_LUMINANCE, GL_LUMINANCE, 1, lum, 1, a);
		lm("rgba0", 4, GL_RGBA, 4, rgba0, 0, a);
		lm("rgba0", 4, GL_RGBA, 4, rgba0, 1, a);
		lm("rgba64", 4, GL_RGBA, 4, rgba64, 0, a);
		lm("lumalpha", GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, 2, la, 0, a);
		lm("int", GL_INTENSITY, GL_LUMINANCE, 1, lum, 0, a);
	}
	mip(0, 1);
	mip(2, 1);
	mip(4, 1);
	mip(7, 1);
	mip(-2, 1);
	mip(0, 4);
	printf("glprobe: done\n");
	return 0;
}
