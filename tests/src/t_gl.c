/*
 * t_gl.c -- software GL: scenes with known pixels rendered into an off-screen
 * buffer (clear, smooth shading, depth test, lighting, texturing, mipmaps),
 * then the frame time of a lit, textured, depth-tested sphere and the vertex
 * time of glxgears' scene.
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

/*
 * a 16x4 texture mipmapped down to 1x1, a colour per level, each level
 * drawn at its own size; then half of level 0 replaced
 */
static void
test_mipmap()
{
	static unsigned char lv[5][3] = {
		{ 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 },
		{ 255, 255, 0 }, { 0, 255, 255 }
	};
	static unsigned char tex[16 * 4 * 4];
	char name[32];
	int lum, l, w, h, x, i;

	for (lum = 0; lum < 2; lum++) {
		reset();
		glMatrixMode(GL_PROJECTION);
		glOrtho(0, W, 0, H, -1, 1);
		glMatrixMode(GL_MODELVIEW);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glEnable(GL_TEXTURE_2D);
		for (l = 0, w = 16, h = 4; l < 5; l++, w /= 2, h = h > 1 ? h / 2 : 1) {
			for (i = 0; i < w * h; i++)
				if (lum)
					tex[i] = 40 + 40 * l;
				else
					memcpy(tex + i * 4, lv[l], 3), tex[i * 4 + 3] = 255;
			glTexImage2D(GL_TEXTURE_2D, l, lum ? GL_LUMINANCE : 3, w, h, 0,
			    lum ? GL_LUMINANCE : GL_RGBA, GL_UNSIGNED_BYTE, tex);
		}
		for (l = x = 0, w = 16, h = 4; l < 5; x += w + 1, l++, w /= 2, h = h > 1 ? h / 2 : 1)
			rect(x, 0, x + w, h, 0);
		glFinish();
		for (l = x = 0, w = 16, h = 4; l < 5; x += w + 1, l++, w /= 2, h = h > 1 ? h / 2 : 1) {
			sprintf(name, "mipmap.%s%d", lum ? "lum" : "rgb", l);
			if (lum)
				check(name, x + w / 2, h / 2, 40 + 40 * l, 40 + 40 * l, 40 + 40 * l, 1);
			else
				check(name, x + w / 2, h / 2, lv[l][0], lv[l][1], lv[l][2], 0);
		}
		memset(tex, 250, sizeof tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 2, 16, 2, lum ? GL_LUMINANCE : GL_RGBA,
		    GL_UNSIGNED_BYTE, tex);
		glClear(GL_COLOR_BUFFER_BIT);
		rect(0, 0, 16, 4, 0);
		glFinish();
		sprintf(name, "mipmap.%ssub", lum ? "lum" : "rgb");
		check(name, 8, 3, 250, 250, 250, 1);
		check(name, 8, 0, lum ? 40 : 255, lum ? 40 : 0, lum ? 40 : 0, 1);
	}
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

/* glxgears' scene: three flat-shaded, lit gears in display lists, every
 * face culled so the time is vertex work */
#define GFRAMES	100

static int nvert;

/* the gears' geometry as given: mode, then x y z nx ny nz per vertex */
#define LOGN	20000
static float glog[LOGN][6], gnrm[3];
static int glogn, gmode[LOGN], logging;

static void
v3(float x, float y, float z)
{
	glVertex3f(x, y, z);
	nvert++;
	if (logging && glogn < LOGN) {
		glog[glogn][0] = x;
		glog[glogn][1] = y;
		glog[glogn][2] = z;
		memcpy(glog[glogn] + 3, gnrm, sizeof gnrm);
		gmode[glogn++] = 0;
	}
}

static void
n3(float x, float y, float z)
{
	glNormal3f(x, y, z);
	gnrm[0] = x;
	gnrm[1] = y;
	gnrm[2] = z;
}

static void
begin(int mode)
{
	glBegin(mode);
	if (logging && glogn < LOGN)
		gmode[glogn++] = mode + 1;
}

static void
gear(float r0, float ro, float w, int teeth, float td)
{
	float r1 = ro - td / 2, r2 = ro + td / 2, da = 2 * M_PI / teeth / 4;
	float a, u, v, len, h = w * 0.5;
	int i, s;

	glShadeModel(GL_FLAT);
	for (s = 1; s >= -1; s -= 2) {
		n3(0, 0, s);
		begin(GL_QUAD_STRIP);
		for (i = 0; i <= teeth; i++) {
			a = i * 2 * M_PI / teeth;
			if (s > 0) {
				v3(r0 * cos(a), r0 * sin(a), h);
				v3(r1 * cos(a), r1 * sin(a), h);
				if (i < teeth) {
					v3(r0 * cos(a), r0 * sin(a), h);
					v3(r1 * cos(a + 3 * da), r1 * sin(a + 3 * da), h);
				}
			} else {
				v3(r1 * cos(a), r1 * sin(a), -h);
				v3(r0 * cos(a), r0 * sin(a), -h);
				if (i < teeth) {
					v3(r1 * cos(a + 3 * da), r1 * sin(a + 3 * da), -h);
					v3(r0 * cos(a), r0 * sin(a), -h);
				}
			}
		}
		glEnd();
		begin(GL_QUADS);
		for (i = 0; i < teeth; i++) {
			a = i * 2 * M_PI / teeth;
			if (s > 0) {
				v3(r1 * cos(a), r1 * sin(a), h);
				v3(r2 * cos(a + da), r2 * sin(a + da), h);
				v3(r2 * cos(a + 2 * da), r2 * sin(a + 2 * da), h);
				v3(r1 * cos(a + 3 * da), r1 * sin(a + 3 * da), h);
			} else {
				v3(r1 * cos(a + 3 * da), r1 * sin(a + 3 * da), -h);
				v3(r2 * cos(a + 2 * da), r2 * sin(a + 2 * da), -h);
				v3(r2 * cos(a + da), r2 * sin(a + da), -h);
				v3(r1 * cos(a), r1 * sin(a), -h);
			}
		}
		glEnd();
	}
	begin(GL_QUAD_STRIP);
	for (i = 0; i < teeth; i++) {
		a = i * 2 * M_PI / teeth;
		v3(r1 * cos(a), r1 * sin(a), h);
		v3(r1 * cos(a), r1 * sin(a), -h);
		u = r2 * cos(a + da) - r1 * cos(a);
		v = r2 * sin(a + da) - r1 * sin(a);
		len = sqrt(u * u + v * v);
		n3(v / len, -u / len, 0);
		v3(r2 * cos(a + da), r2 * sin(a + da), h);
		v3(r2 * cos(a + da), r2 * sin(a + da), -h);
		n3(cos(a), sin(a), 0);
		v3(r2 * cos(a + 2 * da), r2 * sin(a + 2 * da), h);
		v3(r2 * cos(a + 2 * da), r2 * sin(a + 2 * da), -h);
		u = r1 * cos(a + 3 * da) - r2 * cos(a + 2 * da);
		v = r1 * sin(a + 3 * da) - r2 * sin(a + 2 * da);
		n3(v, -u, 0);
		v3(r1 * cos(a + 3 * da), r1 * sin(a + 3 * da), h);
		v3(r1 * cos(a + 3 * da), r1 * sin(a + 3 * da), -h);
		n3(cos(a), sin(a), 0);
	}
	v3(r1, 0, h);
	v3(r1, 0, -h);
	glEnd();
	glShadeModel(GL_SMOOTH);
	begin(GL_QUAD_STRIP);
	for (i = 0; i <= teeth; i++) {
		a = i * 2 * M_PI / teeth;
		n3(-cos(a), -sin(a), 0);
		v3(r0 * cos(a), r0 * sin(a), -h);
		v3(r0 * cos(a), r0 * sin(a), h);
	}
	glEnd();
}

static void
test_gears(char *name)
{
	static GLfloat pos[4] = { 5, 5, 10, 0 };
	static GLfloat col[3][4] = { { 0.8, 0.1, 0, 1 }, { 0, 0.8, 0.2, 1 }, { 0.2, 0.2, 1, 1 } };
	static float at[3][3] = { { -3, -2, 0 }, { 3.1, -2, 0 }, { -3.1, 4.2, 0 } };
	static float g3[3][5] = { { 1, 4, 1, 20, 0.7 }, { 0.5, 2, 2, 10, 0.7 }, { 1.3, 2, 0.5, 10, 0.7 } };
	static float spin[3][2] = { { 1, 0 }, { -2, -9 }, { -2, -25 } };
	unsigned char *fb;
	OSMesaContext ctx;
	GLuint list[3];
	long t0, ms;
	int f, g;

	ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
	nvert = 0;
	fb = malloc(300 * 300 * 4);
	if (ctx == NULL || fb == NULL || !OSMesaMakeCurrent(ctx, fb, GL_UNSIGNED_BYTE, 300, 300)) {
		t_fail("gears.context", "no context");
		return;
	}
	glLightfv(GL_LIGHT0, GL_POSITION, pos);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT_AND_BACK);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_NORMALIZE);
	glMatrixMode(GL_PROJECTION);
	glFrustum(-1, 1, -1, 1, 5, 60);
	glMatrixMode(GL_MODELVIEW);
	for (g = 0; g < 3; g++) {
		list[g] = glGenLists(1);
		glNewList(list[g], GL_COMPILE);
		glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, col[g]);
		gear(g3[g][0], g3[g][1], g3[g][2], g3[g][3], g3[g][4]);
		glEndList();
	}
	t0 = t_now_ms();
	for (f = 0; f < GFRAMES; f++) {
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glLoadIdentity();
		glTranslatef(0, 0, -40);
		glRotatef(20, 1, 0, 0);
		glRotatef(30, 0, 1, 0);
		for (g = 0; g < 3; g++) {
			glPushMatrix();
			glTranslatef(at[g][0], at[g][1], at[g][2]);
			glRotatef(f * 2 * spin[g][0] + spin[g][1], 0, 0, 1);
			glCallList(list[g]);
			glPopMatrix();
		}
		glFinish();
	}
	ms = t_now_ms() - t0;
	t_check("gears.error", glGetError() == GL_NO_ERROR, "GL error");
	t_info(name, "%d vertices: %ld us per frame, %ld ns per vertex", nvert,
	    ms * 1000 / GFRAMES, ms * 1000 / GFRAMES * 1000 / nvert);
	OSMesaDestroyContext(ctx);
	free(fb);
}

