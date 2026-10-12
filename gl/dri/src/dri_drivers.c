/*
 * The DRI drivers linked into libGL, by the name the X server gives for
 * a screen.  A program that brings its own driver defines this table
 * itself, and this member is not linked.
 */
#include "glxclient.h"
#include "dri_glx.h"

__DRIdriverEntry __driDrivers[] = {
    { NULL, NULL, NULL }
};
