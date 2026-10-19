/* radeon_drv.c -- ATI Radeon driver -*- linux-c -*-
 * Created: Wed Feb 14 17:10:04 2001 by gareth@valinux.com
 *
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
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Gareth Hughes <gareth@valinux.com>
 */

#ifndef __AMIX__
#include <sys/types.h>
#endif

#include "radeon.h"
#include "drmP.h"
#include "drm.h"
#include "radeon_drm.h"
#include "radeon_drv.h"
#if __REALLY_HAVE_SG
#include "ati_pcigart.h"
#endif

#ifdef __AMIX__
/* the X server asks for an idle card before it draws */
#define __HAVE_DMA_QUIESCENT	1
#define DRIVER_DMA_QUIESCENT()	radeon_amix_idle(dev)
#define DRIVER_IDLE(dev)	radeon_amix_idle(dev)
#define DRIVER_AMIX_MAPS(dev)	radeon_amix_init(dev)
static void radeon_amix_idle(drm_device_t *dev);
static void radeon_amix_init(drm_device_t *dev);
#define DRIVER_AMIX_KEEP(dev, lo, hi)	radeon_amix_keep(dev, lo, hi)
#define DRIVER_AMIX_RESTORE(dev)	radeon_amix_retex(dev)
#define DRIVER_AMIX_TEXTOP(dev)	radeon_amix_back
static void radeon_amix_keep(drm_device_t *dev, unsigned long *lo, unsigned long *hi);
static void radeon_amix_retex(drm_device_t *dev);
#endif

drm_chipinfo_t DRM(devicelist)[] = {
	{0x1002, 0x4242, 1, "ATI Radeon BB AIW 8500DV (AGP)"},
	{0x1002, 0x4336, 1, "ATI Radeon Mobility"},
	{0x1002, 0x4964, 1, "ATI Radeon Id 9000"},
	{0x1002, 0x4965, 1, "ATI Radeon Ie 9000"},
	{0x1002, 0x4966, 1, "ATI Radeon If 9000"},
	{0x1002, 0x4967, 1, "ATI Radeon Ig 9000"},
	{0x1002, 0x496e, 1, "ATI Radeon Ig 9000"},
	{0x1002, 0x4C57, 1, "ATI Radeon LW Mobility 7 (AGP)"},
	{0x1002, 0x4C58, 1, "ATI Radeon LX Mobility 7 (AGP)"},
	{0x1002, 0x4C59, 1, "ATI Radeon LY Mobility 6 (AGP)"},
	{0x1002, 0x4C5A, 1, "ATI Radeon LZ Mobility 6 (AGP)"},
	{0x1002, 0x4C64, 1, "ATI Radeon Ld Mobility 9000 (AGP)"},
	{0x1002, 0x4C65, 1, "ATI Radeon Le Mobility 9000 (AGP)"},
	{0x1002, 0x4C66, 1, "ATI Radeon Lf Mobility 9000 (AGP)"},
	{0x1002, 0x4C67, 1, "ATI Radeon Lg Mobility 9000 (AGP)"},
	{0x1002, 0x5144, 1, "ATI Radeon QD R100 (AGP)"},
	{0x1002, 0x5145, 1, "ATI Radeon QE R100 (AGP)"},
	{0x1002, 0x5146, 1, "ATI Radeon QF R100 (AGP)"},
	{0x1002, 0x5147, 1, "ATI Radeon QG R100 (AGP)"},
	{0x1002, 0x5148, 1, "ATI Radeon QH FireGL 8x00 (AGP)"},
	{0x1002, 0x5149, 1, "ATI Radeon QI R200"},
	{0x1002, 0x514A, 1, "ATI Radeon QJ R200"},
	{0x1002, 0x514B, 1, "ATI Radeon QK R200"},
	{0x1002, 0x514C, 1, "ATI Radeon QL 8500 (AGP)"},
	{0x1002, 0x5157, 1, "ATI Radeon QW 7500 (AGP)"},
	{0x1002, 0x5158, 1, "ATI Radeon QX 7500 (AGP)"},
	{0x1002, 0x5159, 1, "ATI Radeon QY VE (AGP)"},
	{0x1002, 0x515A, 1, "ATI Radeon QZ VE (AGP)"},
	{0x1002, 0x5168, 1, "ATI Radeon Qh R200"},
	{0x1002, 0x5169, 1, "ATI Radeon Qi R200"},
	{0x1002, 0x516A, 1, "ATI Radeon Qj R200"},
	{0x1002, 0x516B, 1, "ATI Radeon Qk R200"},
	{0x1002, 0x5960, 1, "ATI Radeon 9250 RV280"},
	{0x1002, 0x5961, 1, "ATI Radeon 9200 RV280"},
	{0x1002, 0x5962, 1, "ATI Radeon 9200 RV280"},
	{0x1002, 0x5964, 1, "ATI Radeon 9200 SE RV280"},
	{0x1002, 0x5965, 1, "ATI FireMV 2200 RV280"},
	{0, 0, 0, NULL}
};