/* the assembly transform and clip test against the C, bit for bit */
typedef unsigned long (*xfcl3_fn)(const float *, const float *, int,
    float (*)[4], float (*)[4], unsigned char *, int);
unsigned long m68k_xfcl3(), m68k_xfcl3_c();
extern xfcl3_fn m68k_xfcl3_p;

#define NV	500

static float
rnd(float r)
{
	return (rand() % 20001 - 10000) * r / 10000;
}

static void
test_xfcl3()
{
	static float in[NV][4], clip[2][NV][4], ndc[2][NV][4];
	static unsigned char mask[2][NV];
	float m[16];
	unsigned long r[2];
	int i, k, bad = 0, clipped = 0;
	long t0, ms[2];

	srand(7);
	for (k = 0; k < 20 && !bad; k++) {
		for (i = 0; i < 16; i++)
			m[i] = rnd(k < 10 ? 2 : 50);
		for (i = 0; i < NV; i++) {
			in[i][0] = rnd(10);
			in[i][1] = rnd(10);
			in[i][2] = rnd(10);
		}
		r[0] = m68k_xfcl3_c(m, in[0], 16, clip[0], ndc[0], mask[0], NV);
		r[1] = m68k_xfcl3(m, in[0], 16, clip[1], ndc[1], mask[1], NV);
		clipped += r[0] >> 16;
		bad = r[0] != r[1] || memcmp(clip[0], clip[1], sizeof clip[0]) ||
		    memcmp(ndc[0], ndc[1], sizeof ndc[0]) || memcmp(mask[0], mask[1], NV);
	}
	t_check("xfcl3.match", !bad && clipped > 0 && clipped < k * NV,
	    "set %d: %lx %lx, %d clipped", k, r[0], r[1], clipped);
	for (k = 0; k < 2; k++) {
		t0 = t_now_ms();
		for (i = 0; i < 200; i++)
			(k ? m68k_xfcl3 : m68k_xfcl3_c)(m, in[0], 16, clip[0], ndc[0], mask[0], NV);
		ms[k] = t_now_ms() - t0;
	}
	t_info("xfcl3.time", "C %ld ns, assembly %ld ns per vertex",
	    ms[0] * 5000 / NV, ms[1] * 5000 / NV);
}

