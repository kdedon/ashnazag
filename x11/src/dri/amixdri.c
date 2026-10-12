/*
 * amixdri.c -- direct rendering on a display-service screen.
 *
 * The kernel DRM made its maps (registers, frame buffer, shared area)
 * when the card's display backend attached it; the server only binds
 * its own session to its DRM file (DSBIND), authenticates clients by
 * their magic, which hands them that session, and keeps the shared
 * area's drawable table: a stamp per window that changes with its clip.
 * Clients ask for the clip rectangles when their stamp moves.
 *
 * The server draws into the frame buffer with the CPU; it holds the
 * hardware lock while it handles requests, once a client has a direct
 * drawable, so a client never renders between the server's reads and
 * writes.  Its lock is granted while the session is hidden: hidden
 * clients are blocked by the kernel, so nobody else draws to it.
 */
#include "X.h"
#include "Xproto.h"
#include "misc.h"
#include "dixstruct.h"
#include "resource.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "regionstr.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#define _XF86DRI_SERVER_
#include "xf86dristr.h"
#include "drm.h"
#include "amixdri.h"

typedef struct {
    WindowPtr	pWin;		/* 0: the slot is free */
    drmDrawable	hw;
    int		refs;
} DRIDrawRec;

typedef struct {
    Bool		active;
    int			fd;		/* opened once, kept across resets */
    drmContext		ctx;		/* the server's own context */
    drmHandle		hSAREA, hFB;
    XF86DRISAREAPtr	sarea;
    char		*busId;
    char		name[32];
    int			major, minor, patch;
    AMIXDRIRec		info;
    Bool		locked;
    int			ndraw;
    DRIDrawRec		draw[SAREA_MAX_DRAWABLES];
    ClipNotifyProcPtr	ClipNotify;
    DestroyWindowProcPtr DestroyWindow;
    CloseScreenProcPtr	CloseScreen;
} DRIScreenRec, *DRIScreenPtr;

typedef struct {
    ScreenPtr	pScreen;
    drmContext	hw;
} DRIContextRec;

static DRIScreenRec driScreens[MAXSCREENS];
static RESTYPE DRIContextResType;

#define DRIPTR(s)	(&driScreens[(s)->myNum])

static void
DRILock(DRIScreenPtr d)
{
    drm_lock_t l;

    if (d->locked)
	return;
    l.context = d->ctx;
    l.flags = _DRM_LOCK_HIDDEN;
    while (ioctl(d->fd, DRM_IOCTL_LOCK, &l) < 0)
	if (errno != EINTR) {
	    ErrorF("DRI: lock: %s\n", strerror(errno));
	    return;
	}
    d->locked = TRUE;
}

static void
DRIUnlock(DRIScreenPtr d)
{
    if (!d->locked)
	return;
    if (drmUnlock(d->fd, d->ctx) < 0)
	ErrorF("DRI: unlock failed\n");
    d->locked = FALSE;
}

/* the lock is held from the wakeup to the block, while clients have drawables */
static void
DRIBlockHandler(data, timeout, readmask)
pointer data, timeout, readmask;
{
    DRIUnlock(DRIPTR((ScreenPtr) data));
}

static void
DRIWakeupHandler(data, result, readmask)
pointer data;
int result;
pointer readmask;
{
    DRIScreenPtr d = DRIPTR((ScreenPtr) data);

    if (d->active && d->ndraw > 0)
	DRILock(d);
}

static DRIDrawRec *
DRIFindDraw(d, pWin)
DRIScreenPtr d;
WindowPtr pWin;
{
    int i;

    if (d->ndraw == 0)
	return 0;
    for (i = 0; i < SAREA_MAX_DRAWABLES; i++)
	if (d->draw[i].pWin == pWin)
	    return &d->draw[i];
    return 0;
}

/* the drawable's stamp moves: its clients fetch the clip again */
static void
DRIBump(d, p)
DRIScreenPtr d;
DRIDrawRec *p;
{
    d->sarea->drawableTable[p - d->draw].stamp++;
}

static void
DRIFreeDraw(d, p)
DRIScreenPtr d;
DRIDrawRec *p;
{
    DRIBump(d, p);
    d->sarea->drawableTable[p - d->draw].flags = 0;
    drmDestroyDrawable(d->fd, p->hw);
    p->pWin = 0;
    p->refs = 0;
    d->ndraw--;
}

