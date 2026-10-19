/*
 * drihost.c -- direct rendering without an X server, for a program that
 * owns a display session: the calls a DRI driver's screen and drawable
 * code makes to the server, answered in process.
 *
 * One screen, the caller's session; one drawable, at the top left of
 * the screen, clipped to itself.  Its front buffer is wherever the
 * caller says (drihost_bind): the driver draws straight into that
 * place in VRAM.  The program's own DRM file is bound to the session
 * and authenticates the driver's file, as an X server does for its
 * clients.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "glxclient.h"
#include "dri_glx.h"
#include "xf86dri.h"
#include "xf86drm.h"
#include "sarea.h"
#include "amixdrirec.h"
#include "drihost.h"

extern void *_dlsym(void *, const char *);
extern void *__driHandle;

/* Glide's front buffer, when the driver draws with Glide */
static void (*front)(int offset, int stride);

#define DRAWABLE 1
#define VISUAL 1

static struct {
    int fd;			/* bound to the session */
    unsigned long base;		/* the session's memory in the frame buffer map */
    drmHandle hSAREA, hFB;
    XF86DRISAREAPtr sarea;
    char *busId;
    char name[32];
    AMIXDRIRec info;
    drmDrawable hw;
    int slot;			/* in the shared drawable table */
    int w, h;
    XF86DRIClipRectRec clip;
    Display dpy;
    Screen screen;
    __GLXvisualConfig config;
    __GLXscreenConfigs sc;
    __GLXdisplayPrivate priv;
    Visual visual;
    XVisualInfo vis;
} H;

/* what a server's GLX would have answered */
__GLXdisplayPrivate *
__glXInitialize(Display *dpy)
{
    return &H.priv;
}

__DRIscreen *
__glXFindDRIScreen(Display *dpy, int scrn)
{
    return scrn == 0 && H.sc.driScreen.private ? &H.sc.driScreen : 0;
}

int
XGetWindowAttributes(Display *dpy, Window w, XWindowAttributes *a)
{
    return 1;
}

int
XSync(Display *dpy, Bool discard)
{
    return 0;
}

XErrorHandler
XSetErrorHandler(XErrorHandler h)
{
    return 0;
}

int
XFree(void *p)
{
    free(p);
    return 0;
}

Bool
XF86DRIQueryVersion(Display *dpy, int *major, int *minor, int *patch)
{
    *major = 4;
    *minor = 0;
    *patch = 0;
    return True;
}

Bool
XF86DRIQueryDirectRenderingCapable(Display *dpy, int scrn, Bool *capable)
{
    *capable = scrn == 0 && H.fd >= 0;
    return True;
}

Bool
XF86DRIOpenConnection(Display *dpy, int scrn, drmHandlePtr hSAREA, char **busId)
{
    if (scrn != 0 || H.fd < 0 || (*busId = malloc(strlen(H.busId) + 1)) == 0)
	return False;
    strcpy(*busId, H.busId);
    *hSAREA = H.hSAREA;
    return True;
}

Bool
XF86DRIAuthConnection(Display *dpy, int scrn, drmMagic magic)
{
    return drmAuthMagic(H.fd, magic) == 0;
}

Bool
XF86DRICloseConnection(Display *dpy, int scrn)
{
    return True;
}

Bool
XF86DRIGetClientDriverName(Display *dpy, int scrn, int *major, int *minor, int *patch, char **name)
{
    *major = 1;
    *minor = 0;
    *patch = 0;
    if ((*name = malloc(strlen(H.name) + 1)) == 0)
	return False;
    strcpy(*name, H.name);
    return True;
}

Bool
XF86DRICreateContext(Display *dpy, int scrn, Visual *visual, XID *id, drmContextPtr hw)
{
    static XID next = 1;

    if (drmCreateContext(H.fd, hw) < 0)
	return False;
    *id = next++;
    return True;
}

Bool
XF86DRIDestroyContext(Display *dpy, int scrn, XID id)
{
    return True;
}

Bool
XF86DRICreateDrawable(Display *dpy, int scrn, Drawable draw, drmDrawablePtr hw)
{
    if (draw != DRAWABLE)
	return False;
    *hw = H.hw;
    return True;
}

Bool
XF86DRIDestroyDrawable(Display *dpy, int scrn, Drawable draw)
{
    return True;
}

