/*
 * 1: the CP's ring, status page and DMA buffers in host memory through
 * PCI GART, for bridges known to let the card master into Amiga RAM.
 * 0: all in the card's VRAM; the card never masters across the bridge.
 */
int	radeon_pcigart = 0;
