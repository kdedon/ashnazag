/*
 * dritest.c -- direct rendering without a card driver.
 *
 *   dritest [seconds]
 *
 * A null DRI driver, linked in under the name the server gives, lets
 * libGL go through the whole DRI set-up: open the DRM, authenticate by
 * magic through the server, map the shared area and frame buffer,
 * create a context and a drawable, and read the drawable's clip
 * rectangles.  The GL window is half covered by a second window; each
 * clip rectangle is painted (green, blue, alternating) and must cover
 * exactly the visible part.  Moving the cover must move the drawable's
 * stamp.  A child that takes the hardware lock and exits must leave it
 * free.  A second child, with no driver linked for the screen, draws
 * with Mesa's Xlib renderer: a red square.  The windows stay up for
 * `seconds' (default 0).  Output: one line per check, then PASS or FAIL.
 */
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include "dri_util.h"
#include "amixdrirec.h"
#include "dsio.h"
#include "drm.h"

#define GLW	320		/* the GL window */
#define GLH	240
#define CW	160		/* its cover */
#define CH	80

static int fails;
static char lines[16][80];
static int nlines;

static void
check(char *what, int ok, char *fmt, ...)
{
    char buf[200];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    printf("dritest: %s %s: %s\n", ok ? "ok" : "FAIL", what, buf);
    fflush(stdout);
    if (nlines < 16)
	sprintf(lines[nlines++], "%s %s %.60s", ok ? "ok" : "FAIL", what, buf);
    if (!ok)
	fails++;
}

/* the null driver: what libGL hands it */
static __DRIscreenPrivate *nscreen;
static __DRIdrawablePrivate *ndraw;

static void
validate(__DRIdrawablePrivate *pdp)
{
    __DRIscreenPrivate *psp = pdp->driScreenPriv;

    DRM_SPINLOCK(&psp->pSAREA->drawable_lock, psp->drawLockID);
    if (*pdp->pStamp != pdp->lastStamp)
	__driUtilUpdateDrawableInfo(psp->display, psp->myNum, pdp);
    DRM_SPINUNLOCK(&psp->pSAREA->drawable_lock, psp->drawLockID);
}

static GLboolean
nInitDriver(__DRIscreenPrivate *psp)
{
    nscreen = psp;
    return GL_TRUE;
}

static void
nDestroyScreen(__DRIscreenPrivate *psp)
{
}

static GLboolean
nCreateContext(Display *dpy, const __GLcontextModes *v, __DRIcontextPrivate *c, void *shared)
{
    return GL_TRUE;
}

static void
nDestroyContext(__DRIcontextPrivate *c)
{
}

static GLboolean
nCreateBuffer(Display *dpy, __DRIscreenPrivate *psp, __DRIdrawablePrivate *d,
	      const __GLcontextModes *v, GLboolean pixmap)
{
    return !pixmap;
}

static void
nDestroyBuffer(__DRIdrawablePrivate *d)
{
}

static void
nSwapBuffers(Display *dpy, void *d)
{
}

static GLboolean
nMakeCurrent(__DRIcontextPrivate *c, __DRIdrawablePrivate *d, __DRIdrawablePrivate *r)
{
    ndraw = d;
    validate(d);
    return GL_TRUE;
}

static GLboolean
nUnbindContext(__DRIcontextPrivate *c)
{
    return GL_TRUE;
}

static struct __DriverAPIRec nullAPI = {
    nInitDriver, nDestroyScreen, nCreateContext, nDestroyContext,
    nCreateBuffer, nDestroyBuffer, nSwapBuffers, nMakeCurrent, nUnbindContext,
    NULL, NULL
};

static void *
nCreateScreen(Display *dpy, int scrn, __DRIscreen *psc, int n, __GLXvisualConfig *c)
{
    return __driUtilCreateScreen(dpy, scrn, psc, n, c, &nullAPI);
}

/* the drivers libGL knows: the null one, named once the server says */
__DRIdriverEntry __driDrivers[] = {
    { NULL, nCreateScreen, NULL },
    { NULL, NULL, NULL }
};

static Display *dpy;
static int scr;
static Window glwin, cover, results;

static unsigned long
pixel(char *name)
{
    XColor c, e;

    if (!XAllocNamedColor(dpy, DefaultColormap(dpy, scr), name, &c, &e))
	return WhitePixel(dpy, scr);
    return c.pixel;
}

