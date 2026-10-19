/*
 * w3d.c -- the host side of Warp3D.library: each library vector's
 * function, drawing with Mesa: on the card's 3D engine when the BitMap
 * is in the session's VRAM in a format the card draws, else with the
 * software renderer.  Called on the host stack from guest code, one
 * caller at a time.
 *
 * On the card Mesa draws straight into the BitMap.  In software it
 * draws into a back buffer in host memory: display memory can be slow
 * to read and write pixel by pixel.  The rows drawn go to the BitMap
 * at each flush and unlock; rows the guest may have drawn into since are
 * read back before Mesa draws over them, unless a clear covers them.
 *
 * Guest structures are read at the byte offsets of the 68k Amiga
 * layout (2-byte alignment).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

extern int gettimeofday();
#include <GL/gl.h>
#include <GL/osmesa.h>
#include "rtgshare.h"
#include "w3dshare.h"
#include "miglog.h"
#ifdef W3D_DRI
#include <sys/ioctl.h>
#include <sys/types.h>
#include "dsio.h"
#include "dri/drihost.h"
extern unsigned long migdisp_gsize;
#endif

/* the display session's /dev/fbN when its VRAM is the guest's card memory */
int w3d_fbfd = -1;
unsigned long w3d_textend;

#define L(p, o) (*(unsigned long *)((char *)(p) + (o)))
#define W(p, o) (*(unsigned short *)((char *)(p) + (o)))
#define F(p, o) (*(float *)((char *)(p) + (o)))
#define D(p, o) (*(double *)((char *)(p) + (o)))

#define TAG_DONE 0
#define TAG_IGNORE 1
#define TAG_MORE 2
#define TAG_SKIP 3
#define CC_BITMAP 0x80200000UL
#define CC_YOFFSET 0x80200001UL
#define CC_DRIVERTYPE 0x80200002UL
#define CC_W3DBM 0x80200003UL
#define CC_INDIRECT 0x80200004UL
#define CC_GLOBALTEXENV 0x80200005UL
#define CC_FAST 0x80200007UL
#define ATO_IMAGE 0x80201000UL
#define ATO_FORMAT 0x80201001UL
#define ATO_WIDTH 0x80201002UL
#define ATO_HEIGHT 0x80201003UL
#define ATO_MIPMAP 0x80201004UL
#define ATO_PALETTE 0x80201005UL
#define ATO_MIPMAPPTRS 0x80201006UL

/* results */
#define OK 0
#define ILLEGALINPUT -2
#define NOMEMORY -3
#define NOTEXTURE -5
#define NOTVISIBLE -9
#define UNSUPPORTEDSTATE -17
#define UNSUPPORTEDFMT -18
#define ILLEGALBITMAP -22
#define NOZBUFFER -23
#define NOPALETTE -24
#define UNSUPPORTEDTEXFMT -28
#define UNSUPPORTED -30

/* states */
#define S_TEXMAPPING (1L << 8)
#define S_PERSPECTIVE (1L << 9)
#define S_GOURAUD (1L << 10)
#define S_ZBUFFER (1L << 11)
#define S_ZBUFFERUPDATE (1L << 12)
#define S_BLENDING (1L << 13)
#define S_FOGGING (1L << 14)
#define S_DITHERING (1L << 19)
#define S_LOGICOP (1L << 20)
#define S_ALPHATEST (1L << 22)
#define S_SCISSOR (1L << 25)
#define S_CULLFACE (1L << 27)
#define S_HOST 0x000000feL	/* kept, no drawing effect */

/* destination formats: W3D_FMT_* */
#define FMT_R5G6B5 (1L << 5)
#define FMT_A8R8G8B8 (1L << 11)
#define FMT_B8G8R8A8 (1L << 14)

/* W3D_Context */
#define C_DRIVER 0
#define C_DRIVERTYPE 8
#define C_ZBUFFER 20
#define C_STATE 28
#define C_DRAWREGION 32
#define C_SUPPORTEDFMT 36
#define C_FORMAT 40
#define C_YOFFSET 44
#define C_BPROW 48
#define C_WIDTH 52
#define C_HEIGHT 56
#define C_DEPTH 60
#define C_ZBUFFERALLOC 68
#define C_HWLOCKED 72
#define C_W3DBITMAP 74
#define C_MAXTEXW 104
#define C_SCISSOR 120
#define C_ENVSUPMASK 160
#define C_DRAWMEM 168
#define C_FRONTFACE 628
#define C_SIZE 640

/* W3D_Texture */
#define T_TEXSOURCE 30
#define T_TEXFMTSRC 98
#define T_PALETTE 102
#define T_WIDTH 118
#define T_WIDTHEXP 122
#define T_HEIGHT 126
#define T_HEIGHTEXP 130
#define T_BPP 134
#define T_BPR 138
#define T_DRIVER 142
#define T_SIZE 146

/* W3D_Vertex: x 0, y 4, z 8 (double), w 16, u 20, v 24, color 32 */
#define V_SIZE 64

struct tex {
    GLuint name;
    char *obj;		/* the guest's W3D_Texture */
    float sw, sh;	/* texel to GL coordinate */
    struct tex *next;
};

/* textures hashed by their W3D_Texture */
#define NTEXH 64
#define TEXH(obj) (((unsigned long)(obj) >> 4) & (NTEXH - 1))

struct ctx {
    char *w3d;		/* the guest's W3D_Context */
    OSMesaContext gl;
    void *dri;		/* on the card instead */
    char *bitmap;
    unsigned char *mem;
    int bpr, w, h, yoff, bpp, rgbfmt;
    unsigned char *back;	/* w * bpp a row; 0: Mesa draws into mem */
    int y0, y1;		/* rows to write to the bitmap */
    int s0, s1;		/* rows holding the bitmap's content */
    int k0, k1;		/* rows drawn since the back buffer was cleared to kc */
    int kok;
    unsigned long kc;
    int z0, z1;		/* rows drawn since the Z buffer was cleared to zval */
    unsigned long zval;
    int cmask;		/* colour mask is all on */
    float pad;		/* widest line or point, in pixels */
    int affine;		/* texture coordinates without w */
    unsigned long backsz;
    unsigned long state;
    struct tex *texs[NTEXH], *cur;
    float color[4];
    /* V4 arrays */
    char *vp, *tp, *cp;
    int vstride, vmode, tstride, tvoff, twoff, tflags, cstride;
    unsigned long cmode;
};

static struct mig_w3d *tab;
static volatile struct mig_rtg *rtg = (volatile struct mig_rtg *)MIG_RTG_BASE;
static char *names[MIG_W3D_NFN];
static unsigned char seen[MIG_W3D_NFN];
static struct ctx *current;
static int drew;
static long frames, t0, copyms;

/* the W3D_Driver of the one driver, in memory the guest reads */
static struct { unsigned long chip, formats, name; unsigned short swdriver; } __attribute__((packed)) drv;
static unsigned long drvlist[2];

/* first in this file's text, which Mesa's follows */
void
w3d_text(void)
{
}

static int
unimpl(int n)
{
    if (!seen[n]) {
        seen[n] = 1;
        miglog(0, "Warp3D: %s not implemented", names[n] ? names[n] : "?");
    }
    return UNSUPPORTED;
}

static void
seterr(unsigned long *e, long v)
{
    if (e) *e = v;
}

static unsigned long
tag(unsigned long *t, unsigned long want, unsigned long def)
{
    while (t) {
        switch (t[0]) {
        case TAG_DONE: return def;
        case TAG_MORE: t = (unsigned long *)t[1]; continue;
        case TAG_SKIP: t += 2 * (t[1] + 1); continue;
        }
        if (t[0] == want) return t[1];
        t += 2;
    }
    return def;
}

#ifdef W3D_DRI
static int drigeom[4];	/* the screen the driver was given */

/* the BitMap in the session's VRAM, in the card's format */
static int
onboard(struct ctx *c)
{
    unsigned long a = (unsigned long)c->mem + c->yoff * c->bpr;
    return c->bpp == 2 && !((volatile struct mig_rtg *)MIG_RTG_BASE)->inram &&
        a >= MIG_RTG_VRAM && a + c->h * c->bpr <= MIG_RTG_VRAM + migdisp_gsize &&
        c->w <= drigeom[0] && c->h <= drigeom[1];
}