#ifndef __AMIX__
#include "drm_agpsupport.h"
#endif
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
#ifndef __AMIX__
#include "drm_sysctl.h"
#endif
#if __HAVE_SG
#include "drm_scatter.h"
#endif

#ifdef __AMIX__
static unsigned long radeon_amix_back, radeon_amix_depth;
#include "drm_amix.h"
#include "drm_sarea.h"
#include "radeon_amix.h"

#define RADEON_AMIX_RING	0x40000		/* 256 KB */
#define RADEON_AMIX_BUFSIZE	0x10000
#define RADEON_AMIX_NBUFS	16

extern int radeon_amix_start(drm_device_t *dev);
extern int radeon_pcigart;
extern unsigned long rd_aper, rd_fbsize;

/* the shown frame, from the display backend; 0 bits: none yet */
static unsigned long radeon_amix_shown, radeon_amix_pitch;
static int radeon_amix_bits;

static int
radeon_amix_family(int id)
{
	switch (id) {
	case 0x4242: case 0x4964: case 0x4965: case 0x4966: case 0x4967:
	case 0x496e: case 0x4c64: case 0x4c65: case 0x4c66: case 0x4c67:
	case 0x5148: case 0x5149: case 0x514a: case 0x514b: case 0x514c:
	case 0x5168: case 0x5169: case 0x516a: case 0x516b:
	case 0x5960: case 0x5961: case 0x5962: case 0x5964: case 0x5965:
		return RADEON_FAMILY_R200;
	}
	return RADEON_FAMILY_R100;
}

static void
radeon_amix_idle(drm_device_t *dev)
{
	drm_radeon_private_t *dev_priv = dev->dev_private;

	if (dev_priv && dev_priv->cp_running)
		(void)radeon_do_cp_idle(dev_priv);
}

/*
 * Front, back and depth buffers for the shown frame: what the X
 * server's CP init gave, but following the display, whose depth and
 * frame change with the front session.  Back and depth take the
 * front's pitch and pixel size.
 */
static void
radeon_amix_buffers(drm_radeon_private_t *dev_priv)
{
	int cpp = radeon_amix_bits / 8;

	dev_priv->color_fmt = cpp == 2 ? RADEON_COLOR_FORMAT_RGB565 : RADEON_COLOR_FORMAT_ARGB8888;
	dev_priv->depth_fmt = cpp == 2 ? RADEON_DEPTH_FORMAT_16BIT_INT_Z : RADEON_DEPTH_FORMAT_24BIT_INT_Z;
	dev_priv->front_offset = radeon_amix_shown;
	dev_priv->front_pitch = radeon_amix_pitch;
	dev_priv->back_offset = radeon_amix_back;
	dev_priv->back_pitch = radeon_amix_pitch;
	dev_priv->depth_offset = radeon_amix_depth;
	dev_priv->depth_pitch = radeon_amix_pitch;
	dev_priv->front_pitch_offset = (dev_priv->front_pitch / 64) << 22 | dev_priv->front_offset >> 10;
	dev_priv->back_pitch_offset = (dev_priv->back_pitch / 64) << 22 | dev_priv->back_offset >> 10;
	dev_priv->depth_pitch_offset = (dev_priv->depth_pitch / 64) << 22 | dev_priv->depth_offset >> 10;
	dev_priv->depth_clear.rb3d_cntl = RADEON_PLANE_MASK_ENABLE | dev_priv->color_fmt << 10 | 1 << 15;
	dev_priv->depth_clear.rb3d_zstencilcntl = (dev_priv->depth_clear.rb3d_zstencilcntl &
	    ~0xfUL) | dev_priv->depth_fmt;
}

