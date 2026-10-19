/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// vid_fb.c -- software renderer to a /dev/fb display session
//
// Renders 320x200 in main memory and copies dirty rects to the screen,
// centred and scaled by an integer factor (-scale n, default the largest
// that fits).  8-bit screens take Quake's palette in their CLUT; 16- and
// 32-bit screens get each index through a lookup table.

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "quakedef.h"
#include "d_local.h"
#include "dsio.h"

extern int munmap();

viddef_t	vid;

#define	BASEWIDTH	320
#define	BASEHEIGHT	200

unsigned short	d_8to16table[256];
unsigned	d_8to24table[256];

int		vid_fbfd = -1;		// the session; in_fb.c binds input to it
static struct fbinfo	fi;
static char	*fb_map = (char *)-1;
static byte	*fb_org;		// first pixel of the game area
static int	fb_scale;
static int	fb_bpp;			// bytes per pixel
static unsigned long	fb_pix[256];	// index -> pixel, 16 and 32 bit
static int	fb_fullcopy;		// palette changed on a true-colour screen
static int	VID_highhunkmark;

static unsigned long Mask (unsigned long mask, int v)
{
	int	shift = 0, bits = 0;

	if (!mask)
		return 0;
	while (!(mask & 1))
	{
		mask >>= 1;
		shift++;
	}
	while (mask & 1)
	{
		mask >>= 1;
		bits++;
	}
	if (bits < 8)
		v >>= 8 - bits;
	return (unsigned long)v << shift;
}

void VID_SetPalette (unsigned char *palette)
{
	unsigned short	r[256], g[256], b[256];
	struct fbcmap	cm;
	int		i;

	for (i = 0; i < 256; i++)
	{
		r[i] = palette[i*3+0] * 0x101;
		g[i] = palette[i*3+1] * 0x101;
		b[i] = palette[i*3+2] * 0x101;
		fb_pix[i] = Mask (fi.fi_rmask, palette[i*3+0]) |
			Mask (fi.fi_gmask, palette[i*3+1]) |
			Mask (fi.fi_bmask, palette[i*3+2]);
	}
	if (vid_fbfd < 0)
		return;
	if (fb_bpp == 1)
	{
		cm.cm_start = 0;
		cm.cm_count = 256;
		cm.cm_red = r;
		cm.cm_green = g;
		cm.cm_blue = b;
		ioctl (vid_fbfd, FBIOPUTCMAP, &cm);
	}
	else
		fb_fullcopy = 1;
}

void VID_ShiftPalette (unsigned char *palette)
{
	VID_SetPalette (palette);
}

// Copy n bytes of an expanded row to the screen with longword stores
// wherever the screen address allows: the frame buffer is uncached bus
// space (Zorro III through the Mediator), where every store costs a bus
// cycle whatever its width, and a read costs more.  The RAM side may be
// misaligned; the 68020+ reads misaligned longs from memory.
static void FB_Copy (byte *dst, byte *src, int n)
{
	unsigned long	*d;
	byte		*s = src;

	while (n > 0 && ((long)dst & 3))
		*dst++ = *s++, n--;
	d = (unsigned long *)dst;
	for ( ; n >= 16; n -= 16, s += 16)
	{
		d[0] = *(unsigned long *)(s + 0);
		d[1] = *(unsigned long *)(s + 4);
		d[2] = *(unsigned long *)(s + 8);
		d[3] = *(unsigned long *)(s + 12);
		d += 4;
	}
	for ( ; n >= 4; n -= 4, s += 4)
		*d++ = *(unsigned long *)s;
	dst = (byte *)d;
	while (n-- > 0)
		*dst++ = *s++;
}

static void FB_Blit (byte *src, int stride, int x, int y, int w, int h)
{
	// Expand each source row once into RAM, then store it s times; the
	// screen is never read.
	static byte	line[BASEWIDTH * 4 * 8];
	int	s = fb_scale, i, j, k, n = w * fb_bpp * s;
	byte	*row = fb_org + (y * fi.fi_rowbytes + x * fb_bpp) * s;

	for (j = 0; j < h; j++, src += stride)
	{
		if (fb_bpp == 1)
		{
			byte	*d = line;

			for (i = 0; i < w; i++)
				for (k = 0; k < s; k++)
					*d++ = src[i];
		}
		else if (fb_bpp == 2)
		{
			unsigned short	*d = (unsigned short *)line;

			for (i = 0; i < w; i++)
				for (k = 0; k < s; k++)
					*d++ = fb_pix[src[i]];
		}
		else
		{
			unsigned long	*d = (unsigned long *)line;

			for (i = 0; i < w; i++)
				for (k = 0; k < s; k++)
					*d++ = fb_pix[src[i]];
		}
		for (k = 0; k < s; k++, row += fi.fi_rowbytes)
			FB_Copy (row, line, n);
	}
}