static int
dribind(struct ctx *c)
{
    return onboard(c) && drihost_bind(c->dri, (unsigned long)c->mem + c->yoff * c->bpr - MIG_RTG_VRAM,
        c->bpr, c->w, c->h);
}

#define DRI_ROOM (2048UL * 1024)

static int
roomy(unsigned long n)
{
    char *p = malloc(n);
    if (!p) return 0;
    free(p);
    return 1;
}

/* a context on the card, if the session has one with a DRI driver; W3D_SOFT set: software */
static void
dricontext(struct ctx *c)
{
    static int tried;
    static const char *name;
    struct fbinfo fi;
    const char *err = 0;

    if (w3d_fbfd < 0 || c->bpp != 2 || getenv("W3D_SOFT")) return;
    /* Mesa's driver leaves allocations unchecked: start it only with room */
    if (!roomy(DRI_ROOM)) {
        miglog(0, "Warp3D: no memory for direct rendering");
        return;
    }
    if (!tried) {
        tried = 1;
        if (ioctl(w3d_fbfd, FBIOGINFO, &fi) < 0) return;
        name = drihost_open(w3d_fbfd, fi.fi_offset, fi.fi_width, fi.fi_height, fi.fi_rowbytes,
            fi.fi_depth, &err);
        if (!name) {
            miglog(0, "Warp3D: no direct rendering: %s", err);
            return;
        }
        drigeom[0] = fi.fi_width; drigeom[1] = fi.fi_height;
    }
    if (!name) return;
    if (!onboard(c)) {
        miglog(0, "Warp3D: %lux%lu at %#lx, rows %lu: outside the session's card memory", c->w, c->h,
            (unsigned long)c->mem + c->yoff * c->bpr, c->bpr);
        return;
    }
    if (!(c->dri = drihost_create())) {
        miglog(0, "Warp3D: no context on the card");
        return;
    }
    if (!dribind(c)) {
        miglog(0, "Warp3D: context not bound to the card");
        drihost_destroy(c->dri);
        c->dri = 0;
        return;
    }
    current = c;
    miglog(0, "Warp3D: %s on the card: %s", name, glGetString(GL_RENDERER));
}
#else
#define dribind(c) 0
#endif

static int
make(struct ctx *c)
{
    if (c->dri) {
        if (!dribind(c)) return 0;
        current = c;
        return 1;
    }
    if (!OSMesaMakeCurrent(c->gl, c->back ? c->back : c->mem + c->yoff * c->bpr,
        c->bpp == 2 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE, c->w, c->h))
        return 0;
    OSMesaPixelStore(OSMESA_ROW_LENGTH, c->back ? c->w : c->bpr / c->bpp);
    OSMesaPixelStore(OSMESA_Y_UP, 0);
    current = c;
    return 1;
}

static int
use(char *w3d)
{
    struct ctx *c;
    if (!w3d || !(c = (struct ctx *)L(w3d, C_DRIVER)) || c->w3d != w3d)
        return 0;
    if (current != c && !make(c))
        return 0;
    return 1;
}