/* display lists packed for replay: the packing against the gears'
 * geometry, each CPU's assembly against the C, and the replay's time */
struct m68k_run {
	unsigned short nv, nfan;
};

#define STRIP	0x8000

struct m68k_cas {
	int nv, nn, nrun;
	float (*pos)[3];
	unsigned short *vn;
	float (*nrm)[3], (*unit)[3];
	struct m68k_run *run;
	unsigned short *fan;
	struct m68k_cas *head;
	float box[6];
	int ok, cont, open, has_last;
	float last[3];
	unsigned long *col, *rec;
};

struct m68k_lt {
	float vp[3], diff[3], base[3];
	unsigned long basecol, alpha;
};

static struct m68k_lt glt = { { 0.4, 0.4, 0.82 }, { 0.6, 0.06, 0 }, { 0.16, 0.02, 0 }, 0x28050000, 0xff };

typedef void (*lxf_fn)();
extern lxf_fn m68k_light_p;
void m68k_light_c();
typedef unsigned long *(*lfan_fn)();
void m68k_lxf_c(), m68k_asm_init();
unsigned long *m68k_lfan_c(), *m68k_lstrip();
extern lxf_fn m68k_lxf_p;
extern lfan_fn m68k_lfan_p;
extern void (*m68k_cas_built)();

