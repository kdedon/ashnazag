/*
 * grDRIFront: the front buffer moves to another place in VRAM, with its
 * own row length.  A host without windows draws into whichever bitmap
 * its caller names; the next grRenderBuffer loads the registers.
 */
#include <3dfx.h>
#include <glidesys.h>
#include <glide.h>
#include "fxglide.h"
#include <lindri.h>

void
grDRIFront(int offset, int stride)
{
	GR_DCL_GC;

	driInfo.fbOffset = offset;
	driInfo.stride = stride;
	if (gc)
		gc->buffers[gc->frontBuffer] = offset;
}