/* the display shows another frame or depth: called by its backend */
void
radeon_amix_retarget(unsigned long shown, int bits, unsigned long pitch)
{
	drm_device_t *dev = &DRM(softc)[0];

	radeon_amix_shown = shown;
	radeon_amix_bits = bits;
	radeon_amix_pitch = pitch;
	if (dev->dev_private && bits >= 16)
		radeon_amix_buffers(dev->dev_private);
	DRM(amix_front)();
}

/* back and depth, then the CP's ring, status and buffers when in VRAM */
static void
radeon_amix_keep(drm_device_t *dev, unsigned long *lo, unsigned long *hi)
{
	radeon_amix_layout_t *lp;

	if (dev->lock.hw_lock == NULL)
		return;
	lp = (radeon_amix_layout_t *)((char *)dev->lock.hw_lock + RADEON_AMIX_LAYOUT);
	if (lp->backSize == 0)
		return;
	*lo = lp->backOffset;
	*hi = DRM(card).c_vram;
}

/* textures from the free VRAM up to the back buffer, at most a region each granule */
static unsigned long
radeon_amix_texsize(unsigned long tex, int l)
{
	unsigned long n = (radeon_amix_back - tex) >> l;

	return (n < RADEON_NR_TEX_REGIONS ? n : RADEON_NR_TEX_REGIONS) << l;
}

/* textures where the free VRAM now starts, every region newer than any client's view */
static void
radeon_amix_retex(drm_device_t *dev)
{
	radeon_amix_layout_t *lp;
	drm_radeon_sarea_t *sp;
	int h, i, age;

	if (dev->lock.hw_lock == NULL)
		return;
	lp = (radeon_amix_layout_t *)((char *)dev->lock.hw_lock + RADEON_AMIX_LAYOUT);
	if (lp->backSize != 0) {
		lp->textureOffset = (DRM(card).c_free + 0xffff) & ~0xffffUL;
		lp->textureSize = radeon_amix_texsize(lp->textureOffset, lp->log2TexGran);
	}
	sp = (drm_radeon_sarea_t *)((char *)dev->lock.hw_lock + sizeof(drm_sarea_t));
	for (h = 0; h < RADEON_NR_TEX_HEAPS; h++) {
		age = ++sp->tex_age[h];
		for (i = 0; i < RADEON_NR_TEX_REGIONS; i++)
			sp->tex_list[h][i].age = age;
	}
	DRM_SAREA_SYNC();
}

static int
radeon_amix_sgmap(dev_t kdev, unsigned long off, unsigned long size, unsigned long *where)
{
	drm_map_t m;
	int e;

	bzero(&m, sizeof m);
	m.offset = off;
	m.size = size;
	m.type = _DRM_SCATTER_GATHER;
	if ((e = DRM(addmap)(kdev, DRM_IOCTL_ADD_MAP, (caddr_t)&m, FREAD | FWRITE, DRM_CURPROC)) == 0)
		*where = m.offset;
	return e;
}

/*
 * VRAM at off as the CP's memory, reached through the second aperture,
 * which never swaps bytes: CPU stores land as they would in host memory.
 */
