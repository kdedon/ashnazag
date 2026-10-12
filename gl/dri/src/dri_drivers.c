/*
 * The DRI drivers linked into libGL, by the name the X server gives for
 * a screen.  A program that brings its own driver defines this table
 * itself, and this member is not linked.
 */
#include "glxclient.h"
#include "dri_glx.h"

extern void *tdfx_driCreateScreen(Display *, int, __DRIscreen *, int, __GLXvisualConfig *);
extern void tdfx_driRegisterExtensions(void);

__DRIdriverEntry __driDrivers[] = {
    { "tdfx", tdfx_driCreateScreen, (void *(*)(void)) tdfx_driRegisterExtensions },
    { NULL, NULL, NULL }
};