static Window
mkwin(int x, int y, int w, int h, unsigned long bg)
{
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), x, y, w, h, 0,
				     BlackPixel(dpy, scr), bg);
    XSetWindowAttributes a;

    a.override_redirect = True;
    XChangeWindowAttributes(dpy, win, CWOverrideRedirect, &a);
    XMapRaised(dpy, win);
    return win;
}

/* the clip rectangles cover the GL window less the cover, and nothing else */
static int
cover_ok(__DRIdrawablePrivate *d, int cx, int cy)
{
    long area = 0, want;
    int i, x1, y1, x2, y2;
    XF86DRIClipRectPtr r;

    for (i = 0; i < d->numClipRects; i++) {
	r = &d->pClipRects[i];
	if (r->x1 < d->x || r->y1 < d->y || r->x2 > d->x + d->w || r->y2 > d->y + d->h)
	    return 0;
	x1 = r->x1 > cx ? r->x1 : cx;
	y1 = r->y1 > cy ? r->y1 : cy;
	x2 = r->x2 < cx + CW ? r->x2 : cx + CW;
	y2 = r->y2 < cy + CH ? r->y2 : cy + CH;
	if (x1 < x2 && y1 < y2)
	    return 0;
	area += (long)(r->x2 - r->x1) * (r->y2 - r->y1);
    }
    x1 = cx > d->x ? cx : d->x;
    y1 = cy > d->y ? cy : d->y;
    x2 = cx + CW < d->x + d->w ? cx + CW : d->x + d->w;
    y2 = cy + CH < d->y + d->h ? cy + CH : d->y + d->h;
    want = (long) d->w * d->h - (x1 < x2 && y1 < y2 ? (long)(x2 - x1) * (y2 - y1) : 0);
    return area == want;
}

static void
paint(__DRIdrawablePrivate *d)
{
    GC gc = XCreateGC(dpy, RootWindow(dpy, scr), 0, 0);
    unsigned long c[2];
    int i;
    XF86DRIClipRectPtr r;

    c[0] = pixel("green");
    c[1] = pixel("blue");
    XSetSubwindowMode(dpy, gc, IncludeInferiors);
    for (i = 0; i < d->numClipRects; i++) {
	r = &d->pClipRects[i];
	XSetForeground(dpy, gc, c[i & 1]);
	XFillRectangle(dpy, RootWindow(dpy, scr), gc, r->x1, r->y1,
		       r->x2 - r->x1, r->y2 - r->y1);
    }
    XFreeGC(dpy, gc);
}

/* the check lines in a window below the others */
static void
report()
{
    GC gc;
    int i;

    results = mkwin(40, 40 + GLH + 40 + 90 + 30, 640, 14 + 13 * nlines, WhitePixel(dpy, scr));
    gc = XCreateGC(dpy, results, 0, 0);
    XSync(dpy, False);
    sleep(1);
    XSetForeground(dpy, gc, BlackPixel(dpy, scr));
    for (i = 0; i < nlines; i++)
	XDrawString(dpy, results, gc, 6, 14 + i * 13, lines[i], strlen(lines[i]));
    XFreeGC(dpy, gc);
    XSync(dpy, False);
}

/* the lock within secs; libdrm's drmGetLock would retry for ever */
static int
trylock(int fd, drmContext ctx, int secs)
{
    drm_lock_t l;
    int r;

    l.context = ctx;
    l.flags = 0;
    alarm(secs);
    r = ioctl(fd, DRM_IOCTL_LOCK, &l);
    alarm(0);
    return r == 0;
}

