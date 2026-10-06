/*
 * fbcard.c -- RTG card over a display-service frame buffer (/dev/fbN).
 *
 * The screen is a session of the display service: VRAM while in front,
 * shadow pages while hidden; the kernel swaps them and the colour table
 * on a switch.  Packed 1 bpp is drawn by mfb (pixel 0 white, as on the
 * Mac), packed 8 bpp by cfb and interleaved 8 bitplanes by iplan2p8,
 * both with the colour table loaded from the installed colormap.  Input
 * comes from the event devices bound to the session.
 */

#include "../../amix.h"
#include "servermd.h"
#include "resource.h"
#include "colormapst.h"
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "dsio.h"
#include "../rtg.h"

extern fbFd amixFbs[];
extern int amixCurrentScreenIndex;
extern Bool amixEvMb3;
extern Bool amixEvOpen();
extern Bool mfbScreenInit(), mfbCreateDefColormap();
extern Bool cfbScreenInit(), cfbCreateDefColormap(), cfbSetVisualTypes();
extern Bool ipl2p8ScreenInit(), iplCreateDefColormap(), iplSetVisualTypes();
extern Bool amixMacKeyLayout();
extern int TellLostMap(), TellGainedMap();

typedef struct {
    int		  fd;
    struct fbinfo fi;
    unsigned char *map;
    ColormapPtr	  installed;
} fbDevRec;

static fbDevRec fbDev[MAXSCREENS];
static char	*fbName[MAXSCREENS];
static int	nfbName;
static int	fbDepth;	/* -depth: the session mode to set, 0 the display's */

#define fbIpl(d)	((d)->fi.fi_layout == FBL_IPLAN2)

/* the largest mode of fbDepth, if the display offers modes */
static void
fbSetMode(d, name)
fbDevRec *d;
char	 *name;
{
    struct fbmodeinfo mi[16];
    struct fbmodes    ms;
    int		      i, best = -1;

    if (!fbDepth || d->fi.fi_depth == fbDepth || !(d->fi.fi_flags & FBF_SETMODE))
	return;
    ms.ms_count = 16;
    ms.ms_modes = mi;
    if (ioctl(d->fd, FBIOGMODES, &ms) < 0)
	return;
    for (i = 0; i < ms.ms_count && i < 16; i++)
	if (mi[i].mi_depth == fbDepth && (best < 0 ||
	    mi[i].mi_width * mi[i].mi_height > mi[best].mi_width * mi[best].mi_height))
	    best = i;
    if (best < 0)
	ErrorF("fb: %s: no depth %d mode\n", name, fbDepth);
    else if (ioctl(d->fd, FBIOSMODE, mi[best].mi_id) < 0 ||
	     ioctl(d->fd, FBIOGINFO, &d->fi) < 0)
	ErrorF("fb: %s: FBIOSMODE: %s\n", name, strerror(errno));
}

