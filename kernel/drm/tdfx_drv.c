/* tdfx_drv.c -- tdfx driver -*- linux-c -*-
 * Created: Thu Oct  7 10:38:32 1999 by faith@precisioninsight.com
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Rickard E. (Rik) Faith <faith@valinux.com>
 *    Daryll Strauss <daryll@valinux.com>
 *    Gareth Hughes <gareth@valinux.com>
 */


#ifndef __AMIX__
#include <sys/types.h>
#endif

#include "tdfx.h"
#include "drmP.h"

#ifdef __AMIX__
/* status bit 9, busy, clear three reads running */
#define DRIVER_IDLE(dev)						\
do {									\
	int n_, i_;							\
	for (n_ = i_ = 0; n_ < 100000 && i_ < 3; n_++)			\
		i_ = DRM_READ32(DRM(card).c_reg) & (1 << 9) ? 0 : i_ + 1; \
} while (0)

/* the X server asks for an idle card before it draws */
#define __HAVE_DMA_QUIESCENT	1
#define DRIVER_DMA_QUIESCENT()	DRIVER_IDLE(dev)

#define DRIVER_AMIX_MAPS(dev)	tdfx_amix_layout(dev)
static void tdfx_amix_layout(drm_device_t *dev);

/*
 * A client killed while it held the lock may have left half a packet in
 * the command fifo, and the card would read the next client's words as
 * its rest: finish it, then start the fifo afresh.
 */
#define HAVE_DRIVER_RELEASE	1
#define DRIVER_RELEASE()	tdfx_amix_recover(dev)
static void tdfx_amix_recover(drm_device_t *dev);
#endif

#define DRIVER_AUTHOR		"VA Linux Systems Inc."

#define DRIVER_NAME		"tdfx"
#define DRIVER_DESC		"3dfx Banshee/Voodoo3+"
#define DRIVER_DATE		"20010216"

#define DRIVER_MAJOR		1
#define DRIVER_MINOR		0
#define DRIVER_PATCHLEVEL	0

#ifndef PCI_VENDOR_ID_3DFX
#define PCI_VENDOR_ID_3DFX 0x121A
#endif
#ifndef PCI_DEVICE_ID_3DFX_VOODOO5
#define PCI_DEVICE_ID_3DFX_VOODOO5 0x0009
#endif
#ifndef PCI_DEVICE_ID_3DFX_VOODOO4
#define PCI_DEVICE_ID_3DFX_VOODOO4 0x0007
#endif
#ifndef PCI_DEVICE_ID_3DFX_VOODOO3_3000 /* Voodoo3 3000 */
#define PCI_DEVICE_ID_3DFX_VOODOO3_3000 0x0005
#endif
#ifndef PCI_DEVICE_ID_3DFX_VOODOO3_2000 /* Voodoo3 3000 */
#define PCI_DEVICE_ID_3DFX_VOODOO3_2000 0x0004
#endif
#ifndef PCI_DEVICE_ID_3DFX_BANSHEE
#define PCI_DEVICE_ID_3DFX_BANSHEE 0x0003
#endif

/* List acquired from http://www.yourvote.com/pci/pcihdr.h and xc/xc/programs/Xserver/hw/xfree86/common/xf86PciInfo.h
 * Please report to anholt@teleport.com inaccuracies or if a chip you have works that is marked unsupported here.
 */
drm_chipinfo_t DRM(devicelist)[] = {
	{0x121a, 0x0003, 1, "3dfx Voodoo Banshee"},
	{0x121a, 0x0004, 1, "3dfx Voodoo3 2000"},
	{0x121a, 0x0005, 1, "3dfx Voodoo3 3000"},
	{0x121a, 0x0007, 1, "3dfx Voodoo4"},
	{0x121a, 0x0009, 1, "3dfx Voodoo5"},
	{0, 0, 0, NULL}
};


#include "drm_auth.h"
#include "drm_bufs.h"
#include "drm_context.h"
#include "drm_dma.h"
#include "drm_drawable.h"
#include "drm_drv.h"


#include "drm_fops.h"
#include "drm_init.h"
#include "drm_ioctl.h"
#include "drm_lock.h"
#include "drm_memory.h"
#include "drm_vm.h"
#ifdef __AMIX__
#include "drm_amix.h"
#include "drm_sarea.h"
#include "tdfx_sarea.h"

#define TDFX_LFBMEMCFG		0x0c
#define TDFX_FIFO(r)		(0x80000 + (r))	/* command fifo 0 */
#define TDFX_FIFOSIZE		0x40000		/* 64 pages, as Glide expects */

static void
tdfx_wreg(int r, unsigned long v)
{
	DRM_WRITE32(DRM(card).c_reg + r, v);
}