#define NCAS	64
static struct m68k_cas *cas[NCAS];
static int ncas;

static void
built(struct m68k_cas *c)
{
	if (ncas < NCAS)
		cas[ncas++] = c;
}

static void
mul4(float *r, const float *a, const float *b)
{
	int i, j;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			r[4 * j + i] = a[i] * b[4 * j] + a[4 + i] * b[4 * j + 1] +
			    a[8 + i] * b[4 * j + 2] + a[12 + i] * b[4 * j + 3];
}

/* fans of the logged geometry against the packed ones; returns the
 * first difference or NULL */
static char *
cmp_packed(void)
{
	int i, j, k, n, s, c, r, f, nv, pv, v[4], fk, q, m, x[4];
	unsigned short *fan;

	c = r = f = q = 0;
	fan = ncas ? cas[0]->fan : NULL;
	for (i = 0; i < glogn; i = s + n) {
		if (!gmode[i])
			return "vertex outside a primitive";
		s = i + 1;
		for (n = 0; s + n < glogn && !gmode[s + n]; n++)
			;
		fk = gmode[i] - 1 == GL_QUADS ? n / 4 : n / 2 - 1;
		for (j = 0; j < fk; j++) {
			if (gmode[i] - 1 == GL_QUADS) {
				for (k = 0; k < 4; k++)
					v[k] = s + 4 * j + k;
				pv = v[3];
			} else {
				v[0] = s + 2 * j; v[1] = v[0] + 1;
				v[2] = pv = v[0] + 3; v[3] = v[0] + 2;
			}
			while (c < ncas && (r >= cas[c]->nrun)) {
				if (++c < ncas)
					fan = cas[c]->fan;
				r = f = 0;
			}
			if (c == ncas)
				return "fewer fans";
			nv = cas[c]->run[r].nv;
			m = nv & ~STRIP;
			if (nv & STRIP) {
				/* a strip's quad q: its fan's vertices */
				if (m < 7 || fan[1] != fan[2])
					return "strip";
				x[0] = fan[3 + 2 * q];
				x[1] = fan[2 + 2 * q];
				x[2] = fan[4 + 2 * q];
				x[3] = fan[5 + 2 * q];
			} else if (nv != 4)
				return "fan size";
			else
				for (k = 0; k < 4; k++)
					x[k] = fan[k + 1];
			if (memcmp(cas[c]->nrm[cas[c]->vn[fan[0] / 20]], glog[pv] + 3, 12))
				return "provoking normal";
			for (k = 0; k < 4; k++) {
				x[k] /= 20;
				if (memcmp(cas[c]->pos[x[k]], glog[v[k]], 12) ||
				    memcmp(cas[c]->nrm[cas[c]->vn[x[k]]], glog[v[k]] + 3, 12))
					return "vertex";
			}
			if ((nv & STRIP) && ++q < (m - 3) / 2)
				continue;
			q = 0;
			fan += m + 1;
			if (++f == cas[c]->run[r].nfan) {
				r++;
				f = 0;
			}
		}
	}
	for (; c < ncas; c++, r = 0)
		if (r < cas[c]->nrun)
			return "more fans";
	for (c = 0; c < ncas; c++)
		if (!cas[c]->head->ok)
			return "chain not replayable";
	return NULL;
}