static void
DRIClipNotify(pWin, dx, dy)
WindowPtr pWin;
int dx, dy;
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    DRIScreenPtr d = DRIPTR(pScreen);
    DRIDrawRec *p;

    if ((p = DRIFindDraw(d, pWin)) != 0)
	DRIBump(d, p);
    if (d->ClipNotify) {
	pScreen->ClipNotify = d->ClipNotify;
	(*pScreen->ClipNotify)(pWin, dx, dy);
	pScreen->ClipNotify = DRIClipNotify;
    }
}

static Bool
DRIDestroyWindow(pWin)
WindowPtr pWin;
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    DRIScreenPtr d = DRIPTR(pScreen);
    DRIDrawRec *p;
    Bool r;

    if ((p = DRIFindDraw(d, pWin)) != 0)
	DRIFreeDraw(d, p);
    pScreen->DestroyWindow = d->DestroyWindow;
    r = (*pScreen->DestroyWindow)(pWin);
    pScreen->DestroyWindow = DRIDestroyWindow;
    return r;
}

static Bool
DRICloseScreen(index, pScreen)
int index;
ScreenPtr pScreen;
{
    DRIScreenPtr d = DRIPTR(pScreen);
    int i;

    for (i = 0; i < SAREA_MAX_DRAWABLES; i++)
	if (d->draw[i].pWin)
	    DRIFreeDraw(d, &d->draw[i]);
    DRIUnlock(d);
    pScreen->ClipNotify = d->ClipNotify;
    pScreen->DestroyWindow = d->DestroyWindow;
    pScreen->CloseScreen = d->CloseScreen;
    d->active = FALSE;
    return (*pScreen->CloseScreen)(index, pScreen);
}

/* the shared area and frame buffer among the kernel's maps */
static Bool
DRIFindMaps(d)
DRIScreenPtr d;
{
    drmHandle offset, handle;
    drmSize size;
    drmMapType type;
    drmMapFlags flags;
    int i, mtrr;

    d->hSAREA = d->hFB = 0;
    for (i = 0; drmGetMap(d->fd, i, &offset, &size, &type, &flags, &handle, &mtrr) == 0; i++)
	if (type == DRM_SHM && (flags & DRM_CONTAINS_LOCK))
	    d->hSAREA = offset;
	else if (type == DRM_FRAME_BUFFER)
	    d->hFB = offset;
    return d->hSAREA && d->hFB;
}

/*
 * A screen on a display session with a DRM: called from the card's
 * screen init with the session's /dev/fbN, the frame's offset in VRAM
 * and its layout.  No DRM, no direct rendering: that is not an error.
 */
Bool
AmixDRIScreenInit(pScreen, fbfd, offset, stride, width, height, depth)
ScreenPtr pScreen;
int fbfd;
unsigned long offset;
int stride, width, height, depth;
{
    DRIScreenPtr d = DRIPTR(pScreen);
    drmVersionPtr v;
    drmDSBindRec b;
    drmAddress a;
    int e;

    d->active = FALSE;
    if (d->fd <= 0) {
	if ((d->fd = open("/dev/dri/card0", O_RDWR)) < 0) {
	    if (errno != ENOENT && errno != ENXIO)
		ErrorF("DRI: /dev/dri/card0: %s\n", strerror(errno));
	    return FALSE;
	}
	(void) fcntl(d->fd, F_SETFD, 1);
	if ((v = drmGetVersion(d->fd)) == 0) {
	    ErrorF("DRI: no DRM version: %s\n", strerror(errno));
	    close(d->fd);
	    d->fd = -1;
	    return FALSE;
	}
	strncpy(d->name, v->name, sizeof d->name - 1);
	d->major = 1;
	d->minor = 0;
	d->patch = 0;
	drmFreeVersion(v);
	if ((e = drmDSBind(d->fd, fbfd, &b)) < 0 || (e = -EINVAL, !DRIFindMaps(d)) ||
	    (e = drmMap(d->fd, d->hSAREA, SAREA_MAX, &a)) < 0 ||
	    (e = drmCreateContext(d->fd, &d->ctx)) < 0) {
	    ErrorF("DRI: %s: no session binding, maps or context: %s\n", d->name, strerror(-e));
	    close(d->fd);
	    d->fd = -1;
	    return FALSE;
	}
	d->sarea = (XF86DRISAREAPtr) a;
	d->busId = drmGetBusid(d->fd);
	d->info.session = b.session;
	d->info.frontOffset = b.offset;
	d->info.vramOffset = b.vram;
	d->info.vramSize = b.vramsize;
	ErrorF("DRI: %s at %s, session %ld, frame at 0x%lx, %lu KB free VRAM\n",
	       d->name, d->busId ? d->busId : "?", b.session, b.offset, b.vramsize >> 10);
    }
    d->info.width = width;
    d->info.height = height;
    d->info.depth = depth;
    d->info.cpp = (depth + 7) / 8;
    d->info.stride = stride;
    d->locked = FALSE;
    d->ndraw = 0;
    memset(d->draw, 0, sizeof d->draw);
    d->ClipNotify = pScreen->ClipNotify;
    pScreen->ClipNotify = DRIClipNotify;
    d->DestroyWindow = pScreen->DestroyWindow;
    pScreen->DestroyWindow = DRIDestroyWindow;
    d->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = DRICloseScreen;
    RegisterBlockAndWakeupHandlers(DRIBlockHandler, DRIWakeupHandler, (pointer) pScreen);
    d->active = TRUE;
    return TRUE;
}