static int
radeon_amix_vram(drm_device_t *dev, unsigned long off, unsigned long size)
{
	struct drm_amixcard *c = &DRM(card);
	drm_sg_mem_t *e;
	unsigned long i, pages = round_page(size) >> PAGE_SHIFT;

	if (rd_fbsize < 2 * rd_aper || dev->sg)
		return ENXIO;
	if ((e = DRM(alloc)(sizeof *e, DRM_MEM_SGLISTS)) == NULL)
		return ENOMEM;
	bzero(e, sizeof *e);
	e->pages = pages;
	e->pagelist = DRM(alloc)(pages * sizeof *e->pagelist, DRM_MEM_PAGES);
	e->busaddr = DRM(alloc)(pages * sizeof *e->busaddr, DRM_MEM_PAGES);
	if (e->pagelist == NULL || e->busaddr == NULL) {
		if (e->pagelist)
			DRM(free)(e->pagelist, pages * sizeof *e->pagelist, DRM_MEM_PAGES);
		if (e->busaddr)
			DRM(free)(e->busaddr, pages * sizeof *e->busaddr, DRM_MEM_PAGES);
		DRM(free)(e, sizeof *e, DRM_MEM_SGLISTS);
		return ENOMEM;
	}
	bzero(e->pagelist, pages * sizeof *e->pagelist);
	/* VRAM sits at card address 0 */
	for (i = 0; i < pages; i++)
		e->busaddr[i] = off + (i << PAGE_SHIFT);
	e->virtual = (void *)(c->c_fb + rd_aper + off);
	e->handle = (unsigned long)e->virtual;
	e->vram = c->c_physfb + rd_aper + off;
	bzero(e->virtual, pages << PAGE_SHIFT);
	DRM_DMA_SYNC();
	dev->sg = e;
	return 0;
}

/*
 * What the X server and its driver did: memory for the ring,
 * its read pointer and the DMA buffers (VRAM, or PCI GART memory when
 * radeon_pcigart is set) at the top; the CP started on them; back
 * and depth below them, textures in the VRAM the sessions leave;
 * the interrupt.  The layout goes to clients in the shared area.  On
 * any failure clients find backSize 0 and go without 3D.
 */
