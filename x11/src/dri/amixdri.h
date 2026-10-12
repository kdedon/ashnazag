/*
 * amixdri.h -- direct rendering for a display-service screen: the
 * calls the XFree86-DRI and GLX extensions make, and what a screen
 * hands over.
 */
#ifndef _AMIXDRI_H_
#define _AMIXDRI_H_

#include "xf86drm.h"
#include "sarea.h"

#include "amixdrirec.h"

extern Bool AmixDRIScreenInit(ScreenPtr pScreen, int fbfd, unsigned long offset,
			      int stride, int width, int height, int depth);

extern Bool AmixGLXInitVisuals(ScreenPtr pScreen);
extern Bool DRIExtensionInit(void);
extern void DRIReset(void);
extern Bool DRIScreenActive(ScreenPtr pScreen);
extern Bool DRIQueryDirectRenderingCapable(ScreenPtr pScreen, Bool *isCapable);
extern Bool DRIOpenConnection(ScreenPtr pScreen, drmHandlePtr hSAREA, char **busIdString);
extern Bool DRIAuthConnection(ScreenPtr pScreen, drmMagic magic);
extern Bool DRICloseConnection(ScreenPtr pScreen);
extern Bool DRIGetClientDriverName(ScreenPtr pScreen, int *major, int *minor,
				   int *patch, char **name);
extern Bool DRICreateContext(ScreenPtr pScreen, VisualPtr visual, XID context,
			     drmContextPtr pHWContext);
extern Bool DRIDestroyContext(ScreenPtr pScreen, XID context);
extern Bool DRICreateDrawable(ScreenPtr pScreen, Drawable id, DrawablePtr pDrawable,
			      drmDrawablePtr hHWDrawable);
extern Bool DRIDestroyDrawable(ScreenPtr pScreen, Drawable id, DrawablePtr pDrawable);
extern Bool DRIGetDrawableInfo(ScreenPtr pScreen, DrawablePtr pDrawable,
			       unsigned int *index, unsigned int *stamp,
			       int *X, int *Y, int *W, int *H,
			       int *numClipRects, XF86DRIClipRectPtr *pClipRects,
			       int *backX, int *backY,
			       int *numBackClipRects, XF86DRIClipRectPtr *pBackClipRects);
extern Bool DRIGetDeviceInfo(ScreenPtr pScreen, drmHandlePtr hFrameBuffer,
			     int *fbOrigin, int *fbSize, int *fbStride,
			     int *devPrivateSize, void **pDevPrivate);
extern int DRIOpenFullScreen(ScreenPtr pScreen, DrawablePtr pDrawable);
extern int DRICloseFullScreen(ScreenPtr pScreen, DrawablePtr pDrawable);

#endif