/*
 * The VRAM layout and command fifo an X server's driver would set up:
 * the fifo at the bottom of the free VRAM, depth (odd page) and back
 * (even page) buffers tiled at the top, under the tiled aperture,
 * textures between.  Written into the shared area for the clients.
 */
static void
tdfx_amix_layout(drm_device_t *dev)
{
	struct drm_amixcard *c = &DRM(card);
	tdfx_sarea_t *sp;
	unsigned long tiles, buf, fifo, back, depth, tex;
	int id = (int)opci_attr(c->c_dev, OPCI_DEVICE);

	if (dev->lock.hw_lock == NULL)
		return;
	sp = (tdfx_sarea_t *)((char *)dev->lock.hw_lock + sizeof(drm_sarea_t));
	tiles = (c->c_width * 2 + 127) >> 7;
	buf = tiles * 128 * (((c->c_height + 31) >> 5) * 32);
	fifo = (c->c_free + 0xfff) & ~0xfffUL;
	tex = fifo + TDFX_FIFOSIZE;
	if (c->c_vram < tex + 2 * buf + 0x3000)
		return;
	depth = (c->c_vram - buf) & ~0xfffUL;
	if ((depth & 0x1000) == 0)
		depth -= 0x1000;
	back = (depth - buf) & ~0xfffUL;
	if (back & 0x1000)
		back -= 0x1000;
	if (back <= tex)
		return;
	DRIVER_IDLE(dev);
	tdfx_wreg(TDFX_FIFO(0x24), 0);			/* fifo off */
	tdfx_wreg(TDFX_FIFO(0x20), fifo >> 12);		/* base */
	tdfx_wreg(TDFX_FIFO(0x28), 0);			/* bump */
	tdfx_wreg(TDFX_FIFO(0x2c), fifo);		/* read pointer */
	tdfx_wreg(TDFX_FIFO(0x30), 0);
	tdfx_wreg(TDFX_FIFO(0x34), fifo - 4);		/* amin */
	tdfx_wreg(TDFX_FIFO(0x3c), fifo - 4);		/* amax */
	tdfx_wreg(TDFX_FIFO(0x44), 0);			/* depth */
	tdfx_wreg(TDFX_FIFO(0x48), 0);			/* holes */
	tdfx_wreg(TDFX_FIFO(0x80), id == 3 ? 0x9 << 5 | 0x2 : 0xf << 5 | 0x8);	/* fetch threshold */
	tdfx_wreg(TDFX_FIFO(0x24), (TDFX_FIFOSIZE >> 12) - 1 | 1 << 8);
	/* tile base at the back buffer, 4 KB aperture rows, the screen's tiles a row */
	tdfx_wreg(TDFX_LFBMEMCFG, back >> 12 | 2 << 13 | tiles << 16);
	sp->fifoPtr = sp->fifoRead = fifo;
	sp->fifoOwner = sp->ctxOwner = sp->texOwner = DRM_KERNEL_CONTEXT;
	sp->fifoOffset = fifo;
	sp->fifoSize = TDFX_FIFOSIZE;
	sp->backOffset = back;
	sp->depthOffset = depth;
	sp->textureOffset = tex;
	sp->textureSize = back - tex;
	sp->deviceID = id;
	DRM_SAREA_SYNC();
}

/*
 * Packets never cross the fifo's end, so zeros from the last word
 * written up to it complete any packet and then read as no-ops.
 */
static void
tdfx_amix_recover(drm_device_t *dev)
{
	struct drm_amixcard *c = &DRM(card);
	tdfx_sarea_t *sp;
	unsigned long a, end;

	if (dev->lock.hw_lock == NULL)
		return;
	sp = (tdfx_sarea_t *)((char *)dev->lock.hw_lock + sizeof(drm_sarea_t));
	end = sp->fifoOffset + sp->fifoSize;
	if (sp->fifoSize != 0 && end <= c->c_fbsize) {
		DRIVER_IDLE(dev);
		a = DRM_READ32(c->c_reg + TDFX_FIFO(0x2c)) +
		    4 * (DRM_READ32(c->c_reg + TDFX_FIFO(0x44)) & 0xffff);
		if (a >= sp->fifoOffset && a < end)
			for (; a < end; a += 4)
				*(volatile u32 *)(c->c_fb + a) = 0;
	}
	tdfx_amix_layout(dev);
}
#else
#include "drm_sysctl.h"
#endif

#ifdef __FreeBSD__
DRIVER_MODULE(tdfx, pci, tdfx_driver, tdfx_devclass, 0, 0);
#endif /* __FreeBSD__ */