Bool
XF86DRIGetDrawableInfo(Display *dpy, int scrn, Drawable draw, unsigned int *index,
		       unsigned int *stamp, int *x, int *y, int *w, int *h,
		       int *nclip, XF86DRIClipRectPtr *clip, int *bx, int *by,
		       int *nback, XF86DRIClipRectPtr *back)
{
    if (draw != DRAWABLE)
	return False;
    *index = H.slot;
    *stamp = H.sarea->drawableTable[H.slot].stamp;
    *x = *y = *bx = *by = 0;
    *w = H.w;
    *h = H.h;
    H.clip.x1 = H.clip.y1 = 0;
    H.clip.x2 = H.w;
    H.clip.y2 = H.h;
    /* the driver keeps and frees its own copies */
    if ((*clip = malloc(sizeof H.clip)) == 0 || (*back = malloc(sizeof H.clip)) == 0)
	return False;
    **clip = **back = H.clip;
    *nclip = *nback = 1;
    return True;
}

Bool
XF86DRIGetDeviceInfo(Display *dpy, int scrn, drmHandlePtr hFB, int *origin, int *size,
		     int *stride, int *privsize, void **priv)
{
    if ((*priv = malloc(sizeof H.info)) == 0)
	return False;
    memcpy(*priv, &H.info, sizeof H.info);
    *privsize = sizeof H.info;
    *hFB = H.hFB;
    *origin = H.info.frontOffset;
    *size = H.info.stride * H.info.height;
    *stride = H.info.stride;
    return True;
}

Bool
XF86DRIOpenFullScreen(Display *dpy, int scrn, Drawable draw)
{
    return False;
}

Bool
XF86DRICloseFullScreen(Display *dpy, int scrn, Drawable draw)
{
    return False;
}

/* AMIX's libc has none; the DRI utilities compare option names with it */
int
strcasecmp(const char *a, const char *b)
{
    int ca, cb;

    do {
	ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
	cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
	a++, b++;
    } while (ca && ca == cb);
    return ca - cb;
}

static int
findmaps(void)
{
    drmHandle offset, handle;
    drmSize size;
    drmMapType type;
    drmMapFlags flags;
    int i, mtrr;

    for (i = 0; drmGetMap(H.fd, i, &offset, &size, &type, &flags, &handle, &mtrr) == 0; i++)
	if (type == DRM_SHM && (flags & DRM_CONTAINS_LOCK))
	    H.hSAREA = offset;
	else if (type == DRM_FRAME_BUFFER)
	    H.hFB = offset;
    return H.hSAREA && H.hFB;
}

static __DRIdriverEntry *entry;
static const char *loaderr;

/*
 * Binds fbfd's session and loads the card's driver, running the dynamic
 * linker: call it outside guest context.  Returns the driver's name, or 0
 * with the reason in err.
 */
const char *
drihost_load(int fbfd, const char **err)
{
    __DRIdriverEntry *e;
    drmVersionPtr v;
    drmDSBindRec b;
    int i;

    if (entry || loaderr) {
	*err = loaderr;
	return entry ? H.name : 0;
    }
    loaderr = "no DRM for the display";
    if ((H.fd = drmOpenSession(fbfd, &b)) < 0)
	goto fail;
    (void) fcntl(H.fd, F_SETFD, 1);
    H.base = b.offset;
    H.info.session = b.session;
    H.info.vramOffset = b.vram;
    H.info.vramSize = b.vramsize;
    loaderr = "no DRM version";
    if ((v = drmGetVersion(H.fd)) == 0)
	goto fail;
    strncpy(H.name, v->name, sizeof H.name - 1);
    drmFreeVersion(v);
    for (e = __driDrivers; e->name && strcmp(e->name, H.name); e++)
	;
    loaderr = "no driver for the card";
    if (!e->name && !(e = __driLoadDriver(H.name)))
	goto fail;
    if (__driHandle)
	front = (void (*)(int, int)) _dlsym(__driHandle, "grDRIFront");
    loaderr = 0;
    entry = e;
    return H.name;
fail:
    *err = loaderr;
    i = errno;
    if (H.fd >= 0)
	close(H.fd);
    H.fd = -1;
    errno = i;
    return 0;
}

/*
 * The session of fbfd, its frame at frame bytes into the session's
 * memory, width x height pixels of depth bits, stride bytes a row.
 * Returns the driver's name, or 0 with the reason in err.
 */
