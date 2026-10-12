#ifndef _DRMCARD_H_
#define _DRMCARD_H_
/*
 * The card as its display backend hands it over: registers and frame
 * buffer (kernel address, physical address, size), the VRAM the
 * display sessions leave free, and the bridge device for DMA memory.
 */
struct drm_amixcard {
	volatile unsigned char	*c_reg;
	unsigned long		c_physreg, c_regsize;
	volatile unsigned char	*c_fb;
	unsigned long		c_physfb, c_fbsize;
	unsigned long		c_vram;		/* bytes on the card */
	unsigned long		c_free;		/* VRAM from here to c_vram is the DRM's */
	struct opci_dev		*c_dev;
};
#endif