static int
DRIContextDelete(value, id)
pointer value;
XID id;
{
    DRIContextRec *c = (DRIContextRec *) value;

    drmDestroyContext(DRIPTR(c->pScreen)->fd, c->hw);
    xfree(c);
    return Success;
}

Bool
DRIExtensionInit()
{
    int i;

    for (i = 0; i < screenInfo.numScreens; i++)
	if (DRIScreenActive(screenInfo.screens[i]))
	    break;
    if (i == screenInfo.numScreens)
	return FALSE;
    DRIContextResType = CreateNewResourceType(DRIContextDelete);
    return DRIContextResType != 0;
}

void
DRIReset()
{
}

Bool
DRIScreenActive(pScreen)
ScreenPtr pScreen;
{
    return DRIPTR(pScreen)->active;
}

Bool
DRIQueryDirectRenderingCapable(pScreen, isCapable)
ScreenPtr pScreen;
Bool *isCapable;
{
    *isCapable = DRIPTR(pScreen)->active;
    return TRUE;
}

Bool
DRIOpenConnection(pScreen, hSAREA, busIdString)
ScreenPtr pScreen;
drmHandlePtr hSAREA;
char **busIdString;
{
    DRIScreenPtr d = DRIPTR(pScreen);

    if (!d->active)
	return FALSE;
    *hSAREA = d->hSAREA;
    *busIdString = d->busId;
    return TRUE;
}

/* the client's file draws for the server's session from now on */
Bool
DRIAuthConnection(pScreen, magic)
ScreenPtr pScreen;
drmMagic magic;
{
    DRIScreenPtr d = DRIPTR(pScreen);

    return d->active && drmAuthMagic(d->fd, magic) == 0;
}

Bool
DRICloseConnection(pScreen)
ScreenPtr pScreen;
{
    return TRUE;
}

Bool
DRIGetClientDriverName(pScreen, major, minor, patch, name)
ScreenPtr pScreen;
int *major, *minor, *patch;
char **name;
{
    DRIScreenPtr d = DRIPTR(pScreen);

    *major = d->major;
    *minor = d->minor;
    *patch = d->patch;
    *name = d->active ? d->name : 0;
    return d->active;
}

Bool
DRICreateContext(pScreen, visual, context, pHWContext)
ScreenPtr pScreen;
VisualPtr visual;
XID context;
drmContextPtr pHWContext;
{
    DRIScreenPtr d = DRIPTR(pScreen);
    DRIContextRec *c;

    if (!d->active || (c = (DRIContextRec *) xalloc(sizeof *c)) == 0)
	return FALSE;
    if (drmCreateContext(d->fd, &c->hw) < 0) {
	xfree(c);
	return FALSE;
    }
    c->pScreen = pScreen;
    if (!AddResource(context, DRIContextResType, (pointer) c))
	return FALSE;
    *pHWContext = c->hw;
    return TRUE;
}

Bool
DRIDestroyContext(pScreen, context)
ScreenPtr pScreen;
XID context;
{
    FreeResourceByType(context, DRIContextResType, FALSE);
    return TRUE;
}

