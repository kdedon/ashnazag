/*
 * libGL glue for a server whose GLX is direct only: the dispatcher's
 * test for direct rendering, and the indirect renderer's entry points,
 * which no context reaches.
 */
#include "glxclient.h"

/* a DRI driver took this screen */
Bool
_real_glXDirectCapable(Display *dpy, int screen)
{
    __GLXdisplayPrivate *priv = __glXInitialize(dpy);

    return priv && priv->driDisplay.private && priv->screenConfigs &&
	priv->screenConfigs[screen].driScreen.private;
}

__GLapi *
__glXNewIndirectAPI(void)
{
    return NULL;
}

void
__glXInitVertexArrayState(__GLXcontext *gc)
{
}

void
__glFreeAttributeState(__GLXcontext *gc)
{
}

void
__glFillImage(__GLXcontext *gc, GLint dim, GLint width, GLint height,
	      GLint depth, GLenum format, GLenum type, const GLvoid *userdata,
	      GLubyte *newimage, GLubyte *modes)
{
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