static unsigned long fifo[2][60000];

/* one frame's replay of all packed cassettes */
static long
replay(float (*mv)[16], int *nv, int flat)
{
	unsigned long *f = fifo[0];
	int c, r, n, s, k;

	*nv = 0;
	for (c = 0; c < ncas; c++) {
		unsigned short *fan = cas[c]->fan;

		m68k_light_p(cas[c]->unit, cas[c]->nn, &glt, cas[c]->col);
		m68k_lxf_p(mv[c % 3], cas[c]->pos, cas[c]->nv, cas[c]->rec,
		    cas[c]->col, cas[c]->vn);
		*nv += cas[c]->nv;
		for (r = 0; r < cas[c]->nrun; r++) {
			n = cas[c]->run[r].nv & ~STRIP;
			if (cas[c]->run[r].nv & STRIP)
				for (s = 0; s < cas[c]->run[r].nfan; s++)
					for (k = 0; k < n; k += 15)
						f = m68k_lstrip(f, fan + s * (n + 1), k,
						    n - k > 15 ? 15 : n - k, 0x1234abcdUL,
						    cas[c]->rec, 5, flat);
			else
				f = m68k_lfan_p(f, fan, (int)cas[c]->run[r].nfan, n,
				    0x1234abcdUL, cas[c]->rec, 5, flat);
			fan += cas[c]->run[r].nfan * (n + 1);
		}
	}
	return f - fifo[0];
}