/* a client that takes the lock and dies with it */
static void
deadlock(char *busid)
{
    int up[2], down[2], fd;
    drmMagic magic;
    drmContext ctx;
    char c;
    pid_t pid;

    if (pipe(up) < 0 || pipe(down) < 0)
	return;
    if ((pid = fork()) == 0) {
	fd = drmOpen(NULL, busid);
	if (fd < 0 || drmGetMagic(fd, &magic) < 0)
	    _exit(1);
	write(up[1], &magic, sizeof magic);
	if (read(down[0], &c, 1) != 1 || drmCreateContext(fd, &ctx) < 0 ||
	    drmGetLock(fd, ctx, 0) < 0)
	    _exit(1);
	write(up[1], "L", 1);
	sleep(1);
	_exit(0);		/* the lock still held */
    }
    close(up[1]);
    close(down[0]);
    if (read(up[0], &magic, sizeof magic) != sizeof magic) {
	check("lock", 0, "child could not open the DRM");
	return;
    }
    check("auth", XF86DRIAuthConnection(dpy, scr, magic), "second client's magic %u", (unsigned) magic);
    write(down[1], "g", 1);
    c = 0;
    read(up[0], &c, 1);
    waitpid(pid, 0, 0);
    if (c != 'L') {
	check("lock", 0, "child got no lock");
	return;
    }
    if (drmCreateContext(nscreen->fd, &ctx) < 0) {
	check("lock", 0, "no context of our own");
	return;
    }
    c = trylock(nscreen->fd, ctx, 5);
    check("lock", c, "free after its holder exited");
    if (c)
	drmUnlock(nscreen->fd, ctx);
    drmDestroyContext(nscreen->fd, ctx);
}

/*
 * A switch away waits while a client holds the lock; once it is free
 * the console comes to the front, and X's clients get no lock while
 * X is hidden.
 */
static void
switchaway(AMIXDRIPtr info)
{
    int fb = open("/dev/fb0", O_RDWR), held, freed, hidden, back;
    drmContext ctx;

    if (fb < 0 || drmCreateContext(nscreen->fd, &ctx) < 0) {
	check("switch", 0, "no /dev/fb0 or context");
	return;
    }
    drmGetLock(nscreen->fd, ctx, 0);
    held = ioctl(fb, FBIOSWITCH, 0) < 0 ? errno : 0;
    drmUnlock(nscreen->fd, ctx);
    freed = ioctl(fb, FBIOSWITCH, 0) < 0 ? errno : 0;
    hidden = !trylock(nscreen->fd, ctx, 2);
    if (!hidden)
	drmUnlock(nscreen->fd, ctx);
    sleep(2);
    back = ioctl(fb, FBIOSWITCH, info->session) < 0 ? errno : 0;
    check("switch", held == EBUSY && freed == 0 && hidden != 0 && back == 0,
	  "lock held: %s, free: %s, hidden lock: %s, back: %s",
	  held ? strerror(held) : "switched", freed ? strerror(freed) : "switched",
	  hidden ? "refused" : "granted", back ? strerror(back) : "ok");
    drmDestroyContext(nscreen->fd, ctx);
    close(fb);
}

static void
alarmed(int sig)
{
}

/* Mesa's own renderer, where no driver takes the screen */
static void
software(int hold, int out)
{
    static int att[] = { GLX_RGBA, None };
    XVisualInfo *vi;
    GLXContext ctx;
    Window win;
    XImage *im;
    XColor c;
    int ok;

    if ((dpy = XOpenDisplay(NULL)) == 0) {
	printf("dritest: FAIL software: no display\n");
	write(out, "F", 1);
	_exit(1);
    }
    scr = DefaultScreen(dpy);
    if ((vi = glXChooseVisual(dpy, scr, att)) == 0) {
	printf("dritest: FAIL software: no RGBA visual\n");
	write(out, "F", 1);
	_exit(1);
    }
    win = mkwin(40, 40 + GLH + 40, 120, 90, BlackPixel(dpy, scr));
    XSync(dpy, False);
    ctx = glXCreateContext(dpy, vi, NULL, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) {
	printf("dritest: FAIL software: no context\n");
	write(out, "F", 1);
	_exit(1);
    }
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    XSync(dpy, False);
    im = XGetImage(dpy, win, 60, 45, 1, 1, AllPlanes, ZPixmap);
    c.pixel = XGetPixel(im, 0, 0);
    XQueryColor(dpy, DefaultColormap(dpy, scr), &c);
    ok = c.red > 0xc000 && c.green < 0x4000 && c.blue < 0x4000 && !glXIsDirect(dpy, ctx);
    printf("dritest: %s software: %s, direct %d, pixel %04x %04x %04x\n", ok ? "ok" : "FAIL",
	   (char *) glGetString(GL_RENDERER), glXIsDirect(dpy, ctx), c.red, c.green, c.blue);
    fflush(stdout);
    write(out, ok ? "P" : "F", 1);
    sleep(hold);
    _exit(0);
}