static Bool
fbProbe(index)
int index;
{
    fbDevRec	 *d = &fbDev[index];
    struct fbacq  a;
    char	 *name = fbName[index] ? fbName[index] :
			 index == 0 ? "/dev/fb0" : (char *) 0;

    if (!name)
	return FALSE;
    if ((d->fd = open(name, O_RDWR)) < 0)
    {
	ErrorF("fb: %s: %s\n", name, strerror(errno));
	return FALSE;
    }
    memset((char *) &a, 0, sizeof a);
    a.fa_kind = FBK_USER;
    a.fa_flags = FBA_FRONT;
    strcpy(a.fa_name, "X");
    if (ioctl(d->fd, FBIOACQUIRE, &a) < 0)
    {
	/* not at the console: the screen stays with its session */
	a.fa_flags = 0;
	if (errno != EPERM || ioctl(d->fd, FBIOACQUIRE, &a) < 0)
	{
	    ErrorF("fb: FBIOACQUIRE %s: %s\n", name, strerror(errno));
	    goto fail;
	}
	ErrorF("fb: session %ld in the background; bring it to front with "
	       "Control-Option-Command and its number\n", (long) a.fa_id);
    }
    if (ioctl(d->fd, FBIOGINFO, &d->fi) < 0)
    {
	ErrorF("fb: FBIOGINFO: %s\n", strerror(errno));
	goto fail;
    }
    fbSetMode(d, name);
    if (d->fi.fi_offset > d->fi.fi_size ||
	d->fi.fi_offset + d->fi.fi_rowbytes * d->fi.fi_height > d->fi.fi_size ||
	d->fi.fi_rowbytes < (d->fi.fi_width * d->fi.fi_depth + 7) / 8 ||
	(fbIpl(d) && d->fi.fi_rowbytes % 16))
    {
	ErrorF("fb: %s: inconsistent geometry\n", name);
	goto fail;
    }
    if (!(d->fi.fi_layout == FBL_PACKED &&
	  (d->fi.fi_depth == 1 || d->fi.fi_depth == 8)) &&
	!(fbIpl(d) && d->fi.fi_depth == 8))
    {
	ErrorF("fb: %s: depth %lu layout %lu not supported\n", name,
	       d->fi.fi_depth, d->fi.fi_layout);
	goto fail;
    }
    amixFbs[index].bp.width = d->fi.fi_width;
    amixFbs[index].bp.height = d->fi.fi_height;
    ErrorF("fb: %s %.16s %lux%lu depth %lu rowbytes %lu\n", name,
	   d->fi.fi_name, d->fi.fi_width, d->fi.fi_height, d->fi.fi_depth,
	   d->fi.fi_rowbytes);
    return TRUE;

fail:
    close(d->fd);
    d->fd = -1;
    return FALSE;
}

static Bool
fbCreate(pScreenInfo, index)
ScreenInfo *pScreenInfo;
int	    index;
{
    int i, depth = fbDev[index].fi.fi_depth;

    for (i = 0; i < pScreenInfo->numPixmapFormats; i++)
	if (pScreenInfo->formats[i].depth == depth)
	    return TRUE;
    pScreenInfo->formats[i].depth = depth;
    pScreenInfo->formats[i].bitsPerPixel = depth;
    pScreenInfo->formats[i].scanlinePad = BITMAP_SCANLINE_PAD;
    pScreenInfo->numPixmapFormats = i + 1;
    return TRUE;
}

static Bool
fbInitHW(pRTG, index)
rtgScreenPtr pRTG;
int	     index;
{
    fbDevRec *d = &fbDev[index];

    if (!d->map)
    {
	d->map = (unsigned char *) mmap((caddr_t) 0, d->fi.fi_size,
			PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t) 0);
	if (d->map == (unsigned char *) -1)
	{
	    ErrorF("fb: mmap: %s\n", strerror(errno));
	    d->map = 0;
	    return FALSE;
	}
    }
    pRTG->frameBase = d->map;
    pRTG->fbOffset = d->fi.fi_offset;
    pRTG->fbBase = (unsigned short *) (d->map + d->fi.fi_offset);
    pRTG->width = d->fi.fi_width;
    pRTG->height = d->fi.fi_height;
    pRTG->pitch = d->fi.fi_rowbytes;
    pRTG->bitsPerPixel = d->fi.fi_depth;
    pRTG->depth = d->fi.fi_depth;
    pRTG->cardPrivate = (unsigned char *) d;
    return TRUE;
}

/* The session lives as long as the server: a reset keeps screen and input */
static void
fbCloseHW(pRTG, index)
rtgScreenPtr pRTG;
int	     index;
{
    fbDev[index].installed = (ColormapPtr) 0;
}

static void
fbPutCmap(d, start, count, r, g, b)
fbDevRec	*d;
int		start, count;
unsigned short	*r, *g, *b;
{
    struct fbcmap cm;

    if (!(d->fi.fi_flags & FBF_CMAP))
	return;
    cm.cm_start = start;
    cm.cm_count = count;
    cm.cm_red = r;
    cm.cm_green = g;
    cm.cm_blue = b;
    if (ioctl(d->fd, FBIOPUTCMAP, &cm) < 0)
	ErrorF("fb: FBIOPUTCMAP: %s\n", strerror(errno));
}

