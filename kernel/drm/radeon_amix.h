#ifndef _RADEON_AMIX_H_
#define _RADEON_AMIX_H_
/*
 * What an X server's radeon driver would hand its clients, filled by
 * the kernel at first open.  It sits in the shared area after the DRI
 * part (drm_sarea_t) and the radeon part (drm_radeon_sarea_t), at
 * RADEON_AMIX_LAYOUT.  Offsets are in the frame buffer map; handles are
 * map offsets for drmMap.  backSize 0 means no room for 3D.  The front
 * buffer is the client's display session (AMIXDRIRec); back and depth
 * buffers share its pitch and pixel size (depth 16 or 24 + 8 stencil).
 * The ring, status page and DMA buffers sit below the back buffer when
 * in VRAM; their maps reach it through the card's unswapped second
 * aperture, so clients fill buffers as they would host memory.
 */
typedef struct {
	int		deviceID;
	int		chipFamily;	/* RADEON_FAMILY_* */
	int		width, height;	/* back and depth buffers in pixels */
	int		backOffset, backSize;	/* sized for 32 bits a pixel */
	int		depthOffset, depthSize;
	int		textureOffset, textureSize;
	int		log2TexGran;
	unsigned long	registerHandle, registerSize;
	unsigned long	statusHandle, statusSize;	/* ring read pointer and scratch */
	unsigned int	sarea_priv_offset;
	int		irq;		/* nonzero: fences by interrupt */
	int		cpInVram;	/* ring, status and DMA buffers in VRAM, else PCI GART */
} radeon_amix_layout_t;

#define RADEON_FAMILY_R100	1
#define RADEON_FAMILY_R200	2

#define RADEON_AMIX_LAYOUT	(sizeof(drm_sarea_t) + sizeof(drm_radeon_sarea_t))
#endif
