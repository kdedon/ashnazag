/*
 * glxamix.c -- GLX for direct rendering only.
 *
 * The server has no GL renderer: a client's library renders through a
 * DRI driver into the frame buffer, and only asks the server for the
 * visual configurations and for context names.  Contexts that are not
 * direct, GLX pixmaps and rendering requests are refused.  Clients are
 * local (no TCP), so they share the server's byte order.
 */
#include "X.h"
#include "Xproto.h"
#include "misc.h"
#include "dixstruct.h"
#include "extnsionst.h"
#include "resource.h"
#include "scrnintstr.h"
#include <string.h>
#include "GL/glxproto.h"
#include "GL/glxtokens.h"
#define _XF86DRI_SERVER_
#include "xf86dristr.h"
#include "amixdri.h"

#define GLX_SERVER_MAJOR	1
#define GLX_SERVER_MINOR	2

/* a visual configuration: 18 values, then 7 extension token/value pairs */
#define GLX_CONFIG_PROPS	(18 + 2 * 7)

typedef struct {
    int		screen;
    VisualID	vid;
    Bool	isDirect;
} GLXContextRec;

static RESTYPE GLXContextResType;
static int GLXErrorBase;

static int
GLXContextDelete(value, id)
pointer value;
XID id;
{
    xfree(value);
    return Success;
}

static int
bits(m)
unsigned long m;
{
    int n;

    for (n = 0; m; m >>= 1)
	n += m & 1;
    return n;
}

/* one configuration per X visual: double buffered, 16-bit depth */
static int
GLXGetVisualConfigs(client)
ClientPtr client;
{
    xGLXGetVisualConfigsReq *req = (xGLXGetVisualConfigsReq *) client->requestBuffer;
    xGLXGetVisualConfigsReply rep;
    CARD32 buf[GLX_CONFIG_PROPS];
    ScreenPtr pScreen;
    VisualPtr v;
    int i, p, rgba;

    REQUEST_SIZE_MATCH(xGLXGetVisualConfigsReq);
    if (req->screen >= screenInfo.numScreens) {
	client->errorValue = req->screen;
	return BadValue;
    }
    pScreen = screenInfo.screens[req->screen];
    memset(&rep, 0, sizeof rep);
    rep.type = X_Reply;
    rep.sequenceNumber = client->sequence;
    rep.numVisuals = pScreen->numVisuals;
    rep.numProps = GLX_CONFIG_PROPS;
    rep.length = pScreen->numVisuals * GLX_CONFIG_PROPS;
    WriteToClient(client, sz_xGLXGetVisualConfigsReply, (char *) &rep);
    for (i = 0, v = pScreen->visuals; i < pScreen->numVisuals; i++, v++) {
	rgba = v->class == TrueColor || v->class == DirectColor;
	p = 0;
	buf[p++] = v->vid;
	buf[p++] = v->class;
	buf[p++] = rgba;
	buf[p++] = rgba ? bits(v->redMask) : 0;
	buf[p++] = rgba ? bits(v->greenMask) : 0;
	buf[p++] = rgba ? bits(v->blueMask) : 0;
	buf[p++] = 0;			/* alpha */
	buf[p++] = 0;			/* accumulation red, green, blue, alpha */
	buf[p++] = 0;
	buf[p++] = 0;
	buf[p++] = 0;
	buf[p++] = TRUE;		/* double buffer */
	buf[p++] = FALSE;		/* stereo */
	buf[p++] = v->nplanes;
	buf[p++] = 16;			/* depth */
	buf[p++] = 0;			/* stencil */
	buf[p++] = 0;			/* aux buffers */
	buf[p++] = 0;			/* level */
	buf[p++] = GLX_VISUAL_CAVEAT_EXT;
	buf[p++] = GLX_NONE_EXT;
	buf[p++] = GLX_TRANSPARENT_TYPE_EXT;
	buf[p++] = GLX_NONE_EXT;
	buf[p++] = GLX_TRANSPARENT_RED_VALUE_EXT;
	buf[p++] = 0;
	buf[p++] = GLX_TRANSPARENT_GREEN_VALUE_EXT;
	buf[p++] = 0;
	buf[p++] = GLX_TRANSPARENT_BLUE_VALUE_EXT;
	buf[p++] = 0;
	buf[p++] = GLX_TRANSPARENT_ALPHA_VALUE_EXT;
	buf[p++] = 0;
	buf[p++] = GLX_TRANSPARENT_INDEX_VALUE_EXT;
	buf[p++] = 0;
	WriteToClient(client, sizeof buf, (char *) buf);
    }
    return client->noClientException;
}