#define fbDevOf(s)	((fbDevRec *) GetRTGScreen(s)->cardPrivate)

static void
fbLoadMap(pmap)
ColormapPtr pmap;
{
    static unsigned short r[256], g[256], b[256];
    fbDevRec *d = fbDevOf(pmap->pScreen);
    int	      i, n = pmap->pVisual->ColormapEntries;
    Entry    *e;

    if (n > 256)
	n = 256;
    for (i = 0; i < n; i++)
    {
	e = &pmap->red[i];
	if (e->fShared)
	{
	    r[i] = e->co.shco.red->color;
	    g[i] = e->co.shco.green->color;
	    b[i] = e->co.shco.blue->color;
	}
	else
	{
	    r[i] = e->co.local.red;
	    g[i] = e->co.local.green;
	    b[i] = e->co.local.blue;
	}
    }
    fbPutCmap(d, 0, n, r, g, b);
}

static void
fbInstallColormap(pmap)
ColormapPtr pmap;
{
    fbDevRec *d = fbDevOf(pmap->pScreen);

    if (pmap == d->installed)
	return;
    if (d->installed)
	WalkTree(pmap->pScreen, TellLostMap, (pointer) &d->installed->mid);
    d->installed = pmap;
    fbLoadMap(pmap);
    WalkTree(pmap->pScreen, TellGainedMap, (pointer) &pmap->mid);
}

static void
fbUninstallColormap(pmap)
ColormapPtr pmap;
{
    fbDevRec   *d = fbDevOf(pmap->pScreen);
    ColormapPtr def;

    if (pmap != d->installed)
	return;
    def = (ColormapPtr) LookupIDByType(pmap->pScreen->defColormap, RT_COLORMAP);
    if (def && def != pmap)
	(*pmap->pScreen->InstallColormap)(def);
}

static int
fbListInstalledColormaps(pScreen, pmaps)
ScreenPtr pScreen;
Colormap *pmaps;
{
    fbDevRec *d = fbDevOf(pScreen);

    if (!d->installed)
	return 0;
    *pmaps = d->installed->mid;
    return 1;
}

static void
fbStoreColors(pmap, ndef, pdefs)
ColormapPtr pmap;
int	    ndef;
xColorItem *pdefs;
{
    fbDevRec *d = fbDevOf(pmap->pScreen);

    if (pmap != d->installed)
	return;
    for (; ndef > 0; ndef--, pdefs++)
	if (pdefs->pixel < 256)
	    fbPutCmap(d, (int) pdefs->pixel, 1,
		      &pdefs->red, &pdefs->green, &pdefs->blue);
}

static Bool
fbScreenInit(pScreen, pRTG, dpix, dpiy)
ScreenPtr    pScreen;
rtgScreenPtr pRTG;
int	     dpix, dpiy;
{
    fbDevRec *d = (fbDevRec *) pRTG->cardPrivate;
    pointer   base = (pointer) (d->map + d->fi.fi_offset);

    if (d->fi.fi_mmwidth && d->fi.fi_mmheight)
    {
	dpix = (d->fi.fi_width * 254 + d->fi.fi_mmwidth * 5) / (d->fi.fi_mmwidth * 10);
	dpiy = (d->fi.fi_height * 254 + d->fi.fi_mmheight * 5) / (d->fi.fi_mmheight * 10);
    }
    if (d->fi.fi_depth == 1)
    {
	static unsigned short wb[2] = { 0xFFFF, 0 };

	if (!mfbScreenInit(pScreen, base, pRTG->width, pRTG->height,
			   dpix, dpiy, pRTG->pitch * 8))
	    return FALSE;
	pScreen->whitePixel = 0;
	pScreen->blackPixel = 1;
	fbPutCmap(d, 0, 2, wb, wb, wb);
	return TRUE;
    }
    if (fbIpl(d))
    {
	if (!iplSetVisualTypes(8, (1 << PseudoColor) | (1 << GrayScale) |
			       (1 << StaticGray), (int) d->fi.fi_cmapbits) ||
	    !ipl2p8ScreenInit(pScreen, base, pRTG->width, pRTG->height,
			      dpix, dpiy, pRTG->pitch))
	    return FALSE;
    }
    else if (!cfbSetVisualTypes(8, (1 << PseudoColor) | (1 << GrayScale) |
				(1 << StaticGray), (int) d->fi.fi_cmapbits) ||
	     !cfbScreenInit(pScreen, base, pRTG->width, pRTG->height,
			    dpix, dpiy, pRTG->pitch))
	return FALSE;
    pScreen->InstallColormap = fbInstallColormap;
    pScreen->UninstallColormap = fbUninstallColormap;
    pScreen->ListInstalledColormaps = fbListInstalledColormaps;
    pScreen->StoreColors = fbStoreColors;
    return TRUE;
}