static long
ms(void)
{
    struct timeval tv;
    gettimeofday(&tv);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* rows a..b-1 from the back buffer to the bitmap (out) or back */
static void
copy(struct ctx *c, int a, int b, int out)
{
    int n = c->w * c->bpp;
    unsigned char *p = c->back + a * n, *m = c->mem + (c->yoff + a) * c->bpr;
    if (n == c->bpr) {
        if (out) memcpy(m, p, (b - a) * n); else memcpy(p, m, (b - a) * n);
        return;
    }
    for (; a < b; a++, p += n, m += c->bpr)
        if (out) memcpy(m, p, n); else memcpy(p, m, n);
}

static void
grow(int *lo, int *hi, int a, int b)
{
    if (*lo >= *hi) *lo = a, *hi = b;
    if (a < *lo) *lo = a;
    if (b > *hi) *hi = b;
}

/* Mesa is about to draw between rows y and e */
static void
touch(struct ctx *c, float y, float e)
{
    float lo = y - c->pad - 1, hi = e + c->pad + 2;
    int a, b;
    if (!(lo < hi)) return;
    a = lo < 0 ? 0 : lo > c->h ? c->h : (int)lo;
    b = hi < 0 ? 0 : hi > c->h ? c->h : (int)hi;
    if (a >= b) return;
    if (glIsEnabled(GL_DEPTH_TEST)) grow(&c->z0, &c->z1, a, b);
    if (!c->back) return;
    if (c->s0 >= c->s1) {
        copy(c, a, b, 0);
        c->s0 = a; c->s1 = b;
    } else {
        if (a < c->s0) { copy(c, a, c->s0, 0); c->s0 = a; }
        if (b > c->s1) { copy(c, c->s1, b, 0); c->s1 = b; }
    }
    grow(&c->y0, &c->y1, a, b);
    grow(&c->k0, &c->k1, a, b);
}

/* bitmap rows a..b-1 to the clear colour */
static void
fillrows(struct ctx *c, int a, int b)
{
    int n = c->w * c->bpp;
    unsigned char *m = c->mem + (c->yoff + a) * c->bpr;
    if (n == c->bpr) {
        if (a < b) fill32(m, c->kc, (b - a) * n);
        return;
    }
    for (; a < b; a++, m += c->bpr)
        fill32(m, c->kc, n);
}

/* rows still the clear colour are filled: nothing to read for them */
static void
copyout(struct ctx *c)
{
    int a = c->y0, b = c->y1, k0 = c->kok ? c->k0 : 0, k1 = c->kok ? c->k1 : c->h;
    long t;
    if (!c->back || a >= b || rtg->hidden) return;
    t = ms();
    if (k0 >= k1) k0 = k1 = b;
    fillrows(c, a, k0 < a ? a : k0 > b ? b : k0);
    if ((k0 > a ? k0 : a) < (k1 < b ? k1 : b))
        copy(c, k0 > a ? k0 : a, k1 < b ? k1 : b, 1);
    fillrows(c, k1 < a ? a : k1 > b ? b : k1, b);
    copyms += ms() - t;
    c->y0 = c->y1 = 0;
}

/* a back buffer the bitmap's size; none if memory is short */
static void
backbuf(struct ctx *c, int want)
{
    unsigned long n = (unsigned long)c->w * c->h * c->bpp;
    c->y0 = c->y1 = c->s0 = c->s1 = 0;
    if (c->back && want && c->backsz == n) return;
    c->kok = 0;
    if (c->back) free(c->back);
    c->back = want ? malloc(n) : 0;
    c->backsz = c->back ? n : 0;
}

/* the bitmap's layout: W3D_Bitmap, or what the library found */
static int
region(struct ctx *c, char *bm, int w3dbm)
{
    unsigned long f;
    if (!bm) return 0;
    if (w3dbm) {
        f = L(bm, 12);
        c->mem = (unsigned char *)L(bm, 16);
        c->bpr = L(bm, 0); c->w = L(bm, 4); c->h = L(bm, 8);
        c->rgbfmt = f == FMT_R5G6B5 ? MIG_RTG_R5G6B5 : f == FMT_A8R8G8B8 ? MIG_RTG_A8R8G8B8 :
            f == FMT_B8G8R8A8 ? MIG_RTG_B8G8R8A8 : -1;
    } else {
        if ((char *)tab->bm[0] != bm || !tab->bm[1]) return 0;
        c->mem = (unsigned char *)tab->bm[1];
        c->bpr = tab->bm[2]; c->rgbfmt = tab->bm[3];
        c->w = tab->bm[4]; c->h = tab->bm[5];
    }
    switch (c->rgbfmt) {
    case MIG_RTG_R5G6B5: c->bpp = 2; f = FMT_R5G6B5; break;
    case MIG_RTG_A8R8G8B8: c->bpp = 4; f = FMT_A8R8G8B8; break;
    case MIG_RTG_B8G8R8A8: c->bpp = 4; f = FMT_B8G8R8A8; break;
    default: return 0;
    }
    c->bitmap = bm;
    L(c->w3d, C_DRAWREGION) = (unsigned long)bm;
    L(c->w3d, C_FORMAT) = f;
    L(c->w3d, C_BPROW) = c->bpr;
    L(c->w3d, C_WIDTH) = c->w;
    L(c->w3d, C_HEIGHT) = c->h;
    L(c->w3d, C_DEPTH) = c->bpp * 8;
    L(c->w3d, C_YOFFSET) = c->yoff;
    L(c->w3d, C_DRAWMEM) = (unsigned long)c->mem;
    W(c->w3d, C_W3DBITMAP) = w3dbm;
    return 1;
}

static void
viewport(struct ctx *c)
{
    glViewport(0, 0, c->w, c->h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, c->w, c->h, 0, 0, -1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

#define MESA_ROOM (768UL * 1024)

static long
CreateContext(unsigned long *err, unsigned long *tags)
{
    struct ctx *c;
    char *w, *probe;
    unsigned long bm = tag(tags, CC_BITMAP, 0), w3dbm = tag(tags, CC_W3DBM, 0);
    GLenum fmt;

    if (!bm) {
        miglog(0, "Warp3D: CreateContext without a bitmap");
        seterr(err, ILLEGALBITMAP);
        return 0;
    }
    if (!(c = calloc(1, sizeof *c)) || !(w = calloc(1, C_SIZE))) {
        free(c);
        seterr(err, NOMEMORY);
        return 0;
    }
    c->w3d = w;
    c->yoff = tag(tags, CC_YOFFSET, 0);
    L(w, C_DRIVER) = (unsigned long)c;
    if (!region(c, (char *)bm, w3dbm != 0)) {
        miglog(0, "Warp3D: CreateContext: bitmap %#lx not in a 16- or 32-bit format", bm);
        free(w); free(c);
        seterr(err, UNSUPPORTEDFMT);
        return 0;
    }
#ifdef W3D_DRI
    dricontext(c);
#endif
    fmt = c->bpp == 2 ? OSMESA_RGB_565 : c->rgbfmt == MIG_RTG_A8R8G8B8 ? OSMESA_ARGB : OSMESA_BGRA;
    /*
     * Mesa leaves many allocations unchecked, and a null pointer here is
     * the guest's chip memory: create only with room for the context
     * (about 0.7 MB) and its depth buffer, as wide as a pixel (16 or 24
     * bits, as on the cards); the probe stays in the heap
     */
    if (c->dri)
        ;
    else if ((probe = malloc(MESA_ROOM + c->w * c->h * (c->bpp + c->bpp))) != 0) {
        free(probe);
        backbuf(c, 1);
    } else if ((probe = malloc(MESA_ROOM + c->w * c->h * c->bpp)) != 0)
        free(probe);
    else {
        miglog(0, "Warp3D: CreateContext: no memory for Mesa");
        free(w); free(c);
        seterr(err, NOMEMORY);
        return 0;
    }
    c->cmask = 1;
    c->zval = ~0UL;
    if (!c->dri && (!(c->gl = OSMesaCreateContextExt(fmt, c->bpp == 4 ? 24 : 16, 0, 0, 0)) || !make(c))) {
        miglog(0, "Warp3D: CreateContext: no Mesa context");
        if (c->gl) OSMesaDestroyContext(c->gl);
        if (c->back) free(c->back);
        free(w); free(c);
        seterr(err, NOMEMORY);
        return 0;
    }
    L(w, C_DRIVERTYPE) = c->dri ? 1 << 2 : 1 << 3;	/* 3D hardware, CPU */
    L(w, C_SUPPORTEDFMT) = FMT_R5G6B5 | FMT_A8R8G8B8 | FMT_B8G8R8A8;
    L(w, C_MAXTEXW) = L(w, C_MAXTEXW + 4) = L(w, C_MAXTEXW + 8) = L(w, C_MAXTEXW + 12) = 1024;
    L(w, C_ENVSUPMASK) = 0x1e;
    L(w, C_SCISSOR + 8) = c->w;
    L(w, C_SCISSOR + 12) = c->h;
    c->state = S_GOURAUD | S_ZBUFFERUPDATE | S_DITHERING | S_PERSPECTIVE | (1L << 1);
    if (tag(tags, CC_INDIRECT, 0)) c->state |= 1L << 3;
    if (tag(tags, CC_GLOBALTEXENV, 0)) c->state |= 1L << 4;
    if (tag(tags, CC_FAST, 0)) c->state |= 1L << 6;
    L(w, C_STATE) = c->state;
    c->color[0] = c->color[1] = c->color[2] = c->color[3] = 1;
    viewport(c);
    glShadeModel(GL_SMOOTH);
    glDepthFunc(GL_LESS);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    frames = 0;
    miglog(0, "Warp3D: context %dx%d, %d bytes a pixel, rows %d%s", c->w, c->h, c->bpp, c->bpr,
        c->dri ? ", on the card" : c->back ? "" : ", no back buffer");
    seterr(err, OK);
    return (long)w;
}

static void freetex(struct ctx *, struct tex *);
static void freetexs(struct ctx *);

/* each vector's calls so far, for the log */
static void
calls(void)
{
    char b[1024];
    int i, n = 0;
    for (i = 0; i < MIG_W3D_NFN && n < sizeof b - 40; i++)
        if (tab->calls[i])
            n += sprintf(b + n, " %s %u", names[i], tab->calls[i]);
    b[n] = 0;
    miglog(0, "Warp3D calls:%s", b);
}

static long
DestroyContext(char *w)
{
    struct ctx *c;
    if (!use(w)) return 0;
    c = (struct ctx *)L(w, C_DRIVER);
    calls();
    copyout(c);
    freetexs(c);
#ifdef W3D_DRI
    if (c->dri) drihost_destroy(c->dri);
    else
#endif
    OSMesaDestroyContext(c->gl);
    if (c->back) free(c->back);
    current = 0;
    c->w3d = 0;
    free(c);
    free(w);
    return 0;
}

static long
GetState(char *w, unsigned long s)
{
    if (!use(w)) return 2;
    return ((struct ctx *)L(w, C_DRIVER))->state & s ? 1 : 2;
}

static void
glstate(unsigned long s, int on)
{
    static const struct { unsigned long s; GLenum e; } map[] = {
        { S_TEXMAPPING, GL_TEXTURE_2D }, { S_ZBUFFER, GL_DEPTH_TEST }, { S_BLENDING, GL_BLEND },
        { S_FOGGING, GL_FOG }, { S_DITHERING, GL_DITHER }, { S_LOGICOP, GL_COLOR_LOGIC_OP },
        { S_ALPHATEST, GL_ALPHA_TEST }, { S_SCISSOR, GL_SCISSOR_TEST }, { S_CULLFACE, GL_CULL_FACE },
    };
    int i;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].s == s) {
            if (on) glEnable(map[i].e); else glDisable(map[i].e);
        }
    if (s == S_GOURAUD) glShadeModel(on ? GL_SMOOTH : GL_FLAT);
    if (s == S_ZBUFFERUPDATE) glDepthMask(on ? GL_TRUE : GL_FALSE);
}

static long
SetState(char *w, unsigned long s, unsigned long action)
{
    struct ctx *c;
    unsigned long known = S_HOST | S_TEXMAPPING | S_PERSPECTIVE | S_GOURAUD | S_ZBUFFER |
        S_ZBUFFERUPDATE | S_BLENDING | S_FOGGING | S_DITHERING | S_LOGICOP | S_ALPHATEST |
        S_SCISSOR | S_CULLFACE;
    if (!use(w)) return ILLEGALINPUT;
    c = (struct ctx *)L(w, C_DRIVER);
    if (s & ~known) return UNSUPPORTEDSTATE;
    if (action == 1) c->state |= s; else c->state &= ~s;
    glstate(s, action == 1);
    if (s == S_ZBUFFER && action == 1 && !W(w, C_ZBUFFERALLOC))
        glDisable(GL_DEPTH_TEST);
    L(w, C_STATE) = c->state;
    return OK;
}

static long
CheckDriver(void)
{
    return 1 << 3;
}

static long
LockHardware(char *w)
{
    if (!use(w)) return ILLEGALINPUT;
    if (rtg->hidden) return NOTVISIBLE;
    W(w, C_HWLOCKED) = 1;
    return OK;
}

static long
finish(char *w, int wait)
{
    struct ctx *c;
    if (!use(w)) return ILLEGALINPUT;
    c = (struct ctx *)L(w, C_DRIVER);
    /* the card's lock waits while the session is hidden */
    if (c->dri && rtg->hidden) return OK;
    if (wait) glFinish(); else glFlush();
#ifdef W3D_DRI
    /* cache lines read before the card drew would hide its pixels */
    if (wait && c->dri) ioctl(w3d_fbfd, FBIOSYNC, 0);
#endif
    copyout(c);
    /* unlocked, the guest may draw into the bitmap */
    if (!W(w, C_HWLOCKED)) c->s0 = c->s1 = 0;
    if (drew) {
        drew = 0;
        tab->ring = 1;
    }
    return OK;
}

static long UnLockHardware(char *w) { if (w) W(w, C_HWLOCKED) = 0; finish(w, 0); return 0; }
static long WaitIdle(char *w) { finish(w, 1); return 0; }
static long CheckIdle(char *w) { return OK; }
static long Flush(char *w) { return finish(w, 0); }

/* the first 20 frames' time, for the log */
static long
FlushFrame(char *w)
{
    finish(w, 1);
    if (frames == 0) t0 = ms(), copyms = 0;
    if (++frames == 21)
        miglog(0, "Warp3D: 20 frames, %ld ms per frame, %ld copying", (ms() - t0) / 20, copyms / 20);
    return 0;
}

static long
Query(char *w, unsigned long q, unsigned long fmt)
{
    switch (q) {
    case 111: case 112: case 113: case 114: return 1024;
    case 4: case 5: case 6: case 7: case 8: case 9: case 14: case 24: case 26: case 27: case 28:
    case 52: case 75: case 81: case 82: case 83: case 84: case 85: case 92: case 141:
    case 142: case 143: case 144: case 145: case 146: case 147:
        return 5;
    }
    return q >= 1 && q <= 162 ? 3 : 5;
}

static long
GetTexFmtInfo(char *w, unsigned long fmt, unsigned long dest)
{
    return fmt >= 1 && fmt <= 11 ? 1 | 1L << 18 : 2;
}

static long GetDriverTexFmtInfo(char *d, unsigned long fmt, unsigned long dest) { return GetTexFmtInfo(0, fmt, dest); }
static long QueryDriver(char *d, unsigned long q, unsigned long fmt) { return Query(0, q, fmt); }
static long GetDestFmt(void) { return FMT_R5G6B5 | FMT_A8R8G8B8 | FMT_B8G8R8A8; }
static long GetDrivers(void) { return (long)drvlist; }
static long GetDriverState(char *w) { return use(w) ? (rtg->hidden ? NOTVISIBLE : OK) : ILLEGALINPUT; }

/* ---- textures ---- */

static int
log2of(unsigned long n)
{
    int e = 0;
    while ((1UL << e) < n) e++;
    return (1UL << e) == n ? e : -1;
}

/* the image into the bound texture */
static int
teximage(char *obj, unsigned char *img, int level, unsigned long *pal, int x, int y, int w, int h, int bpr)
{
    unsigned long fmt = L(obj, T_TEXFMTSRC);
    unsigned long *argb;
    GLenum f, t;
    int i, j;

    switch (fmt) {
    case 1:	/* chunky: through the palette */
        if (!pal) return NOPALETTE;
        if (!(argb = malloc(w * h * 4))) return NOMEMORY;
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++)
                argb[j * w + i] = pal[img[j * bpr + i]];
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (level < 0)
            glTexSubImage2D(GL_TEXTURE_2D, -level - 1, x, y, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, argb);
        else
            glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, argb);
        free(argb);
        return OK;
    case 2: f = GL_BGRA; t = GL_UNSIGNED_SHORT_1_5_5_5_REV; i = 2; break;
    case 3: f = GL_RGB; t = GL_UNSIGNED_SHORT_5_6_5; i = 2; break;
    case 4: f = GL_RGB; t = GL_UNSIGNED_BYTE; i = 3; break;
    case 5: f = GL_BGRA; t = GL_UNSIGNED_SHORT_4_4_4_4_REV; i = 2; break;
    case 6: f = GL_BGRA; t = GL_UNSIGNED_INT_8_8_8_8_REV; i = 4; break;
    case 7: f = GL_ALPHA; t = GL_UNSIGNED_BYTE; i = 1; break;
    case 8: f = GL_LUMINANCE; t = GL_UNSIGNED_BYTE; i = 1; break;
    case 9: f = GL_LUMINANCE_ALPHA; t = GL_UNSIGNED_BYTE; i = 2; break;
    case 10: f = GL_INTENSITY; t = GL_UNSIGNED_BYTE; i = 1; break;
    case 11: f = GL_RGBA; t = GL_UNSIGNED_BYTE; i = 4; break;
    default: return UNSUPPORTEDTEXFMT;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, bpr / i);
    if (level < 0)
        glTexSubImage2D(GL_TEXTURE_2D, -level - 1, x, y, w, h, f, t, img);
    else
        glTexImage2D(GL_TEXTURE_2D, level, f == GL_INTENSITY ? GL_INTENSITY : f == GL_RGB ? GL_RGB : GL_RGBA,
            w, h, 0, f == GL_INTENSITY ? GL_LUMINANCE : f, t, img);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return OK;
}