Bool
DRICreateDrawable(pScreen, id, pDrawable, hHWDrawable)
ScreenPtr pScreen;
Drawable id;
DrawablePtr pDrawable;
drmDrawablePtr hHWDrawable;
{
    DRIScreenPtr d = DRIPTR(pScreen);
    WindowPtr pWin = (WindowPtr) pDrawable;
    DRIDrawRec *p;
    int i;

    if (!d->active || pDrawable->type != DRAWABLE_WINDOW)
	return FALSE;
    if ((p = DRIFindDraw(d, pWin)) == 0) {
	for (i = 0; i < SAREA_MAX_DRAWABLES && d->draw[i].pWin; i++)
	    ;
	if (i == SAREA_MAX_DRAWABLES)
	    return FALSE;
	p = &d->draw[i];
	if (drmCreateDrawable(d->fd, &p->hw) < 0)
	    return FALSE;
	DRILock(d);
	p->pWin = pWin;
	d->ndraw++;
	d->sarea->drawableTable[i].flags = 0;
	DRIBump(d, p);
    }
    p->refs++;
    *hHWDrawable = p->hw;
    return TRUE;
}

Bool
DRIDestroyDrawable(pScreen, id, pDrawable)
ScreenPtr pScreen;
Drawable id;
DrawablePtr pDrawable;
{
    DRIScreenPtr d = DRIPTR(pScreen);
    DRIDrawRec *p;

    if (pDrawable->type != DRAWABLE_WINDOW ||
	(p = DRIFindDraw(d, (WindowPtr) pDrawable)) == 0)
	return FALSE;
    if (--p->refs == 0)
	DRIFreeDraw(d, p);
    return TRUE;
}

/* in screen coordinates; the back buffer is the window's box on the screen */
Bool
DRIGetDrawableInfo(pScreen, pDrawable, index, stamp, X, Y, W, H,
		   numClipRects, pClipRects, backX, backY, numBackClipRects, pBackClipRects)
ScreenPtr pScreen;
DrawablePtr pDrawable;
unsigned int *index, *stamp;
int *X, *Y, *W, *H, *numClipRects, *backX, *backY, *numBackClipRects;
XF86DRIClipRectPtr *pClipRects, *pBackClipRects;
{
    static XF86DRIClipRectRec back;
    DRIScreenPtr d = DRIPTR(pScreen);
    WindowPtr pWin = (WindowPtr) pDrawable;
    DRIDrawRec *p;
    int x1, y1, x2, y2;

    if (pDrawable->type != DRAWABLE_WINDOW || (p = DRIFindDraw(d, pWin)) == 0)
	return FALSE;
    *index = p - d->draw;
    *stamp = d->sarea->drawableTable[*index].stamp;
    *X = pWin->drawable.x;
    *Y = pWin->drawable.y;
    *W = pWin->drawable.width;
    *H = pWin->drawable.height;
    *backX = *X;
    *backY = *Y;
    if (!pWin->viewable) {
	*numClipRects = 0;
	*pClipRects = 0;
	*numBackClipRects = 0;
	*pBackClipRects = 0;
	return TRUE;
    }
    *numClipRects = REGION_NUM_RECTS(&pWin->clipList);
    *pClipRects = (XF86DRIClipRectPtr) REGION_RECTS(&pWin->clipList);
    x1 = max(*X, 0);
    y1 = max(*Y, 0);
    x2 = min(*X + *W, pScreen->width);
    y2 = min(*Y + *H, pScreen->height);
    if (x1 < x2 && y1 < y2) {
	back.x1 = x1;
	back.y1 = y1;
	back.x2 = x2;
	back.y2 = y2;
	*numBackClipRects = 1;
	*pBackClipRects = &back;
    } else {
	*numBackClipRects = 0;
	*pBackClipRects = 0;
    }
    return TRUE;
}

Bool
DRIGetDeviceInfo(pScreen, hFrameBuffer, fbOrigin, fbSize, fbStride, devPrivateSize, pDevPrivate)
ScreenPtr pScreen;
drmHandlePtr hFrameBuffer;
int *fbOrigin, *fbSize, *fbStride, *devPrivateSize;
void **pDevPrivate;
{
    DRIScreenPtr d = DRIPTR(pScreen);

    if (!d->active)
	return FALSE;
    *hFrameBuffer = d->hFB;
    *fbOrigin = d->info.frontOffset;
    *fbSize = d->info.stride * d->info.height;
    *fbStride = d->info.stride;
    *devPrivateSize = sizeof d->info;
    *pDevPrivate = (void *) &d->info;
    return TRUE;
}

int
DRIOpenFullScreen(pScreen, pDrawable)
ScreenPtr pScreen;
DrawablePtr pDrawable;
{
    return FALSE;
}

int
DRICloseFullScreen(pScreen, pDrawable)
ScreenPtr pScreen;
DrawablePtr pDrawable;
{
    return FALSE;
}