static Bool
fbCreateDefColormap(pScreen)
ScreenPtr pScreen;
{
    if (fbDevOf(pScreen)->fi.fi_depth == 1)
	return mfbCreateDefColormap(pScreen);
    if (fbIpl(fbDevOf(pScreen)))
	return iplCreateDefColormap(pScreen);
    return cfbCreateDefColormap(pScreen);
}

static Bool
fbSaveScreen(pScreen, on)
ScreenPtr pScreen;
int	  on;
{
    if (on == SCREEN_SAVER_FORCER)
    {
	SetTimeSinceLastInputEvent();
	on = SCREEN_SAVER_OFF;
    }
    (void) ioctl(fbDevOf(pScreen)->fd, FBIOBLANK, on == SCREEN_SAVER_ON);
    return TRUE;
}

static void
fbOpenInput(index)
int index;
{
    if (!amixEvOpen(fbDev[index].fd))
    {
	ErrorF("fb: no keyboard or mouse; starting display-only\n");
	return;
    }
    amixFbs[index].fd = fbDev[index].fd;
    amixFbs[index].mapped = TRUE;
    if (amixCurrentScreenIndex == -1)
	amixCurrentScreenIndex = index;
}

static void
fbCloseInput(index)
int index;
{
}

static int
fbProcessArgument(argc, argv, i)
int    argc;
char **argv;
int    i;
{
    if (strcmp(argv[i], "-fb") == 0 && i + 1 < argc && nfbName < MAXSCREENS)
    {
	/* the server may run as root: only device nodes */
	if (strncmp(argv[i + 1], "/dev/", 5) != 0 || strstr(argv[i + 1], ".."))
	    FatalError("-fb: %s is not a device path\n", argv[i + 1]);
	fbName[nfbName++] = argv[i + 1];
	return 2;
    }
    if (strcmp(argv[i], "-depth") == 0 && i + 1 < argc)
    {
	fbDepth = atoi(argv[i + 1]);
	return 2;
    }
    if (strcmp(argv[i], "-kbd") == 0 && i + 1 < argc)
    {
	if (!amixMacKeyLayout(argv[i + 1]))
	    FatalError("-kbd: layout %s unknown (us, de)\n", argv[i + 1]);
	return 2;
    }
    if (strcmp(argv[i], "-mb3") == 0)
    {
	amixEvMb3 = TRUE;
	return 1;
    }
    return 0;
}

static void
fbUseMsg()
{
    ErrorF("-fb /dev/fbN           frame buffer of the next screen (/dev/fb0)\n");
    ErrorF("-depth n               the display's largest mode of depth n\n");
    ErrorF("-kbd us|de             keyboard layout\n");
    ErrorF("-mb3                   left/right arrow are buttons 2/3 (Option: arrows)\n");
}

rtgCardRec fbCard = {
    "fb",
    fbProbe,
    fbCreate,
    fbInitHW,
    fbCloseHW,
    fbScreenInit,
    fbCreateDefColormap,
    fbSaveScreen,
    fbOpenInput,
    fbCloseInput,
    fbProcessArgument,
    fbUseMsg,
};