static int
GLXCreateContext(client)
ClientPtr client;
{
    xGLXCreateContextReq *req = (xGLXCreateContextReq *) client->requestBuffer;
    GLXContextRec *c;
    ScreenPtr pScreen;
    int i;

    REQUEST_SIZE_MATCH(xGLXCreateContextReq);
    LEGAL_NEW_RESOURCE(req->context, client);
    if (req->screen >= screenInfo.numScreens) {
	client->errorValue = req->screen;
	return BadValue;
    }
    pScreen = screenInfo.screens[req->screen];
    for (i = 0; i < pScreen->numVisuals; i++)
	if (pScreen->visuals[i].vid == req->visual)
	    break;
    if (i == pScreen->numVisuals) {
	client->errorValue = req->visual;
	return BadValue;
    }
    if (!req->isDirect)
	return BadMatch;
    if ((c = (GLXContextRec *) xalloc(sizeof *c)) == 0)
	return BadAlloc;
    c->screen = req->screen;
    c->vid = req->visual;
    c->isDirect = TRUE;
    if (!AddResource(req->context, GLXContextResType, (pointer) c))
	return BadAlloc;
    return client->noClientException;
}

static int
GLXDestroyContext(client)
ClientPtr client;
{
    xGLXDestroyContextReq *req = (xGLXDestroyContextReq *) client->requestBuffer;

    REQUEST_SIZE_MATCH(xGLXDestroyContextReq);
    if (!LookupIDByType(req->context, GLXContextResType)) {
	client->errorValue = req->context;
	return GLXErrorBase + GLXBadContext;
    }
    FreeResource(req->context, RT_NONE);
    return client->noClientException;
}

/* only releasing the current context reaches the server */
static int
GLXMakeCurrent(client)
ClientPtr client;
{
    xGLXMakeCurrentReq *req = (xGLXMakeCurrentReq *) client->requestBuffer;
    xGLXMakeCurrentReply rep;

    REQUEST_SIZE_MATCH(xGLXMakeCurrentReq);
    if (req->context != None)
	return BadMatch;
    memset(&rep, 0, sizeof rep);
    rep.type = X_Reply;
    rep.sequenceNumber = client->sequence;
    WriteToClient(client, sz_xGLXMakeCurrentReply, (char *) &rep);
    return client->noClientException;
}

static int
GLXIsDirect(client)
ClientPtr client;
{
    xGLXIsDirectReq *req = (xGLXIsDirectReq *) client->requestBuffer;
    xGLXIsDirectReply rep;
    GLXContextRec *c;

    REQUEST_SIZE_MATCH(xGLXIsDirectReq);
    if ((c = (GLXContextRec *) LookupIDByType(req->context, GLXContextResType)) == 0) {
	client->errorValue = req->context;
	return GLXErrorBase + GLXBadContext;
    }
    memset(&rep, 0, sizeof rep);
    rep.type = X_Reply;
    rep.sequenceNumber = client->sequence;
    rep.isDirect = c->isDirect;
    WriteToClient(client, sz_xGLXIsDirectReply, (char *) &rep);
    return client->noClientException;
}

