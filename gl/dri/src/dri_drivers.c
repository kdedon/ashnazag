/*
 * The DRI drivers, by the name the X server gives for a screen.  Linked in
 * (__driDrivers), or with DRI_DLOPEN loaded from <dir>/<file>_dri.so, dir
 * from LIBGL_DRIVERS_PATH (colon-separated) or DRI_DIR.  A program that
 * brings its own driver defines __driDrivers itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "glxclient.h"
#include "dri_glx.h"

/* the driver file and its entry points' prefix, by server name */
const char *
__driFileName(const char *name)
{
    return strcmp(name, "radeon") == 0 ? "r200" : name;
}

#ifdef DRI_DLOPEN
#define	DRI_DIR	"/usr/x11r6/lib/modules/dri"
#define	RTLD_NOW	2

/* libc's own entry points: the dl* wrappers come only as an archive */
extern void *_dlopen(const char *, int);
extern void *_dlsym(void *, const char *);
extern char *_dlerror(void);
extern int _dlclose(void *);

__DRIdriverEntry __driDrivers[] = {
    { NULL, NULL, NULL }
};

/* the last driver loaded */
void *__driHandle;

__DRIdriverEntry *
__driLoadDriver(const char *name)
{
    static __DRIdriverEntry loaded[4];
    static int n;
    const char *f = __driFileName(name), *p = 0, *e;
    char path[256], sym[64];
    void *h = 0;
    int i;

    for (i = 0; i < n; i++)
	if (strcmp(loaded[i].name, name) == 0)
	    return &loaded[i];
    if (n == 4 || strlen(f) > 32)
	return NULL;
    if (getuid() == geteuid() && getgid() == getegid())
	p = getenv("LIBGL_DRIVERS_PATH");
    if (!p)
	p = DRI_DIR;
    for (; !h && *p; p = *e ? e + 1 : e) {
	if (!(e = strchr(p, ':')))
	    e = p + strlen(p);
	if (e == p || e - p > sizeof path - 48)
	    continue;
	sprintf(path, "%.*s/%s_dri.so", (int) (e - p), p, f);
	if ((h = _dlopen(path, RTLD_NOW)) == 0 && getenv("LIBGL_DEBUG"))
	    fprintf(stderr, "libGL: %s\n", _dlerror());
    }
    if (!h)
	return NULL;
    sprintf(sym, "%s_driCreateScreen", f);
    loaded[n].createScreen = (void *(*)()) _dlsym(h, sym);
    sprintf(sym, "%s_driRegisterExtensions", f);
    loaded[n].registerExtensions = (void *(*)(void)) _dlsym(h, sym);
    if (!loaded[n].createScreen) {
	if (getenv("LIBGL_DEBUG"))
	    fprintf(stderr, "libGL: %s has no %s_driCreateScreen\n", path, f);
	_dlclose(h);
	return NULL;
    }
    loaded[n].name = strdup(name);
    __driHandle = h;
    return &loaded[n++];
}
#else
extern void *tdfx_driCreateScreen(Display *, int, __DRIscreen *, int, __GLXvisualConfig *);
extern void tdfx_driRegisterExtensions(void);
extern void *r200_driCreateScreen(Display *, int, __DRIscreen *, int, __GLXvisualConfig *);
extern void r200_driRegisterExtensions(void);

__DRIdriverEntry __driDrivers[] = {
    { "tdfx", tdfx_driCreateScreen, (void *(*)(void)) tdfx_driRegisterExtensions },
    { "radeon", r200_driCreateScreen, (void *(*)(void)) r200_driRegisterExtensions },
    { NULL, NULL, NULL }
};

__DRIdriverEntry *
__driLoadDriver(const char *name)
{
    return NULL;
}
#endif