void VID_Init (unsigned char *palette)
{
	struct fbacq	acq;
	int		bsize, zsize, tsize, i, sw, sh;
	char		*dev = "/dev/fb";

	i = COM_CheckParm ("-fb");
	if (i && i < com_argc - 1)
		dev = com_argv[i+1];
	if ((vid_fbfd = open (dev, O_RDWR)) < 0 && !i)
		vid_fbfd = open (dev = "/dev/fb0", O_RDWR);
	if (vid_fbfd < 0)
		Sys_Error ("VID_Init: cannot open %s", dev);
	memset ((char *)&acq, 0, sizeof(acq));
	acq.fa_kind = FBK_USER;
	acq.fa_flags = FBA_FRONT;
	strcpy (acq.fa_name, "quake");
	if (ioctl (vid_fbfd, FBIOACQUIRE, &acq) < 0 ||
	    ioctl (vid_fbfd, FBIOGINFO, &fi) < 0)
		Sys_Error ("VID_Init: no display session on %s", dev);
	fb_bpp = fi.fi_depth / 8;
	if (fi.fi_layout != FBL_PACKED || (fb_bpp != 1 && fb_bpp != 2 && fb_bpp != 4) ||
	    (fb_bpp == 1 && fi.fi_visual != FBV_PSEUDO))
		Sys_Error ("VID_Init: %lu-bit screen not supported", fi.fi_depth);

	fb_map = (char *)mmap ((caddr_t)0, (size_t)fi.fi_size,
		PROT_READ | PROT_WRITE, MAP_SHARED, vid_fbfd, (off_t)0);
	if (fb_map == (char *)-1)
		Sys_Error ("VID_Init: cannot map %s", dev);

	sw = fi.fi_width / BASEWIDTH;
	sh = fi.fi_height / BASEHEIGHT;
	fb_scale = sw < sh ? sw : sh;
	i = COM_CheckParm ("-scale");
	if (i && i < com_argc - 1 && Q_atoi (com_argv[i+1]) < fb_scale)
		fb_scale = Q_atoi (com_argv[i+1]);
	if (fb_scale > 8)		// FB_Blit's row buffer
		fb_scale = 8;
	if (fb_scale < 1)
		Sys_Error ("VID_Init: screen smaller than %dx%d", BASEWIDTH, BASEHEIGHT);
	for (i = 0; i < fi.fi_height; i++)
		memset (fb_map + fi.fi_offset + i * fi.fi_rowbytes, 0, fi.fi_width * fb_bpp);
	fb_org = (byte *)fb_map + fi.fi_offset +
		(fi.fi_height - BASEHEIGHT * fb_scale) / 2 * fi.fi_rowbytes +
		(fi.fi_width - BASEWIDTH * fb_scale) / 2 * fb_bpp;
	fb_org = (byte *)((long)fb_org & ~3);

	vid.maxwarpwidth = vid.width = vid.conwidth = BASEWIDTH;
	vid.maxwarpheight = vid.height = vid.conheight = BASEHEIGHT;
	vid.rowbytes = vid.conrowbytes = BASEWIDTH;
	vid.numpages = 1;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));
	vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);
	vid.direct = NULL;

	bsize = vid.rowbytes * vid.height;
	tsize = D_SurfaceCacheForRes (vid.width, vid.height);
	zsize = vid.width * vid.height * sizeof(*d_pzbuffer);
	VID_highhunkmark = Hunk_HighMark ();
	d_pzbuffer = Hunk_HighAllocName (bsize + tsize + zsize, "video");
	if (!d_pzbuffer)
		Sys_Error ("VID_Init: not enough memory for video buffers");
	D_InitCaches (((byte *)d_pzbuffer) + zsize, tsize);
	vid.conbuffer = vid.buffer = (pixel_t *)(((byte *)d_pzbuffer) + zsize + tsize);

	VID_SetPalette (palette);
	Con_Printf ("%s: %lux%lu %lu-bit, %dx\n", fi.fi_name, fi.fi_width,
		fi.fi_height, fi.fi_depth, fb_scale);
}

void VID_Shutdown (void)
{
	if (fb_map != (char *)-1)
		munmap ((caddr_t)fb_map, (size_t)fi.fi_size);
	fb_map = (char *)-1;
	if (vid_fbfd >= 0)
		close (vid_fbfd);
	vid_fbfd = -1;
}

void VID_Update (vrect_t *rects)
{
	if (vid_fbfd < 0)
		return;
	if (fb_fullcopy)
	{
		fb_fullcopy = 0;
		FB_Blit (vid.buffer, vid.rowbytes, 0, 0, vid.width, vid.height);
		return;
	}
	for ( ; rects; rects = rects->pnext)
		FB_Blit (vid.buffer + rects->y * vid.rowbytes + rects->x,
			vid.rowbytes, rects->x, rects->y, rects->width, rects->height);
}

// the loading disc, drawn while the renderer is busy
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
	if (vid_fbfd >= 0)
		FB_Blit (pbitmap, width, x, y, width, height);
}

void D_EndDirectRect (int x, int y, int width, int height)
{
	if (vid_fbfd >= 0 && vid.buffer)
		FB_Blit (vid.buffer + y * vid.rowbytes + x, vid.rowbytes,
			x, y, width, height);
}