static void
test_list(void)
{
	static GLfloat pos[4] = { 5, 5, 10, 0 };
	static float g3[3][5] = { { 1, 4, 1, 20, 0.7 }, { 0.5, 2, 2, 10, 0.7 }, { 1.3, 2, 0.5, 10, 0.7 } };
	static float rec[2][2000][5], in[2000][3], m[16];
	static unsigned short vn[2000], fan[2000 * 5];
	unsigned long col[16], *e[2];
	float p[16], mv[16], mvp[3][16];
	unsigned char *fb;
	OSMesaContext ctx;
	char *why;
	int g, i, k, w, bad, nv, frames;
	long t0, ms, words;

	ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
	fb = malloc(300 * 300 * 4);
	if (ctx == NULL || fb == NULL || !OSMesaMakeCurrent(ctx, fb, GL_UNSIGNED_BYTE, 300, 300)) {
		t_fail("list.context", "no context");
		return;
	}
	glLightfv(GL_LIGHT0, GL_POSITION, pos);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_NORMALIZE);
	m68k_cas_built = built;
	ncas = glogn = 0;
	logging = 1;
	for (g = 0; g < 3; g++) {
		glNewList(glGenLists(1), GL_COMPILE);
		gear(g3[g][0], g3[g][1], g3[g][2], g3[g][3], g3[g][4]);
		glEndList();
	}
	logging = 0;
	m68k_cas_built = NULL;
	why = cmp_packed();
	t_check("list.pack", why == NULL && ncas > 0, "%d cassettes, %d logged: %s",
	    ncas, glogn, why ? why : "");

	/* each CPU's routines against the C on random input */
	srand(11);
	for (k = 0; k < 3; k++) {
		putenv(k == 0 ? "MESA_M68K=30" : k == 1 ? "MESA_M68K=40" : "MESA_M68K=60");
		m68k_asm_init();
		bad = 0;
		for (w = 0; w < 20 && !bad; w++) {
			for (i = 0; i < 16; i++)
				m[i] = rnd(w < 10 ? 2 : 50);
			for (i = 0; i < 16; i++)
				col[i] = rand() * 65599UL + rand();
			for (i = 0; i < 2000; i++) {
				in[i][0] = rnd(10);
				in[i][1] = rnd(10);
				in[i][2] = rnd(10);
				vn[i] = rand() % 16;
			}
			for (i = 0; i < 2000 * 5; i++)
				fan[i] = rand() % 1600 * 20;
			for (i = 0; i < 4; i++) {
				glt.vp[i % 3] = rnd(0.06);
				glt.diff[i % 3] = rnd(1.5);
				glt.base[i % 3] = rnd(0.5);
			}
			m68k_light_c(in, 2000, &glt, rec[0]);
			m68k_light_p(in, 2000, &glt, rec[1]);
			bad = memcmp(rec[0], rec[1], 2000 * 4) != 0;
			m68k_lxf_c(m, in, 2000, rec[0], col, vn);
			m68k_lxf_p(m, in, 2000, rec[1], col, vn);
			bad |= memcmp(rec[0], rec[1], sizeof rec[0]) != 0;
			for (i = 0; i < 4 && !bad; i++) {
				memset(fifo, 0, sizeof fifo);
				e[0] = m68k_lfan_c(fifo[0], fan, 1000, 3 + (i & 1), 0x5a5aa5a5UL, rec[0],
				    4 + (i >> 1), i & 1);
				e[1] = m68k_lfan_p(fifo[1], fan, 1000, 3 + (i & 1), 0x5a5aa5a5UL, rec[0],
				    4 + (i >> 1), i & 1);
				bad = e[0] - fifo[0] != e[1] - fifo[1] ||
				    memcmp(fifo[0], fifo[1], sizeof fifo[0]) != 0;
			}
			/* colourless vertices, colour written when it changes */
			for (i = 0; i < 2 && !bad; i++) {
				unsigned long reg[2][2];

				reg[0][0] = reg[1][0] = 0x12345678UL;
				reg[0][1] = reg[1][1] = i ?
				    ((unsigned long *)((char *)rec[0] + fan[0]))[2] : 0;
				memset(fifo, 0, sizeof fifo);
				e[0] = m68k_lfan_c(fifo[0], fan, 1000, 3 + i, 0x5a5aa5a5UL, rec[0],
				    3, (int)reg[0]);
				e[1] = m68k_lfan_p(fifo[1], fan, 1000, 3 + i, 0x5a5aa5a5UL, rec[0],
				    3, (int)reg[1]);
				bad = e[0] - fifo[0] != e[1] - fifo[1] || reg[0][1] != reg[1][1] ||
				    memcmp(fifo[0], fifo[1], sizeof fifo[0]) != 0;
			}
		}
		t_check(k == 0 ? "list.match30" : k == 1 ? "list.match40" : "list.match60",
		    !bad, "set %d differs", w);
	}

	/* replay time: glxgears' frame, C then each CPU's assembly */
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1, 1, -1, 1, 5, 60);
	glGetFloatv(GL_PROJECTION_MATRIX, p);
	glMatrixMode(GL_MODELVIEW);
	for (g = 0; g < 3; g++) {
		glLoadIdentity();
		glTranslatef(0, 0, -40);
		glRotatef(20, 1, 0, 0);
		glRotatef(30 + g * 10, 0, 1, 0);
		glGetFloatv(GL_MODELVIEW_MATRIX, mv);
		mul4(mvp[g], p, mv);
		for (i = 0; i < 16; i += 4) {
			mvp[g][i] = 150 * mvp[g][i] + 150 * mvp[g][i + 3];
			mvp[g][i + 1] = 150 * mvp[g][i + 1] + 150 * mvp[g][i + 3];
			mvp[g][i + 2] = 32767 * mvp[g][i + 2] + 32768 * mvp[g][i + 3];
		}
	}
	for (k = 0; k < 4; k++) {
		if (k == 0) {
			m68k_lxf_p = m68k_lxf_c;
			m68k_lfan_p = m68k_lfan_c;
			m68k_light_p = m68k_light_c;
		} else {
			putenv(k == 1 ? "MESA_M68K=30" : k == 2 ? "MESA_M68K=40" : "MESA_M68K=60");
			m68k_asm_init();
		}
		frames = 0;
		t0 = t_now_ms();
		do {
			words = replay(mvp, &nv, 1);
			frames++;
		} while ((ms = t_now_ms() - t0) < 2000);
		t_info(k == 0 ? "list.timec" : k == 1 ? "list.time30" : k == 2 ? "list.time40" : "list.time60",
		    "%d vertices, %ld fifo words: %ld us per frame, %ld ns per vertex",
		    nv, words, ms * 1000 / frames, ms * 1000 / frames * 1000 / nv);
	}
	putenv("MESA_M68K=");
	m68k_asm_init();
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
	test_mipmap();
	t_check("error", glGetError() == GL_NO_ERROR, "GL error");
	OSMesaDestroyContext(ctx);
	test_frame();
	test_gears("gears.time");
	m68k_xfcl3_p = m68k_xfcl3;
	test_gears("gears.asm");
	test_xfcl3();
	test_list();
	return t_done();
}
#endif