const char *
drihost_open(int fbfd, unsigned long frame, int width, int height, int stride, int depth,
	     const char **err)
{
    __DRIdriverEntry *e;
    drmAddress a;
    int i;

    if (H.sc.driScreen.private)
	return H.name;
    if (!drihost_load(fbfd, err))
	return 0;
    e = entry;
    *err = "no maps or drawable";
    if (!findmaps() ||
	drmMap(H.fd, H.hSAREA, SAREA_MAX, &a) < 0 || drmCreateDrawable(H.fd, &H.hw) < 0)
	goto fail;
    H.sarea = (XF86DRISAREAPtr) a;
    H.busId = drmGetBusid(H.fd);
    if (!H.busId)
	H.busId = "";
    /* a slot of its own when an X server shares the card */
    H.slot = H.hw % SAREA_MAX_DRAWABLES;
    H.sarea->drawableTable[H.slot].flags = 0;
    H.sarea->drawableTable[H.slot].stamp++;
    H.info.width = width;
    H.info.height = height;
    H.info.depth = depth;
    H.info.cpp = (depth + 7) / 8;
    H.info.stride = stride;
    H.info.frontOffset = H.base + frame;
    H.w = width;
    H.h = height;

    H.screen.width = width;
    H.screen.height = height;
    H.screen.root_depth = depth;
    H.dpy.screens = &H.screen;
    H.dpy.nscreens = 1;
    H.config.vid = VISUAL;
    H.config.class = TrueColor;
    H.config.rgba = True;
    H.config.redSize = depth == 16 ? 5 : 8;
    H.config.greenSize = depth == 16 ? 6 : 8;
    H.config.blueSize = depth == 16 ? 5 : 8;
    H.config.redMask = depth == 16 ? 0xf800 : 0xff0000;
    H.config.greenMask = depth == 16 ? 0x07e0 : 0x00ff00;
    H.config.blueMask = depth == 16 ? 0x001f : 0x0000ff;
    H.config.bufferSize = depth;
    /* the Voodoo3's depth buffer is 16 bits; the R200's is 24+8 at 32 bits */
    H.config.depthSize = depth == 32 && strcmp(H.name, "tdfx") ? 24 : 16;
    H.config.stencilSize = H.config.depthSize == 24 ? 8 : 0;
    H.config.visualRating = GLX_NONE_EXT;
    H.config.transparentPixel = GLX_NONE_EXT;
    H.sc.configs = &H.config;
    H.sc.numConfigs = 1;
    H.priv.screenConfigs = &H.sc;
    H.visual.visualid = VISUAL;
    H.vis.visual = &H.visual;
    H.vis.visualid = VISUAL;
    H.vis.screen = 0;
    H.vis.depth = depth;
    H.vis.class = TrueColor;

    *err = "the driver refused the screen";
    H.sc.driScreen.private = (*e->createScreen)(&H.dpy, 0, &H.sc.driScreen, 1, &H.config);
    if (!H.sc.driScreen.private)
	goto fail;
    if (e->registerExtensions)
	(*e->registerExtensions)();
    *err = 0;
    return H.name;
fail:
    i = errno;
    close(H.fd);
    H.fd = -1;
    errno = i;
    return 0;
}

/* a context on the drawable; 0 if the driver refuses it */
void *
drihost_create(void)
{
    __GLXcontext *gc;

    if (!H.sc.driScreen.private || (gc = calloc(1, sizeof *gc)) == 0)
	return 0;
    gc->vid = VISUAL;
    gc->screen = 0;
    gc->driContext.private = (*H.sc.driScreen.createContext)(&H.dpy, &H.vis, 0, &gc->driContext);
    if (!gc->driContext.private) {
	free(gc);
	return 0;
    }
    return gc;
}

void
drihost_destroy(void *ctx)
{
    __GLXcontext *gc = ctx;

    (*gc->driContext.unbindContext)(&H.dpy, 0, DRAWABLE, gc, 0);
    (*gc->driContext.destroyContext)(&H.dpy, 0, gc->driContext.private);
    free(gc);
}

/*
 * Draw from now on into w x h pixels at offset bytes into the session's
 * memory, rows stride bytes apart.
 */
int
drihost_bind(void *ctx, unsigned long offset, int stride, int w, int h)
{
    __GLXcontext *gc = ctx;

    if (w != H.w || h != H.h) {
	H.w = w;
	H.h = h;
	H.sarea->drawableTable[H.slot].stamp++;
    }
    /* before: a new Glide context starts from it; after: an open one moves */
    if (front)
	front(H.base + offset, stride);
    if (!(*gc->driContext.bindContext)(&H.dpy, 0, DRAWABLE, gc))
	return 0;
    if (front)
	front(H.base + offset, stride);
    glDrawBuffer(GL_FRONT);
    return 1;
}

unsigned long
drihost_textend(void)
{
    return __driHandle ? (unsigned long) _dlsym(__driHandle, "__dri_end") : 0;
}
