#ifndef _AMIXDRIREC_H_
#define _AMIXDRIREC_H_
/*
 * The device private every client gets (XF86DRIGetDeviceInfo): the
 * session's frame in the frame buffer map and the VRAM left for back,
 * depth and texture buffers.  A card's client driver reads it, or a
 * longer record that starts with it.
 */
typedef struct {
    unsigned int	session;	/* display session id */
    unsigned int	width, height;	/* screen in pixels */
    unsigned int	depth, cpp;	/* bits and bytes a pixel */
    unsigned int	stride;		/* bytes a row */
    unsigned int	frontOffset;	/* the frame in the frame buffer map */
    unsigned int	vramOffset;	/* free VRAM, an offset in that map... */
    unsigned int	vramSize;	/* ...and its size */
} AMIXDRIRec, *AMIXDRIPtr;
#endif