static const unsigned char texbpp[12] = { 0, 1, 2, 2, 3, 2, 4, 1, 1, 2, 1, 4 };

static struct tex *
findtex(struct ctx *c, char *obj)
{
    struct tex *t;
    if (c->cur && c->cur->obj == obj) return c->cur;
    for (t = c->texs[TEXH(obj)]; t; t = t->next)
        if (t->obj == obj) return t;
    return 0;
}

static void
bindtex(struct ctx *c, struct tex *t)
{
    if (c->cur != t) {
        glBindTexture(GL_TEXTURE_2D, t ? t->name : 0);
        c->cur = t;
    }
}

static long
AllocTexObj(char *w, unsigned long *err, unsigned long *tags)
{
    struct ctx *c;
    struct tex *t;
    char *obj;
    unsigned long img = tag(tags, ATO_IMAGE, 0), fmt = tag(tags, ATO_FORMAT, 0);
    unsigned long tw = tag(tags, ATO_WIDTH, 0), th = tag(tags, ATO_HEIGHT, 0);
    unsigned long *mips = (unsigned long *)tag(tags, ATO_MIPMAPPTRS, 0);
    int e, lv, mw, mh;

    if (!use(w)) { seterr(err, ILLEGALINPUT); return 0; }
    c = (struct ctx *)L(w, C_DRIVER);
    if (!img || fmt < 1 || fmt > 11) { seterr(err, !img ? ILLEGALINPUT : UNSUPPORTEDTEXFMT); return 0; }
    if (log2of(tw) < 0 || log2of(th) < 0 || tw > 1024 || th > 1024) { seterr(err, -19); return 0; }
    if (!(t = calloc(1, sizeof *t)) || !(obj = calloc(1, T_SIZE))) {
        free(t);
        seterr(err, NOMEMORY);
        return 0;
    }
    t->obj = obj;
    L(obj, T_TEXSOURCE) = img;
    L(obj, T_TEXFMTSRC) = fmt;
    L(obj, T_PALETTE) = tag(tags, ATO_PALETTE, 0);
    L(obj, T_WIDTH) = tw; L(obj, T_WIDTHEXP) = log2of(tw);
    L(obj, T_HEIGHT) = th; L(obj, T_HEIGHTEXP) = log2of(th);
    L(obj, T_BPP) = texbpp[fmt];
    L(obj, T_BPR) = tw * texbpp[fmt];
    W(obj, 14) = 1;	/* resident */
    W(obj, 16) = mips != 0;
    t->sw = 1.0f / tw;
    t->sh = 1.0f / th;
    glGenTextures(1, &t->name);
    bindtex(c, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if ((e = teximage(obj, (unsigned char *)img, 0, (unsigned long *)L(obj, T_PALETTE), 0, 0, tw, th,
        tw * texbpp[fmt])) != OK) {
        glDeleteTextures(1, &t->name);
        c->cur = 0;
        free(obj); free(t);
        seterr(err, e);
        return 0;
    }
    for (lv = 1, mw = tw, mh = th; mips && (mw > 1 || mh > 1); lv++) {
        mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
        if (mips[lv - 1])
            teximage(obj, (unsigned char *)mips[lv - 1], lv, (unsigned long *)L(obj, T_PALETTE), 0, 0,
                mw, mh, mw * texbpp[fmt]);
    }
    t->next = c->texs[TEXH(obj)];
    c->texs[TEXH(obj)] = t;
    L(obj, T_DRIVER) = (unsigned long)t;
    seterr(err, OK);
    return (long)obj;
}

static void
freetex(struct ctx *c, struct tex *t)
{
    struct tex **p;
    for (p = &c->texs[TEXH(t->obj)]; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    if (c->cur == t) c->cur = 0;
    glDeleteTextures(1, &t->name);
    free(t->obj);
    free(t);
}

static void
freetexs(struct ctx *c)
{
    int i;
    for (i = 0; i < NTEXH; i++)
        while (c->texs[i]) freetex(c, c->texs[i]);
}

static long
FreeTexObj(char *w, char *obj)
{
    struct ctx *c;
    struct tex *t;
    if (!use(w)) return 0;
    c = (struct ctx *)L(w, C_DRIVER);
    if ((t = findtex(c, obj)) != 0) freetex(c, t);
    return 0;
}

static long
FreeAllTexObj(char *w)
{
    struct ctx *c;
    if (!use(w)) return 0;
    c = (struct ctx *)L(w, C_DRIVER);
    freetexs(c);
    return 0;
}

static long ReleaseTexture(char *w, char *obj) { return 0; }
static long FlushTextures(char *w) { return 0; }
static long UploadTexture(char *w, char *obj) { return OK; }

static struct tex *
pick(char *w, char *obj)
{
    struct ctx *c;
    struct tex *t;
    if (!use(w)) return 0;
    c = (struct ctx *)L(w, C_DRIVER);
    if ((t = findtex(c, obj)) != 0) bindtex(c, t);
    return t;
}

static long
SetFilter(char *w, char *obj, unsigned long min, unsigned long mag)
{
    static const GLenum f[7] = { 0, GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST,
        GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR };
    if (!pick(w, obj)) return NOTEXTURE;
    if (min < 1 || min > 6 || mag < 1 || mag > 2) return -10;
    /* without mipmaps a mipmapped filter would show nothing */
    if (min > 2 && !W(obj, 16)) min = min == 3 || min == 4 ? 1 : 2;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f[min]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f[mag]);
    return OK;
}

static long
SetTexEnv(char *w, char *obj, unsigned long env, float *color)
{
    static const GLenum e[5] = { 0, GL_REPLACE, GL_DECAL, GL_MODULATE, GL_BLEND };
    if (!use(w)) return ILLEGALINPUT;
    if (env < 1 || env > 4) return -11;
    if (obj) pick(w, obj);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, e[env]);
    if (color) glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
    return OK;
}

