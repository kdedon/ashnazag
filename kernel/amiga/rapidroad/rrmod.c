/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rrmod.c - the RapidRoad as a loadable module: the X-Surf-100 found by
 * autoconfig, the ISP1763 behind its USB window, the controller
 * registered with the usb module (which enumerates the root hub and
 * whatever hangs off it, then and on every poll from /dev/usb).
 *
 * Loaded by hand or from an rc script (modadmin -l rapidroad); it holds
 * the usb module and stays loaded once the controller is registered.
 *
 * The X-Surf-100 shows the chip's registers twice, with a four byte
 * spacing: at 0x2800 with the bytes of each word swapped and at 0x3800
 * as they are.  Which of the two gives the words as the 68k wants them
 * is settled by reading the chip ID (0x0017 at register 0x72, with the
 * rest of the ID and the revision at 0x70).  The data port's longword
 * windows (0xa000-0xbffc) and the interrupt (0x4800, level 6) are not
 * used: all accesses are 16-bit, and the done maps are polled.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/errno.h"
#include "sys/moddefs.h"

#include "ub.h"
#include "usb.h"
#include "hub.h"
#include "usb_api.h"
#include "isp1763.h"

extern int autocon();
extern char *plat_iomap();
extern int printf();
extern long rr_submit_control_msg(), rr_submit_bulk_msg(), rr_submit_int_msg();

#define XSURF100	0x12120064	/* manufacturer 4626, product 100 */
#define RR_WIN_SWAPPED	0x2800
#define RR_WIN_PLAIN	0x3800
#define RR_CHIP_ID	0x0017		/* register 0x72 of an ISP1763 */

unsigned long rr_board;			/* the X-Surf-100's window, 0: none */
char *rr_base;				/* the chip's register window */
static struct usb_device *rr_root_hub;
static int rr_registered;

static long
rr_open(u)
struct ucdif *u;
{
	return 0;
}

static long
rr_close(u)
struct ucdif *u;
{
	return 0;
}

static long
rr_ioctl(struct ucdif *u, short cmd, long arg)
{
	switch (cmd) {
	case LOWLEVEL_INIT:
		return rr_lowlevel_init();
	case LOWLEVEL_STOP:
		rr_lowlevel_stop();
		return 0;
	case SUBMIT_CONTROL_MSG: {
		struct control_msg *m = (struct control_msg *)arg;

		return rr_submit_control_msg(m->dev, m->pipe, m->data, (long)m->size, m->setup);
	}
	case SUBMIT_BULK_MSG: {
		struct bulk_msg *m = (struct bulk_msg *)arg;

		return rr_submit_bulk_msg(m->dev, m->pipe, m->data, m->len);
	}
	case SUBMIT_INT_MSG: {
		struct int_msg *m = (struct int_msg *)arg;

		return rr_submit_int_msg(m->dev, m->pipe, m->buffer, m->transfer_len, m->interval);
	}
	}
	return -ENOSYS;
}

static struct ucdif rr_uif = {
	0,			/* next */
	USB_API_VERSION,
	USB_CONTRLL,
	"RapidRoad (ISP1763) on the X-Surf-100",
	"rapidroad",
	0,			/* unit */
	0,			/* flags */
	rr_open,
	rr_close,
	0,
	rr_ioctl,
	0,
	0
};

/* the chip ID from a window: 0 when it is the ISP1763's */
static int
rr_try_window(va, win)
char *va;
unsigned long win;
{
	rr_base = va + win;
	return RR_RD(ISP1763_HC_CHIP_ID) == RR_CHIP_ID ? 0 : -1;
}

int
rr_probe()
{
	unsigned long base, size, win = RR_WIN_PLAIN;
	char *va;
	unsigned rev;

	if (!autocon(XSURF100, 0, &base, &size)) {
		printf("rapidroad: no X-Surf-100\n");
		return -1;
	}
	if ((va = plat_iomap(base, size)) == (char *)0) {
		printf("rapidroad: X-Surf-100 at 0x%lx: cannot map it\n", base);
		return -1;
	}
	if (rr_try_window(va, RR_WIN_PLAIN) < 0) {
		win = RR_WIN_SWAPPED;
		if (rr_try_window(va, RR_WIN_SWAPPED) < 0) {
			printf("rapidroad: X-Surf-100 at 0x%lx: no ISP1763 (ID %04x at +0x%x, %04x at +0x%x)\n",
			    base, *(__volatile__ unsigned short *)(va + RR_WIN_PLAIN + (ISP1763_HC_CHIP_ID << 2)),
			    RR_WIN_PLAIN, *(__volatile__ unsigned short *)(va + RR_WIN_SWAPPED + (ISP1763_HC_CHIP_ID << 2)),
			    RR_WIN_SWAPPED);
			rr_base = (char *)0;
			return -1;
		}
	}
	rr_board = (unsigned long)va;
	rev = RR_RD(ISP1763_HC_CHIP_REV);
	printf("rapidroad: ISP17%02x rev %02x on the X-Surf-100 at 0x%lx, registers at +0x%lx\n",
	    rev >> 8, rev & 0xff, base, win);
	return 0;
}

static int
rr_load()
{
	if (rr_probe() < 0)
		return ENXIO;
	if (ucd_register(&rr_uif, &rr_root_hub) != 0) {
		printf("rapidroad: the USB stack would not take the controller\n");
		return EIO;
	}
	rr_registered = 1;
	return 0;
}

/* a host adapter: once its root hub is in the stack's tree it stays */
static int
rr_unload()
{
	return rr_registered ? EBUSY : 0;
}

MOD_HDRV_WRAPPER(rr, rr_load, rr_unload, 0, "RapidRoad USB (ISP1763)");