static int
GLXQueryVersion(client)
ClientPtr client;
{
    xGLXQueryVersionReply rep;

    REQUEST_SIZE_MATCH(xGLXQueryVersionReq);
    memset(&rep, 0, sizeof rep);
    rep.type = X_Reply;
    rep.sequenceNumber = client->sequence;
    rep.majorVersion = GLX_SERVER_MAJOR;
    rep.minorVersion = GLX_SERVER_MINOR;
    WriteToClient(client, sz_xGLXQueryVersionReply, (char *) &rep);
    return client->noClientException;
}

/* a string reply: n bytes with the terminator, padded */
static int
GLXString(client, s)
ClientPtr client;
char *s;
{
    xGLXQueryServerStringReply rep;
    int n = strlen(s) + 1;

    memset(&rep, 0, sizeof rep);
    rep.type = X_Reply;
    rep.sequenceNumber = client->sequence;
    rep.n = n;
    rep.length = (n + 3) >> 2;
    WriteToClient(client, sz_xGLXQueryServerStringReply, (char *) &rep);
    WriteToClient(client, n, s);
    return client->noClientException;
}

static int
GLXQueryServerString(client)
ClientPtr client;
{
    xGLXQueryServerStringReq *req = (xGLXQueryServerStringReq *) client->requestBuffer;

    REQUEST_SIZE_MATCH(xGLXQueryServerStringReq);
    if (req->screen >= screenInfo.numScreens) {
	client->errorValue = req->screen;
	return BadValue;
    }
    switch (req->name) {
    case GLX_VENDOR:
	return GLXString(client, "SGI");
    case GLX_VERSION:
	return GLXString(client, "1.2");
    case GLX_EXTENSIONS:
	return GLXString(client, "");
    }
    client->errorValue = req->name;
    return BadValue;
}

static int
GLXQueryExtensionsString(client)
ClientPtr client;
{
    xGLXQueryExtensionsStringReq *req = (xGLXQueryExtensionsStringReq *) client->requestBuffer;

    REQUEST_SIZE_MATCH(xGLXQueryExtensionsStringReq);
    if (req->screen >= screenInfo.numScreens) {
	client->errorValue = req->screen;
	return BadValue;
    }
    return GLXString(client, "");
}

static int
GLXDispatch(client)
ClientPtr client;
{
    REQUEST(xReq);

    if (client->swapped)
	return BadRequest;
    switch (stuff->data) {
    case X_GLXCreateContext:
	return GLXCreateContext(client);
    case X_GLXDestroyContext:
	return GLXDestroyContext(client);
    case X_GLXMakeCurrent:
	return GLXMakeCurrent(client);
    case X_GLXIsDirect:
	return GLXIsDirect(client);
    case X_GLXQueryVersion:
	return GLXQueryVersion(client);
    case X_GLXWaitGL:
    case X_GLXWaitX:
    case X_GLXClientInfo:
	return client->noClientException;
    case X_GLXGetVisualConfigs:
	return GLXGetVisualConfigs(client);
    case X_GLXQueryExtensionsString:
	return GLXQueryExtensionsString(client);
    case X_GLXQueryServerString:
	return GLXQueryServerString(client);
    }
    return BadRequest;
}

static void
GLXReset(extEntry)
ExtensionEntry *extEntry;
{
}

/* only where a screen renders directly */
void
GlxExtensionInit()
{
    ExtensionEntry *e;
    int i;

    for (i = 0; i < screenInfo.numScreens; i++)
	if (DRIScreenActive(screenInfo.screens[i]))
	    break;
    if (i == screenInfo.numScreens)
	return;
    if ((GLXContextResType = CreateNewResourceType(GLXContextDelete)) == 0)
	return;
    e = AddExtension(GLX_EXTENSION_NAME, __GLX_NUMBER_EVENTS, __GLX_NUMBER_ERRORS,
		     GLXDispatch, GLXDispatch, GLXReset, StandardMinorOpcode);
    if (e)
	GLXErrorBase = e->errorBase;
}
