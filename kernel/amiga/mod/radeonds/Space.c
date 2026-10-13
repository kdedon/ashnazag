/*
 * The display service's mode on the Radeon: its size (640x480,
 * 800x600, 1024x768, 1152x864 or 1280x1024), the frames of VRAM kept
 * for hidden sessions beside the front one's (alternate frame buffers;
 * -1 as many as fit; the rest is for 3D), and the card's PLL reference
 * clock in kHz.
 */
int		rds_width = 1024;
int		rds_height = 768;
int		rds_altfb = 0;
long		rds_refkhz = 27000;
