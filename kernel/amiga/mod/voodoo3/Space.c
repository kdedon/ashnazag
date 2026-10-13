/*
 * The display service's mode on the Voodoo3: its size (640x480,
 * 800x600, 1024x768, 1152x864 or 1280x1024), and the frames of VRAM
 * kept for hidden sessions beside the front one's (alternate frame
 * buffers; -1 as many as fit).  Hidden sessions without one are kept in
 * main memory; the VRAM past them is the 3D driver's.
 */
int		v3ds_width = 1024;
int		v3ds_height = 768;
int		v3ds_altfb = 0;