static void
radeon_amix_init(drm_device_t *dev)
{
	struct drm_amixcard *c = &DRM(card);
	drm_radeon_init_t init;
	drm_scatter_gather_t sg;
	drm_buf_desc_t bd;
	radeon_amix_layout_t *lp;
	drm_map_list_entry_t *le;
	unsigned long ring, rptr, bufs, mmio = 0, status = 0, buf, tex, cp, top = c->c_vram & ~0xffffUL;
	int id = (int)opci_attr(c->c_dev, OPCI_DEVICE), fam = radeon_amix_family(id);
	/* an 8-bit display has no 3D clients; the buffers follow its next depth */
	int bits = radeon_amix_bits < 16 ? 16 : radeon_amix_bits, l, e;
	unsigned long pitch = (unsigned long)c->c_width * (bits / 8);
	dev_t kdev = 0;

	if (dev->lock.hw_lock == NULL)
		return;
	lp = (radeon_amix_layout_t *)((char *)dev->lock.hw_lock + RADEON_AMIX_LAYOUT);
	bzero(lp, sizeof *lp);
	lp->deviceID = id;
	lp->chipFamily = fam;
	lp->width = c->c_width;
	lp->height = c->c_height;
	lp->sarea_priv_offset = sizeof(drm_sarea_t);
	TAILQ_FOREACH(le, dev->maplist, link)
		if (le->map->type == _DRM_REGISTERS) {
			mmio = le->map->offset;
			lp->registerHandle = mmio;
			lp->registerSize = le->map->size;
		}
	/* back and depth for 32 bits a pixel at a pitch of 64 bytes, textures below */
	buf = (((unsigned long)c->c_width * 4 + 63) & ~63UL) * c->c_height;
	cp = top;
	if (!radeon_pcigart)
		cp = (top - (RADEON_AMIX_RING + PAGE_SIZE + RADEON_AMIX_NBUFS * RADEON_AMIX_BUFSIZE)) &
		    ~0xffffUL;
	radeon_amix_depth = (cp - buf) & ~0xffffUL;
	radeon_amix_back = (radeon_amix_depth - buf) & ~0xffffUL;
	tex = (c->c_free + 0xffff) & ~0xffffUL;
	if (cp < 2 * buf + 0x20000 || tex + (1 << RADEON_LOG_TEX_GRANULARITY) > radeon_amix_back) {
		DRM_ERROR("no VRAM left for 3D\n");
		radeon_amix_back = 0;
		goto out;
	}
	for (l = 0; (radeon_amix_back - tex) / RADEON_NR_TEX_REGIONS >> (l + 1); l++)
		;
	if (l < RADEON_LOG_TEX_GRANULARITY)
		l = RADEON_LOG_TEX_GRANULARITY;

	ring = 0;
	rptr = RADEON_AMIX_RING;
	bufs = rptr + PAGE_SIZE;
	sg.size = bufs + RADEON_AMIX_NBUFS * RADEON_AMIX_BUFSIZE;
	sg.handle = 0;
	if ((e = radeon_pcigart ? DRM(sg_alloc)(kdev, DRM_IOCTL_SG_ALLOC, (caddr_t)&sg, FREAD | FWRITE,
	    DRM_CURPROC) : radeon_amix_vram(dev, cp, sg.size)) != 0 ||
	    (e = radeon_amix_sgmap(kdev, ring, RADEON_AMIX_RING, &ring)) != 0 ||
	    (e = radeon_amix_sgmap(kdev, rptr, PAGE_SIZE, &status)) != 0 ||
	    (e = radeon_amix_sgmap(kdev, bufs, RADEON_AMIX_NBUFS * RADEON_AMIX_BUFSIZE, &bufs)) != 0) {
		DRM_ERROR("no %s for the CP (%d)\n", radeon_pcigart ? "PCI GART memory" : "second aperture", e);
		goto out;
	}
	bzero(&bd, sizeof bd);
	bd.count = RADEON_AMIX_NBUFS;
	bd.size = RADEON_AMIX_BUFSIZE;
	bd.flags = _DRM_SG_BUFFER;
	bd.agp_start = rptr + PAGE_SIZE;
	if ((e = DRM(addbufs)(kdev, DRM_IOCTL_ADD_BUFS, (caddr_t)&bd, FREAD | FWRITE, DRM_CURPROC)) != 0) {
		DRM_ERROR("no DMA buffers (%d)\n", e);
		goto out;
	}

	bzero(&init, sizeof init);
	init.func = fam == RADEON_FAMILY_R200 ? RADEON_INIT_R200_CP : RADEON_INIT_CP;
	init.sarea_priv_offset = sizeof(drm_sarea_t);
	init.is_pci = 1;
	init.cp_mode = RADEON_CSQ_PRIBM_INDBM;
	init.agp_size = sg.size;
	init.ring_size = RADEON_AMIX_RING;
	init.usec_timeout = 100000;
	init.fb_bpp = init.depth_bpp = bits;
	init.front_offset = radeon_amix_shown;
	init.front_pitch = init.back_pitch = init.depth_pitch = pitch;
	init.back_offset = radeon_amix_back;
	init.depth_offset = radeon_amix_depth;
	init.fb_offset = c->c_physfb;
	init.mmio_offset = mmio;
	init.ring_offset = ring;
	init.ring_rptr_offset = status;
	init.buffers_offset = bufs;
	if ((e = radeon_cp_init(kdev, DRM_IOCTL_RADEON_CP_INIT, (caddr_t)&init, FREAD | FWRITE,
	    DRM_CURPROC)) != 0 || (e = radeon_amix_start(dev)) != 0) {
		DRM_ERROR("CP init failed (%d)\n", e);
		goto out;
	}
	e = (int)opci_attr(c->c_dev, OPCI_INTLINE);
	if (DRM(irq_install)(dev, e ? e : 1) != 0)
		DRM_INFO("no interrupt: clients poll\n");

	lp->backOffset = radeon_amix_back;
	lp->backSize = buf;
	lp->depthOffset = radeon_amix_depth;
	lp->depthSize = buf;
	lp->textureOffset = tex;
	lp->textureSize = radeon_amix_texsize(tex, l);
	lp->log2TexGran = l;
	lp->statusHandle = status;
	lp->statusSize = PAGE_SIZE;
	lp->irq = dev->irq;
	lp->cpInVram = !radeon_pcigart;
	DRM_INFO("%s, CP %s, back 0x%x depth 0x%x textures 0x%x+%dK%s\n",
	    fam == RADEON_FAMILY_R200 ? "R200" : "R100", radeon_pcigart ? "on PCI GART" : "in VRAM",
	    (int)radeon_amix_back,
	    (int)radeon_amix_depth, (int)tex, (int)(lp->textureSize >> 10),
	    dev->irq ? ", interrupt" : "");
out:
	DRM_SAREA_SYNC();
}
#else
DRIVER_MODULE(DRIVER_NAME, pci, DRM(driver), DRM(devclass), 0, 0);
#endif
