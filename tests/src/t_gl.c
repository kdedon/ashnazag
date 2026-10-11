/*
 * t_gl.c -- software GL: scenes with known pixels rendered into an off-screen
 * buffer (clear, smooth shading, depth test, lighting, texturing), then the
 * frame time of a lit, textured, depth-tested sphere.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "t.h"

#ifdef NOGL
int
main()
{
	t_init("gl", 60);
	t_skip("gl", "built without the GL library");
	return t_done();
}
#else
#include <GL/gl.h>
#include <GL/osmesa.h>

#define W	64
#define H	64

static unsigned char buf[W * H * 4];

static unsigned char *
px(int x, int y)
{
	return buf + (y * W + x) * 4;
}

static int
near(int v, int want, int tol)
{
	return v >= want - tol && v <= want + tol;
}

/* pixel (x, y), bottom row 0, against r g b within tol */
static int
check(char *name, int x, int y, int r, int g, int b, int tol)
{
	unsigned char *p = px(x, y);

	return t_check(name, near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol),
	    "(%d,%d) is %d %d %d, want %d %d %d", x, y, p[0], p[1], p[2], r, g, b);
}

static void
rect(float x0, float y0, float x1, float y1, float z)
{
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex3f(x0, y0, z);
	glTexCoord2f(1, 0); glVertex3f(x1, y0, z);
	glTexCoord2f(1, 1); glVertex3f(x1, y1, z);
	glTexCoord2f(0, 1); glVertex3f(x0, y1, z);
	glEnd();
}

static void
reset()
{
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glDisable(GL_TEXTURE_2D);
	glShadeModel(GL_SMOOTH);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void
test_clear()
{
	glClearColor(0.25, 0.5, 0.75, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	check("clear.first", 0, 0, 64, 128, 191, 1);
	check("clear.last", W - 1, H - 1, 64, 128, 191, 1);
}

static void
test_smooth()
{
	unsigned char *c;

	reset();
	glBegin(GL_TRIANGLES);
	glColor3f(1, 0, 0); glVertex2f(-1, -1);
	glColor3f(0, 1, 0); glVertex2f(1, -1);
	glColor3f(0, 0, 1); glVertex2f(-1, 1);
	glEnd();
	glFinish();
	check("smooth.red", 1, 1, 245, 5, 5, 12);
	check("smooth.green", W - 3, 1, 5, 245, 5, 12);
	check("smooth.blue", 1, H - 3, 5, 5, 245, 12);
	c = px(W / 3, H / 3);
	t_check("smooth.mid", near(c[0], 85, 12) && near(c[1], 85, 12) && near(c[2], 85, 12),
	    "%d %d %d", c[0], c[1], c[2]);
	check("smooth.outside", W - 4, H - 4, 0, 0, 0, 0);
}

static void
test_depth()
{
	float z;

	reset();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glColor3f(0, 1, 0);
	rect(-1, -1, 0.5, 0.5, -0.5);
	glColor3f(1, 0, 0);
	rect(-0.5, -0.5, 1, 1, 0.5);
	glFinish();
	check("depth.near", 24, 24, 0, 255, 0, 0);
	check("depth.farvisible", 56, 56, 255, 0, 0, 0);
	check("depth.nearonly", 4, 4, 0, 255, 0, 0);
	z = -1;
	glReadPixels(24, 24, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &z);
	t_check("depth.read", fabs(z - 0.25) < 0.001, "depth %g, want 0.25", z);
}

static void
test_light()
{
	static GLfloat zero[4] = { 0, 0, 0, 1 }, one[4] = { 1, 1, 1, 1 };
	static GLfloat dir[4] = { 0, 0.8, 0.6, 0 }, red[4] = { 1, 0, 0, 1 };

	reset();
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
	glLightfv(GL_LIGHT0, GL_AMBIENT, zero);
	glLightfv(GL_LIGHT0, GL_DIFFUSE, one);
	glLightfv(GL_LIGHT0, GL_SPECULAR, zero);
	glLightfv(GL_LIGHT0, GL_POSITION, dir);
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, zero);
	glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, zero);
	glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, one);
	glNormal3f(0, 0, 1);
	rect(-1, -1, 0, 1, 0);
	glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, red);
	glNormal3f(0, 1, 0);
	rect(0, -1, 1, 1, 0);
	glFinish();
	/* N.L = 0.6 and 0.8 */
	check("light.diffuse", 16, 32, 153, 153, 153, 2);
	check("light.material", 48, 32, 204, 0, 0, 2);
}