int
main(int argc, char **argv)
{
    int hold = argc > 1 ? atoi(argv[1]) : 0;
    int major, minor, patch, capable, status, ox, oy;
    char *name = 0, *busid = 0;
    drmHandle hsarea;
    XVisualInfo tmpl, *vi;
    GLXContext ctx;
    pid_t sw;
    AMIXDRIPtr info;
    unsigned int stamp;
    int n, p[2];
    char c = 0;

    signal(SIGALRM, alarmed);
    if (pipe(p) < 0 || (sw = fork()) < 0)
	return 1;
    if (sw == 0)
	software(hold + 5, p[1]);
    close(p[1]);
    if (read(p[0], &c, 1) != 1 || c != 'P')
	fails++;
    if ((dpy = XOpenDisplay(NULL)) == 0) {
	printf("dritest: FAIL no display\n");
	return 1;
    }
    scr = DefaultScreen(dpy);
    if (!XF86DRIQueryDirectRenderingCapable(dpy, scr, &capable) || !capable ||
	!XF86DRIGetClientDriverName(dpy, scr, &major, &minor, &patch, &name) ||
	!XF86DRIOpenConnection(dpy, scr, &hsarea, &busid)) {
	check("server", 0, "no direct rendering on screen %d", scr);
	goto out;
    }
    check("server", 1, "driver %s %d.%d.%d, bus %s, SAREA 0x%lx", name, major, minor, patch,
	  busid, (unsigned long) hsarea);
    __driDrivers[0].name = name;

    glwin = mkwin(40, 40, GLW, GLH, WhitePixel(dpy, scr));
    ox = 40 + (GLW - CW) / 2;
    oy = 40 + (GLH - CH) / 2;
    cover = mkwin(ox, oy, CW, CH, WhitePixel(dpy, scr));
    XSync(dpy, False);

    tmpl.visualid = XVisualIDFromVisual(DefaultVisual(dpy, scr));
    vi = XGetVisualInfo(dpy, VisualIDMask, &tmpl, &n);
    check("glx", glXQueryExtension(dpy, 0, 0) && vi != 0, "vendor %s",
	  glXQueryServerString(dpy, scr, GLX_VENDOR));
    ctx = glXCreateContext(dpy, vi, NULL, True);
    if (!ctx || !nscreen) {
	check("screen", 0, "no direct context");
	goto out;
    }
    info = (AMIXDRIPtr) nscreen->pDevPriv;
    check("screen", nscreen->fd >= 0 && nscreen->pSAREA && nscreen->devPrivSize >= sizeof *info,
	  "drm fd %d, session %u, frame 0x%x, stride %u", nscreen->fd,
	  info->session, info->frontOffset, info->stride);
    check("direct", glXIsDirect(dpy, ctx), "context is direct");
    if (!glXMakeCurrent(dpy, glwin, ctx) || !ndraw) {
	check("drawable", 0, "make current failed");
	goto out;
    }
    check("drawable", ndraw->index < SAREA_MAX_DRAWABLES && ndraw->w == GLW && ndraw->h == GLH,
	  "index %d at %d,%d %dx%d", ndraw->index, ndraw->x, ndraw->y, ndraw->w, ndraw->h);
    check("cliprects", ndraw->numClipRects == 4 && cover_ok(ndraw, ox, oy),
	  "%d around the cover", ndraw->numClipRects);

    stamp = ndraw->lastStamp;
    ox = 40 + GLW - CW / 2;
    XMoveWindow(dpy, cover, ox, oy);
    XSync(dpy, False);
    check("stamp", *ndraw->pStamp != stamp, "%u -> %u after the cover moved", stamp, *ndraw->pStamp);
    validate(ndraw);
    check("cliprects", ndraw->numClipRects >= 2 && cover_ok(ndraw, ox, oy),
	  "%d after the move", ndraw->numClipRects);
    paint(ndraw);

    deadlock(busid);
    switchaway(info);
    glXMakeCurrent(dpy, None, NULL);
out:
    check("software", c == 'P', "Mesa without a driver");
    check("result", !fails, "%d failed", fails);
    report();
    printf("dritest: %s\n", fails ? "FAIL" : "PASS");
    fflush(stdout);
    sleep(hold);
    kill(sw, SIGTERM);
    waitpid(sw, &status, 0);
    return fails != 0;
}