static long
SetWrapMode(char *w, char *obj, unsigned long s, unsigned long t, float *border)
{
    if (!pick(w, obj)) return NOTEXTURE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s == 2 ? GL_CLAMP : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t == 2 ? GL_CLAMP : GL_REPEAT);
    if (border) glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    return OK;
}

static long
UpdateTexImage(char *w, char *obj, unsigned char *img, long level, unsigned long *pal)
{
    int tw, th;
    if (!pick(w, obj)) return NOTEXTURE;
    tw = L(obj, T_WIDTH) >> level; th = L(obj, T_HEIGHT) >> level;
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (!level) L(obj, T_TEXSOURCE) = (unsigned long)img;
    if (pal) L(obj, T_PALETTE) = (unsigned long)pal;
    return teximage(obj, img, level, (unsigned long *)L(obj, T_PALETTE), 0, 0, tw, th,
        tw * texbpp[L(obj, T_TEXFMTSRC)]);
}

static long
UpdateTexSubImage(char *w, char *obj, unsigned char *img, long level, unsigned long *pal, long *sc, long srcbpr)
{
    if (!pick(w, obj)) return NOTEXTURE;
    if (!sc) return ILLEGALINPUT;
    if (pal) L(obj, T_PALETTE) = (unsigned long)pal;
    return teximage(obj, img, -level - 1, (unsigned long *)L(obj, T_PALETTE), sc[0], sc[1], sc[2], sc[3],
        srcbpr ? srcbpr : sc[2] * texbpp[L(obj, T_TEXFMTSRC)]);
}

/* ---- drawing ---- */

static int
drawable(char *w)
{
    if (!use(w)) return ILLEGALINPUT;
    if (rtg->hidden) return NOTVISIBLE;
    drew = 1;
    return OK;
}

/* the same w at every vertex needs no divide per pixel */
static void
affine(struct ctx *c, int on)
{
    if (on != c->affine) {
        glHint(GL_PERSPECTIVE_CORRECTION_HINT, on ? GL_FASTEST : GL_NICEST);
        c->affine = on;
    }
}

static void
vertex(struct ctx *c, char *v, struct tex *t)
{
    float q = c->state & S_PERSPECTIVE && !c->affine ? F(v, 16) : 1.0f;
    glColor4fv((float *)(v + 32));
    if (t)
        glTexCoord4f(F(v, 20) * t->sw * q, F(v, 24) * t->sh * q, 0, q);
    glVertex3f(F(v, 0), F(v, 4), (float)D(v, 8));
}

/* the texture a primitive names, bound; 0 when texturing is off */
static struct tex *
primtex(struct ctx *c, char *obj)
{
    struct tex *t = 0;
    if ((c->state & S_TEXMAPPING) && obj && (t = findtex(c, obj)) != 0)
        bindtex(c, t);
    if (!t) glDisable(GL_TEXTURE_2D);
    else if (c->state & S_TEXMAPPING) glEnable(GL_TEXTURE_2D);
    return t;
}

static long
prim(char *w, GLenum mode, char **vs, int n, char *obj)
{
    struct ctx *c;
    struct tex *t;
    int i, e, same = 1;
    float y0, y1;
    if ((e = drawable(w)) != OK) return e;
    c = (struct ctx *)L(w, C_DRIVER);
    for (i = 0, y0 = y1 = F(vs[0], 4); i < n; i++) {
        if (F(vs[i], 4) < y0) y0 = F(vs[i], 4);
        if (F(vs[i], 4) > y1) y1 = F(vs[i], 4);
        if (F(vs[i], 16) != F(vs[0], 16)) same = 0;
    }
    touch(c, y0, y1);
    if ((t = primtex(c, obj)) != 0) affine(c, same);
    glBegin(mode);
    for (i = 0; i < n; i++) vertex(c, vs[i], t);
    glEnd();
    return OK;
}

static long
DrawTriangle(char *w, char *tri)
{
    char *v[3];
    v[0] = tri; v[1] = tri + V_SIZE; v[2] = tri + 2 * V_SIZE;
    return prim(w, GL_TRIANGLES, v, 3, (char *)L(tri, 3 * V_SIZE));
}

static long
DrawTriangleV(char *w, char *tri)
{
    return prim(w, GL_TRIANGLES, (char **)tri, 3, (char *)L(tri, 12));
}

static long
many(char *w, GLenum mode, char *tris, int byref)
{
    int n = L(tris, 0), i;
    char *v = (char *)L(tris, 4), *vs[64], **p;
    float y0, y1;
    int same = 1;
    long e = OK;
    struct ctx *c;
    struct tex *t;
    if (n < 1) return ILLEGALINPUT;
    if (n <= 64 && !byref) {
        for (i = 0; i < n; i++) vs[i] = v + i * V_SIZE;
        return prim(w, mode, vs, n, (char *)L(tris, 8));
    }
    if (byref) return prim(w, mode, (char **)v, n, (char *)L(tris, 8));
    if ((e = drawable(w)) != OK) return e;
    c = (struct ctx *)L(w, C_DRIVER);
    for (i = 0, y0 = y1 = F(v, 4); i < n; i++) {
        if (F(v + i * V_SIZE, 4) < y0) y0 = F(v + i * V_SIZE, 4);
        if (F(v + i * V_SIZE, 4) > y1) y1 = F(v + i * V_SIZE, 4);
        if (F(v + i * V_SIZE, 16) != F(v, 16)) same = 0;
    }
    touch(c, y0, y1);
    if ((t = primtex(c, (char *)L(tris, 8))) != 0) affine(c, same);
    glBegin(mode);
    for (i = 0; i < n; i++) vertex(c, v + i * V_SIZE, t);
    glEnd();
    (void)p;
    return OK;
}