static void
test_texture()
{
	static unsigned char tex[2 * 2 * 4] = {
		255, 0, 0, 255,		0, 255, 0, 255,
		0, 0, 255, 255,		255, 255, 255, 255
	};
	GLuint id;

	reset();
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);
	rect(-1, -1, 1, 1, 0);
	glFinish();
	check("texture.00", 16, 16, 255, 0, 0, 0);
	check("texture.10", 48, 16, 0, 255, 0, 0);
	check("texture.01", 16, 48, 0, 0, 255, 0);
	check("texture.11", 48, 48, 255, 255, 255, 0);
	/* modulate by the vertex colour */
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glColor3f(0.5, 0.5, 0.5);
	rect(-1, -1, 1, 1, 0);
	glFinish();
	check("texture.modulate", 48, 48, 128, 128, 128, 1);
	glDeleteTextures(1, &id);
}

/* a lit, textured, depth-tested sphere of 24 x 12 quads, turned each frame */
#define FW	160
#define FH	120
#define FRAMES	10

static void
sphere()
{
	int i, j, k;
	float a, b, x, y, z;

	for (j = 0; j < 12; j++) {
		glBegin(GL_QUAD_STRIP);
		for (i = 0; i <= 24; i++)
			for (k = 1; k >= 0; k--) {
				a = i * 2 * M_PI / 24;
				b = (j + k) * M_PI / 12 - M_PI / 2;
				x = cos(b) * cos(a);
				y = sin(b);
				z = cos(b) * sin(a);
				glNormal3f(x, y, z);
				glTexCoord2f(i / 24.0, (j + k) / 12.0);
				glVertex3f(x, y, z);
			}
		glEnd();
	}
}

static void
test_frame()
{
	static unsigned char tex[8 * 8 * 3];
	static GLfloat pos[4] = { 1, 1, 1, 0 };
	unsigned char *fb;
	OSMesaContext ctx;
	GLuint list;
	long t0, ms;
	int i, lit;

	ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
	fb = malloc(FW * FH * 4);
	if (ctx == NULL || fb == NULL || !OSMesaMakeCurrent(ctx, fb, GL_UNSIGNED_BYTE, FW, FH)) {
		t_fail("frame.context", "no context");
		return;
	}
	for (i = 0; i < 64; i++)
		memset(tex + i * 3, ((i ^ i >> 3) & 1) ? 255 : 64, 3);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 8, 8, 0, GL_RGB, GL_UNSIGNED_BYTE, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glLightfv(GL_LIGHT0, GL_POSITION, pos);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glMatrixMode(GL_PROJECTION);
	glFrustum(-0.8, 0.8, -0.6, 0.6, 2, 10);
	glMatrixMode(GL_MODELVIEW);
	list = glGenLists(1);
	glNewList(list, GL_COMPILE);
	sphere();
	glEndList();
	t0 = t_now_ms();
	for (i = 0; i < FRAMES; i++) {
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glLoadIdentity();
		glTranslatef(0, 0, -4);
		glRotatef(i * 9, 0.3, 1, 0);
		glCallList(list);
		glFinish();
	}
	ms = t_now_ms() - t0;
	t_check("frame.error", glGetError() == GL_NO_ERROR, "GL error");
	for (lit = i = 0; i < FW * FH; i++)
		lit += fb[i * 4] != 0;
	/* the sphere's disc covers about a third of the frame */
	t_check("frame.coverage", lit > FW * FH / 5 && lit < FW * FH / 2, "%d pixels drawn", lit);
	t_info("frame.time", "%dx%d, 576 quads: %ld ms per frame", FW, FH, ms / FRAMES);
	OSMesaDestroyContext(ctx);
	free(fb);
}

int
main()
{
	OSMesaContext ctx;

	t_init("gl", 300);
	ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
	if (!t_check("context", ctx != NULL, "OSMesaCreateContextExt failed"))
		return t_done();
	if (!t_check("makecurrent", OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, W, H),
	    "OSMesaMakeCurrent failed"))
		return t_done();
	t_info("renderer", "%s %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	glViewport(0, 0, W, H);
	test_clear();
	test_smooth();
	test_depth();
	test_light();
	test_texture();
	t_check("error", glGetError() == GL_NO_ERROR, "GL error");
	OSMesaDestroyContext(ctx);
	test_frame();
	return t_done();
}
#endif
