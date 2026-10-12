/*
 * The display service's mode on the Voodoo3: its size (640x480,
 * 800x600, 1024x768, 1152x864 or 1280x1024), and the VRAM at the top
 * kept from sessions for a 3D driver's buffers: 4 MB less the page over
 * each of the three sessions' frames.
 */
int		v3ds_width = 1024;
int		v3ds_height = 768;
unsigned long	v3ds_resv = (4L << 20) - (3L << 12);