static long DrawTriFan(char *w, char *t) { return many(w, GL_TRIANGLE_FAN, t, 0); }
static long DrawTriStrip(char *w, char *t) { return many(w, GL_TRIANGLE_STRIP, t, 0); }
static long DrawTriFanV(char *w, char *t) { return many(w, GL_TRIANGLE_FAN, t, 1); }
static long DrawTriStripV(char *w, char *t) { return many(w, GL_TRIANGLE_STRIP, t, 1); }

static void
wide(char *w, float s, int line)
{
    struct ctx *c;
    if (!use(w)) return;
    c = (struct ctx *)L(w, C_DRIVER);
    if (s > c->pad) c->pad = s;
    if (line) glLineWidth(s); else glPointSize(s);
}

static long
DrawLine(char *w, char *l)
{
    char *v[2];
    wide(w, F(l, 2 * V_SIZE + 4), 1);
    v[0] = l; v[1] = l + V_SIZE;
    return prim(w, GL_LINES, v, 2, (char *)L(l, 2 * V_SIZE));
}

static long
DrawPoint(char *w, char *p)
{
    wide(w, F(p, V_SIZE + 4), 0);
    return prim(w, GL_POINTS, &p, 1, (char *)L(p, V_SIZE));
}

static long
lines(char *w, GLenum mode, char *l)
{
    wide(w, F(l, 12), 1);
    return many(w, mode, l, 0);
}

static long DrawLineStrip(char *w, char *l) { return lines(w, GL_LINE_STRIP, l); }
static long DrawLineLoop(char *w, char *l) { return lines(w, GL_LINE_LOOP, l); }

/* argb in the bitmap's format, twice over at 16 bits */
static unsigned long
pixel(struct ctx *c, unsigned long argb)
{
    unsigned long p;
    if (c->bpp == 2) {
        p = (argb >> 8 & 0xf800) | (argb >> 5 & 0x07e0) | (argb >> 3 & 0x001f);
        return p << 16 | p;
    }
    if (c->rgbfmt == MIG_RTG_A8R8G8B8) return argb;
    return (argb & 0xff) << 24 | (argb & 0xff00) << 8 | (argb >> 8 & 0xff00) | argb >> 24;
}

