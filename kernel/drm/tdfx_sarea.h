#ifndef _TDFX_SAREA_H_
#define _TDFX_SAREA_H_
/*
 * The tdfx driver's part of the shared area, after the DRI one.  The
 * first five are the client's own (command fifo position and owners);
 * the kernel fills the rest at first open with the VRAM layout an X
 * server's driver would have chosen.  Offsets are in the frame buffer
 * map; fifoSize 0 means no room for 3D.
 */
typedef struct {
	volatile int	fifoPtr;
	volatile int	fifoRead;
	volatile int	fifoOwner;
	volatile int	ctxOwner;
	volatile int	texOwner;
	int		fifoOffset, fifoSize;
	int		backOffset, depthOffset;
	int		textureOffset, textureSize;
	int		deviceID;	/* PCI device: Banshee 3, Voodoo3 5 */
} tdfx_sarea_t;
#endif
