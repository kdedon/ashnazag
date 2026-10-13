/*
 * The display service's mode on the Radeon: its size (640x480,
 * 800x600, 1024x768, 1152x864 or 1280x1024), the VRAM at the top kept
 * from sessions for 3D (back, depth, textures), and the card's PLL
 * reference clock in kHz.
 */
int		rds_width = 1024;
int		rds_height = 768;
unsigned long	rds_resv = 16L << 20;
long		rds_refkhz = 27000;