static long
ClearDrawRegion(char *w, unsigned long argb)
{
    struct ctx *c;
    int e;
    if ((e = drawable(w)) != OK) return e;
    c = (struct ctx *)L(w, C_DRIVER);
    if (c->state & S_SCISSOR)
        touch(c, (long)L(w, C_SCISSOR + 4), (long)L(w, C_SCISSOR + 4) + (long)L(w, C_SCISSOR + 12));
    else if (!c->cmask)
        touch(c, 0, c->h);
    else if (c->back) {
        /*
         * every row's content is new: nothing to read back, and only rows
         * drawn since a clear to the same colour need filling
         */
        unsigned long p = pixel(c, argb), n = c->w * c->bpp;
        if (!c->kok || p != c->kc) c->k0 = 0, c->k1 = c->h;
        if (c->k0 < c->k1) fill32(c->back + c->k0 * n, p, (c->k1 - c->k0) * n);
        c->kc = p;
        c->kok = 1;
        c->k0 = c->k1 = 0;
        c->s0 = c->y0 = 0;
        c->s1 = c->y1 = c->h;
        return OK;
    }
    glClearColor((argb >> 16 & 255) / 255.0f, (argb >> 8 & 255) / 255.0f, (argb & 255) / 255.0f,
        (argb >> 24 & 255) / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    return OK;
}

static long AllocZBuffer(char *w) { if (!use(w)) return ILLEGALINPUT; W(w, C_ZBUFFERALLOC) = 1; return OK; }

static long
FreeZBuffer(char *w)
{
    if (!use(w)) return ILLEGALINPUT;
    W(w, C_ZBUFFERALLOC) = 0;
    glDisable(GL_DEPTH_TEST);
    return OK;
}

static long
ClearZBuffer(char *w, double *z)
{
    struct ctx *c;
    GLint zw, zh, zb;
    void *zbuf;
    double d = z ? *z : 1.0;
    unsigned long v;
    if (!use(w)) return ILLEGALINPUT;
    if (!W(w, C_ZBUFFERALLOC)) return NOZBUFFER;
    c = (struct ctx *)L(w, C_DRIVER);
    if (c->dri && rtg->hidden) return NOTVISIBLE;
    d = d < 0 ? 0 : d > 1 ? 1 : d;
    if (!(c->state & S_SCISSOR) && !c->dri && OSMesaGetDepthBuffer(c->gl, &zw, &zh, &zb, &zbuf) && zh == c->h) {
        v = zb == 2 ? (unsigned short)(d * 65535.0) : (unsigned long)(d * 16777215.0);
        /* only rows drawn since a clear to the same value; Z rows count from the bottom */
        if (v != c->zval) c->z0 = 0, c->z1 = zh;
        if (c->z0 < c->z1)
            fill32((char *)zbuf + (zh - c->z1) * zw * zb, zb == 2 ? v << 16 | v : v, (c->z1 - c->z0) * zw * zb);
        c->zval = v;
        c->z0 = c->z1 = 0;
        return OK;
    }
    c->zval = ~0UL;
    glClearDepth(d);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthMask(c->state & S_ZBUFFERUPDATE ? GL_TRUE : GL_FALSE);
    return OK;
}

static long
SetZCompareMode(char *w, unsigned long m)
{
    static const GLenum f[9] = { 0, GL_NEVER, GL_LESS, GL_GEQUAL, GL_LEQUAL, GL_GREATER,
        GL_NOTEQUAL, GL_EQUAL, GL_ALWAYS };
    if (!use(w)) return ILLEGALINPUT;
    if (m < 1 || m > 8) return -13;
    glDepthFunc(f[m]);
    return OK;
}

static long
SetAlphaMode(char *w, unsigned long m, float *ref)
{
    static const GLenum f[9] = { 0, GL_NEVER, GL_LESS, GL_GEQUAL, GL_LEQUAL, GL_GREATER,
        GL_NOTEQUAL, GL_EQUAL, GL_ALWAYS };
    if (!use(w)) return ILLEGALINPUT;
    if (m < 1 || m > 8) return -14;
    glAlphaFunc(f[m], ref ? *ref : 0);
    return OK;
}

static long
SetBlendMode(char *w, unsigned long s, unsigned long d)
{
    static const GLenum f[12] = { 0, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_DST_COLOR,
        GL_ONE_MINUS_SRC_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
        GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE };
    if (!use(w)) return ILLEGALINPUT;
    if (s < 1 || s > 11 || d < 1 || d > 10) return -15;
    glBlendFunc(f[s], f[d]);
    return OK;
}

static long
SetFogParams(char *w, float *fog, unsigned long mode)
{
    static const GLenum m[4] = { 0, GL_LINEAR, GL_EXP, GL_EXP2 };
    if (!use(w)) return ILLEGALINPUT;
    if (mode < 1 || mode > 3) return -16;
    glFogi(GL_FOG_MODE, m[mode]);
    if (fog) {
        GLfloat col[4];
        glFogf(GL_FOG_START, fog[0]);
        glFogf(GL_FOG_END, fog[1]);
        glFogf(GL_FOG_DENSITY, fog[2]);
        col[0] = fog[3]; col[1] = fog[4]; col[2] = fog[5]; col[3] = 1;
        glFogfv(GL_FOG_COLOR, col);
    }
    return OK;
}

static long
SetColorMask(char *w, long r, long g, long b, long a)
{
    if (!use(w)) return ILLEGALINPUT;
    ((struct ctx *)L(w, C_DRIVER))->cmask = r && g && b && a;
    glColorMask(r != 0, g != 0, b != 0, a != 0);
    return OK;
}

static long
SetCurrentColor(char *w, float *col)
{
    if (!use(w) || !col) return ILLEGALINPUT;
    memcpy(((struct ctx *)L(w, C_DRIVER))->color, col, 16);
    glColor4fv(col);
    return OK;
}

static long
SetScissor(char *w, long *s)
{
    if (!use(w) || !s) return ILLEGALINPUT;
    memcpy(w + C_SCISSOR, s, 16);
    /* GL counts rows from the bottom */
    glScissor(s[0], ((struct ctx *)L(w, C_DRIVER))->h - s[1] - s[3], s[2], s[3]);
    return OK;
}

static long
SetDrawRegion(char *w, char *bm, long yoff, long *sc)
{
    struct ctx *c;
    int ow, oh;
    if (!use(w)) return ILLEGALINPUT;
    c = (struct ctx *)L(w, C_DRIVER);
    copyout(c);
    c->yoff = yoff;
    ow = c->w; oh = c->h;
    if (!region(c, bm, 0)) return ILLEGALBITMAP;
    if (c->w != ow || c->h != oh) c->zval = ~0UL;
    backbuf(c, c->back != 0);
    if (!make(c)) return NOMEMORY;
    viewport(c);
    if (sc) SetScissor(w, sc);
    return OK;
}

static long
SetLogicOp(char *w, unsigned long op)
{
    static const GLenum f[17] = { 0, GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED,
        GL_NOOP, GL_XOR, GL_OR, GL_NOR, GL_EQUIV, GL_INVERT, GL_OR_REVERSE, GL_COPY_INVERTED,
        GL_OR_INVERTED, GL_NAND, GL_SET };
    if (!use(w)) return ILLEGALINPUT;
    if (op < 1 || op > 16) return -20;
    glLogicOp(f[op]);
    return OK;
}

static long
Hint(char *w, unsigned long mode, unsigned long q)
{
    return use(w) ? OK : ILLEGALINPUT;
}

static long
SetFrontFace(char *w, unsigned long dir)
{
    if (!use(w)) return ILLEGALINPUT;
    L(w, C_FRONTFACE) = dir;
    /* y runs down the screen: clockwise there is counter-clockwise in GL */
    glFrontFace(dir == 0 ? GL_CCW : GL_CW);
    return OK;
}

/* ---- V4 arrays ---- */

static long
VertexPointer(char *w, char *p, long stride, unsigned long mode, unsigned long flags)
{
    struct ctx *c;
    if (!use(w)) return ILLEGALINPUT;
    c = (struct ctx *)L(w, C_DRIVER);
    c->vp = p; c->vstride = stride; c->vmode = mode;
    return OK;
}

static long
TexCoordPointer(char *w, char *p, long stride, long unit, long voff, long woff, unsigned long flags)
{
    struct ctx *c;
    if (!use(w)) return ILLEGALINPUT;
    if (unit) return OK;
    c = (struct ctx *)L(w, C_DRIVER);
    c->tp = p; c->tstride = stride; c->tvoff = voff; c->twoff = woff; c->tflags = flags;
    return OK;
}

static long
ColorPointer(char *w, char *p, long stride, unsigned long format, unsigned long mode, unsigned long flags)
{
    struct ctx *c;
    if (!use(w)) return ILLEGALINPUT;
    c = (struct ctx *)L(w, C_DRIVER);
    c->cp = p; c->cstride = stride; c->cmode = format | mode;
    return OK;
}

static long
BindTexture(char *w, unsigned long tmu, char *obj)
{
    struct ctx *c;
    if (!use(w)) return ILLEGALINPUT;
    if (tmu) return OK;
    c = (struct ctx *)L(w, C_DRIVER);
    c->cur = 0;
    L(w, 356) = (unsigned long)obj;
    if (obj && findtex(c, obj)) bindtex(c, findtex(c, obj));
    return OK;
}

static void
element(struct ctx *c, struct tex *t, unsigned long i)
{
    char *v = c->vp + i * c->vstride, *a;
    float x, y, z, q = 1, rgba[4];
    if (c->cp) {
        a = c->cp + i * c->cstride;
        if ((c->cmode >> 30) == 1) {
            memcpy(rgba, a, 16);
            if (c->cmode & 0x08) { x = rgba[0]; rgba[0] = rgba[1]; rgba[1] = rgba[2]; rgba[2] = rgba[3]; rgba[3] = x; }
        } else {
            unsigned char *b = (unsigned char *)a;
            int k;
            for (k = 0; k < 4; k++) rgba[k] = b[k] / 255.0f;
            if (c->cmode & 0x08) { x = rgba[0]; rgba[0] = rgba[1]; rgba[1] = rgba[2]; rgba[2] = rgba[3]; rgba[3] = x; }
            if (c->cmode & 0x01) rgba[3] = 1;
        }
        if (c->cmode & (0x02 | 0x10)) { x = rgba[0]; rgba[0] = rgba[2]; rgba[2] = x; }
        glColor4fv(rgba);
    }
    if (c->vmode == 2) {
        x = D(v, 0); y = D(v, 8); z = D(v, 16);
    } else {
        x = F(v, 0); y = F(v, 4); z = c->vmode == 1 ? D(v, 8) : F(v, 8);
    }
    if (t && c->tp) {
        a = c->tp + i * c->tstride;
        if (c->state & S_PERSPECTIVE) q = F(a, c->twoff);
        if (c->tflags & 1)
            glTexCoord4f(F(a, 0) * q, F(a, c->tvoff) * q, 0, q);
        else
            glTexCoord4f(F(a, 0) * t->sw * q, F(a, c->tvoff) * t->sh * q, 0, q);
    }
    glVertex3f(x, y, z);
}

static float
elemy(struct ctx *c, unsigned long i)
{
    char *v = c->vp + i * c->vstride;
    return c->vmode == 2 ? D(v, 8) : F(v, 4);
}

/* rows the elements cover; idx 0 for base..base+n-1 */
static void
touchelems(struct ctx *c, unsigned long base, unsigned long type, unsigned long n, void *idx)
{
    unsigned long i, k;
    float y, y0 = 0, y1 = 0;
    if (!c->back) return;
    for (i = 0; i < n; i++) {
        k = !idx ? base + i : type == 0 ? ((unsigned char *)idx)[i] : type == 1 ?
            ((unsigned short *)idx)[i] : ((unsigned long *)idx)[i];
        y = elemy(c, k);
        if (!i || y < y0) y0 = y;
        if (!i || y > y1) y1 = y;
    }
    if (n) touch(c, y0, y1);
}

static const GLenum primmode[7] = { GL_TRIANGLES, GL_TRIANGLE_FAN, GL_TRIANGLE_STRIP, GL_POINTS,
    GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP };

static long
DrawArray(char *w, unsigned long prim, unsigned long base, unsigned long n)
{
    struct ctx *c;
    struct tex *t;
    unsigned long i;
    int e;
    if ((e = drawable(w)) != OK) return e;
    c = (struct ctx *)L(w, C_DRIVER);
    if (prim > 6 || !c->vp) return ILLEGALINPUT;
    touchelems(c, base, 0, n, 0);
    if ((t = primtex(c, (char *)L(w, 356))) != 0) affine(c, 0);
    if (!c->cp) glColor4fv(c->color);
    glBegin(primmode[prim]);
    for (i = 0; i < n; i++) element(c, t, base + i);
    glEnd();
    return OK;
}

static long
DrawElements(char *w, unsigned long prim, unsigned long type, unsigned long n, void *idx)
{
    struct ctx *c;
    struct tex *t;
    unsigned long i;
    int e;
    if ((e = drawable(w)) != OK) return e;
    c = (struct ctx *)L(w, C_DRIVER);
    if (prim > 6 || type > 2 || !c->vp) return ILLEGALINPUT;
    touchelems(c, 0, type, n, idx);
    if ((t = primtex(c, (char *)L(w, 356))) != 0) affine(c, 0);
    if (!c->cp) glColor4fv(c->color);
    glBegin(primmode[prim]);
    for (i = 0; i < n; i++)
        element(c, t, type == 0 ? ((unsigned char *)idx)[i] : type == 1 ?
            ((unsigned short *)idx)[i] : ((unsigned long *)idx)[i]);
    glEnd();
    return OK;
}

/*
 * At the depth asked for, the smallest mode holding the size asked for,
 * else the largest; the display's without a size.  -1 if the display
 * lacks the depth.
 */
#define BMI_WIDTH 0x80203001UL
#define BMI_HEIGHT 0x80203002UL
#define BMI_DEPTH 0x80203003UL
extern unsigned long p96depth();
extern unsigned long amode[][3];
extern int amodes;

static long
BestModeID(unsigned long *tags)
{
    unsigned long d = tag(tags, BMI_DEPTH, 16), k = p96depth(d), id = 0x50011000UL, e, best = ~0UL;
    unsigned long w = tag(tags, BMI_WIDTH, 0), h = tag(tags, BMI_HEIGHT, 0), a;
    int i;
    if (d > 8 && !k) return -1L;
    for (i = 0; (w || h) && i < amodes; i++) {
        a = amode[i][1] * amode[i][2];
        e = amode[i][1] < w || amode[i][2] < h ? 0x80000000UL - a : a;
        if (e < best) best = e, id = amode[i][0];
    }
    miglog(0, "Warp3D: BestModeID %lux%lu, %lu bits: %#lx", w, h, d, id + k);
    return (long)(id + k);
}

/* ---- the table ---- */

#define U(n) static long u##n(void) { return unimpl(n); }
U(30) U(34) U(35) U(37) U(38) U(39) U(40) U(41) U(42) U(45) U(48) U(49) U(50) U(51) U(52)
U(53) U(54) U(56) U(65) U(68) U(69) U(74) U(75)

static const struct { int n; char *name; long (*fn)(); } fns[] = {
    { 0, "CreateContext", CreateContext }, { 1, "DestroyContext", DestroyContext },
    { 2, "GetState", GetState }, { 3, "SetState", SetState }, { 4, "CheckDriver", CheckDriver },
    { 5, "LockHardware", LockHardware }, { 6, "UnLockHardware", UnLockHardware },
    { 7, "WaitIdle", WaitIdle }, { 8, "CheckIdle", CheckIdle }, { 9, "Query", Query },
    { 10, "GetTexFmtInfo", GetTexFmtInfo }, { 11, "AllocTexObj", AllocTexObj },
    { 12, "FreeTexObj", FreeTexObj }, { 13, "ReleaseTexture", ReleaseTexture },
    { 14, "FlushTextures", FlushTextures }, { 15, "SetFilter", SetFilter },
    { 16, "SetTexEnv", SetTexEnv }, { 17, "SetWrapMode", SetWrapMode },
    { 18, "UpdateTexImage", UpdateTexImage }, { 19, "UploadTexture", UploadTexture },
    { 20, "DrawLine", DrawLine }, { 21, "DrawPoint", DrawPoint },
    { 22, "DrawTriangle", DrawTriangle }, { 23, "DrawTriFan", DrawTriFan },
    { 24, "DrawTriStrip", DrawTriStrip }, { 25, "SetAlphaMode", SetAlphaMode },
    { 26, "SetBlendMode", SetBlendMode }, { 27, "SetDrawRegion", SetDrawRegion },
    { 28, "SetFogParams", SetFogParams }, { 29, "SetColorMask", SetColorMask },
    { 30, "SetStencilFunc", u30 }, { 31, "AllocZBuffer", AllocZBuffer },
    { 32, "FreeZBuffer", FreeZBuffer }, { 33, "ClearZBuffer", ClearZBuffer },
    { 34, "ReadZPixel", u34 }, { 35, "ReadZSpan", u35 }, { 36, "SetZCompareMode", SetZCompareMode },
    { 37, "AllocStencilBuffer", u37 }, { 38, "ClearStencilBuffer", u38 },
    { 39, "FillStencilBuffer", u39 }, { 40, "FreeStencilBuffer", u40 },
    { 41, "ReadStencilPixel", u41 }, { 42, "ReadStencilSpan", u42 }, { 43, "SetLogicOp", SetLogicOp },
    { 44, "Hint", Hint }, { 45, "SetDrawRegionWBM", u45 }, { 46, "GetDriverState", GetDriverState },
    { 47, "Flush", Flush }, { 48, "SetPenMask", u48 }, { 49, "SetStencilOp", u49 },
    { 50, "SetWriteMask", u50 }, { 51, "WriteStencilPixel", u51 }, { 52, "WriteStencilSpan", u52 },
    { 53, "WriteZPixel", u53 }, { 54, "WriteZSpan", u54 }, { 55, "SetCurrentColor", SetCurrentColor },
    { 56, "SetCurrentPen", u56 }, { 57, "UpdateTexSubImage", UpdateTexSubImage },
    { 58, "FreeAllTexObj", FreeAllTexObj }, { 59, "GetDestFmt", GetDestFmt },
    { 60, "DrawLineStrip", DrawLineStrip }, { 61, "DrawLineLoop", DrawLineLoop },
    { 62, "GetDrivers", GetDrivers }, { 63, "QueryDriver", QueryDriver },
    { 64, "GetDriverTexFmtInfo", GetDriverTexFmtInfo }, { 65, "RequestMode", u65 },
    { 66, "SetScissor", SetScissor }, { 67, "FlushFrame", FlushFrame }, { 68, "TestMode", u68 },
    { 69, "SetChromaTestBounds", u69 }, { 70, "ClearDrawRegion", ClearDrawRegion },
    { 71, "DrawTriangleV", DrawTriangleV }, { 72, "DrawTriFanV", DrawTriFanV },
    { 73, "DrawTriStripV", DrawTriStripV }, { 74, "GetScreenmodeList", u74 },
    { 75, "FreeScreenmodeList", u75 }, { 76, "BestModeID", BestModeID },
    { 77, "VertexPointer", VertexPointer }, { 78, "TexCoordPointer", TexCoordPointer },
    { 79, "ColorPointer", ColorPointer }, { 80, "BindTexture", BindTexture },
    { 81, "DrawArray", DrawArray }, { 82, "DrawElements", DrawElements },
    { 83, "SetFrontFace", SetFrontFace },
};

typedef char fns_complete[sizeof fns / sizeof fns[0] == MIG_W3D_NFN ? 1 : -1];

/*
 * A null pointer in Mesa or here is guest chip memory, and a write at 4
 * replaces ExecBase: the guest would crash later with no trace of why
 */
static void
spoilt(unsigned long n, unsigned long was)
{
    miglog(1, "Warp3D: %s changed ExecBase at guest address 4 from %#lx to %#lx "
        "(a null pointer in host code); ending the session",
        n < MIG_W3D_NFN && names[n] ? names[n] : "?", was, *(volatile unsigned long *)4);
    exit(1);
}

void
w3d_init(struct mig_w3d *t)
{
    char *stack;
    int i;

    /* here, not in BSS, so the helpers forked before do not carry it */
    if (!(stack = malloc(MIG_W3D_STACK))) {
        miglog(0, "Warp3D: no memory for its stack");
        return;
    }

    tab = t;
    for (i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        t->fn[fns[i].n] = (unsigned int)fns[i].fn;
        names[fns[i].n] = fns[i].name;
    }
    drv.chip = 1;
    drv.formats = GetDestFmt();
    drv.name = (unsigned long)"Mesa software";
    drv.swdriver = 1;
    drvlist[0] = (unsigned long)&drv;
    t->spoilt = (unsigned int)spoilt;
    t->stack = ((unsigned int)(stack + MIG_W3D_STACK)) & ~15U;
    t->count = MIG_W3D_NFN;
    t->version = MIG_W3D_VERSION;
    t->magic = MIG_W3D_MAGIC;
#ifdef W3D_DRI
    {
        const char *err;

        /* the dynamic linker cannot run in guest context */
        if (w3d_fbfd >= 0 && !getenv("W3D_SOFT") && drihost_load(w3d_fbfd, &err))
            w3d_textend = drihost_textend();
    }
#endif
    miglog(0, "Warp3D: Mesa ready");
}
