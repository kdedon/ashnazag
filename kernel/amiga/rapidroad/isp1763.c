/* SPDX-License-Identifier: GPL-2.0 */
/*
 * isp1763.c - the ISP1763 host controller driver for the USB stack
 * (kernel/usb): transfers through the chip's PTDs, the root hub.
 *
 * From U-Boot's isp1760 driver (drivers/usb/isp1760/isp1760-hcd.c,
 * isp1760-core.c and the URB code of drivers/usb/common/usb_urb.c),
 * itself the Linux driver:
 *
 * Copyright 2021 Linaro, Rui Miguel Silva <rui.silva@linaro.org>
 * Copyright 2014 Laurent Pinchart
 * Copyright 2007 Sebastian Siewior
 *
 * Kept: the PTD construction, the chip memory allocator, the done-map
 * handling, the root hub's port control, the ISP1763 setup sequence.
 * Changed for AMIX: the ISP1760/1761 paths are gone (the RapidRoad is
 * always an ISP1763 on a 16-bit bus), registers are read and written
 * directly (isp1763.h) instead of through regmap fields, the chip's
 * memory goes through the HC_DATA port a word at a time with the bytes
 * put in USB order by hand (the 68k is big-endian), the slab caches are
 * static pools, and nothing runs from an interrupt: a transfer is
 * submitted and then the done maps are polled from the caller's process
 * context, as U-Boot's usb_urb_submit does, sleeping between polls.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/systm.h"
#include "sys/errno.h"

#include "ub.h"
#include "usb.h"
#include "hub.h"
#include "usb_api.h"
#include "isp1763.h"
#include "rrlist.h"

extern clock_t lbolt;
extern clock_t drv_usectohz();

/* ---- registers --------------------------------------------------------- */

#define F(reg, lo, hi)	{ reg, lo, (u16)((((1UL << ((hi) - (lo) + 1)) - 1) << (lo)) & 0xffff) }

/* in the order of enum rr_field */
static const struct rr_fielddef rr_fields[HC_FIELD_MAX] = {
	F(ISP1763_HC_PORTSC1, 13, 13),		/* PORT_OWNER */
	F(ISP1763_HC_PORTSC1, 12, 12),		/* PORT_POWER */
	F(ISP1763_HC_PORTSC1, 10, 11),		/* PORT_LSTATUS */
	F(ISP1763_HC_PORTSC1, 8, 8),		/* PORT_RESET */
	F(ISP1763_HC_PORTSC1, 7, 7),		/* PORT_SUSPEND */
	F(ISP1763_HC_PORTSC1, 6, 6),		/* PORT_RESUME */
	F(ISP1763_HC_PORTSC1, 2, 2),		/* PORT_PE */
	F(ISP1763_HC_PORTSC1, 1, 1),		/* PORT_CSC */
	F(ISP1763_HC_PORTSC1, 0, 0),		/* PORT_CONNECT */
	F(ISP1763_HC_USBCMD, 7, 7),		/* CMD_LRESET */
	F(ISP1763_HC_USBCMD, 1, 1),		/* CMD_RESET */
	F(ISP1763_HC_USBCMD, 0, 0),		/* CMD_RUN */
	F(ISP1763_HC_USBSTS, 2, 2),		/* STS_PCD */
	F(ISP1763_HC_FRINDEX, 0, 13),		/* HC_FRINDEX */
	F(ISP1763_HC_CONFIGFLAG, 0, 0),		/* FLAG_CF */
	F(ISP1763_HC_ISO_PTD_DONEMAP, 0, 15),
	F(ISP1763_HC_ISO_PTD_SKIPMAP, 0, 15),
	F(ISP1763_HC_ISO_PTD_LASTPTD, 0, 15),
	F(ISP1763_HC_INT_PTD_DONEMAP, 0, 15),
	F(ISP1763_HC_INT_PTD_SKIPMAP, 0, 15),
	F(ISP1763_HC_INT_PTD_LASTPTD, 0, 15),
	F(ISP1763_HC_ATL_PTD_DONEMAP, 0, 15),
	F(ISP1763_HC_ATL_PTD_SKIPMAP, 0, 15),
	F(ISP1763_HC_ATL_PTD_LASTPTD, 0, 15),
	F(ISP1763_HC_HW_MODE_CTRL, 4, 4),	/* HW_DATA_BUS_WIDTH */
	F(ISP1763_HC_HW_MODE_CTRL, 6, 6),	/* HW_DACK_POL_HIGH */
	F(ISP1763_HC_HW_MODE_CTRL, 5, 5),	/* HW_DREQ_POL_HIGH */
	F(ISP1763_HC_HW_MODE_CTRL, 3, 3),	/* HW_INTF_LOCK */
	F(ISP1763_HC_HW_MODE_CTRL, 2, 2),	/* HW_INTR_HIGH_ACT */
	F(ISP1763_HC_HW_MODE_CTRL, 1, 1),	/* HW_INTR_EDGE_TRIG */
	F(ISP1763_HC_HW_MODE_CTRL, 0, 0),	/* HW_GLOBAL_INTR_EN */
	F(ISP1763_HC_CHIP_ID, 0, 15),		/* HC_CHIP_ID_HIGH */
	F(ISP1763_HC_CHIP_REV, 8, 15),		/* HC_CHIP_ID_LOW */
	F(ISP1763_HC_CHIP_REV, 0, 7),		/* HC_CHIP_REV */
	F(ISP1763_HC_SCRATCH, 0, 15),		/* HC_SCRATCH */
	F(ISP1763_HC_RESET, 3, 3),		/* SW_RESET_RESET_ATX */
	F(ISP1763_HC_RESET, 0, 0),		/* SW_RESET_RESET_ALL */
	F(ISP1763_HC_BUFFER_STATUS, 2, 2),	/* ISO_BUF_FILL */
	F(ISP1763_HC_BUFFER_STATUS, 1, 1),	/* INT_BUF_FILL */
	F(ISP1763_HC_BUFFER_STATUS, 0, 0),	/* ATL_BUF_FILL */
	F(ISP1763_HC_MEMORY, 0, 15),		/* MEM_START_ADDR */
	F(ISP1763_HC_INTERRUPT, 0, 10),		/* HC_INTERRUPT */
	F(ISP1763_HC_INTERRUPT_ENABLE, 7, 7),	/* HC_INT_IRQ_ENABLE */
	F(ISP1763_HC_INTERRUPT_ENABLE, 8, 8),	/* HC_ATL_IRQ_ENABLE */
	F(ISP1763_HC_ISO_IRQ_MASK_OR, 0, 15),
	F(ISP1763_HC_INT_IRQ_MASK_OR, 0, 15),
	F(ISP1763_HC_ATL_IRQ_MASK_OR, 0, 15),
	F(ISP1763_HC_ISO_IRQ_MASK_AND, 0, 15),
	F(ISP1763_HC_INT_IRQ_MASK_AND, 0, 15),
	F(ISP1763_HC_ATL_IRQ_MASK_AND, 0, 15),
	F(ISP1763_HC_OTG_CTRL_SET, 15, 15),	/* HW_HC_2_DIS */
	F(ISP1763_HC_OTG_CTRL_SET, 10, 10),	/* HW_OTG_DISABLE */
	F(ISP1763_HC_OTG_CTRL_SET, 7, 7),	/* HW_SW_SEL_HC_DC */
	F(ISP1763_HC_OTG_CTRL_SET, 4, 4),	/* HW_VBUS_DRV */
	F(ISP1763_HC_OTG_CTRL_SET, 3, 3),	/* HW_SEL_CP_EXT */
	F(ISP1763_HC_OTG_CTRL_SET, 2, 2),	/* HW_DM_PULLDOWN */
	F(ISP1763_HC_OTG_CTRL_SET, 1, 1),	/* HW_DP_PULLDOWN */
	F(ISP1763_HC_OTG_CTRL_SET, 0, 0),	/* HW_DP_PULLUP */
	F(ISP1763_HC_OTG_CTRL_CLEAR, 15, 15),	/* HW_HC_2_DIS_CLEAR */
	F(ISP1763_HC_OTG_CTRL_CLEAR, 10, 10),	/* HW_OTG_DISABLE_CLEAR */
	F(ISP1763_HC_OTG_CTRL_CLEAR, 7, 7),	/* HW_SW_SEL_HC_DC_CLEAR */
	F(ISP1763_HC_OTG_CTRL_CLEAR, 4, 4),	/* HW_VBUS_DRV_CLEAR */
	F(ISP1763_HC_OTG_CTRL_CLEAR, 3, 3),	/* HW_SEL_CP_EXT_CLEAR */
	F(ISP1763_HC_OTG_CTRL_CLEAR, 2, 2),	/* HW_DM_PULLDOWN_CLEAR */
	F(ISP1763_HC_OTG_CTRL_CLEAR, 1, 1),	/* HW_DP_PULLDOWN_CLEAR */
	F(ISP1763_HC_OTG_CTRL_CLEAR, 0, 0),	/* HW_DP_PULLUP_CLEAR */
};

static u32
rr_read(f)
int f;
{
	const struct rr_fielddef *d = &rr_fields[f];

	return (RR_RD(d->reg) & d->mask) >> d->shift;
}

/*
 * The OTG control registers set (0xe4) or clear (0xe6) the bits written
 * as ones and show the state when read, so they take the bits alone;
 * every other field is read, modified and written back.
 */
static void
rr_write(f, val)
int f;
u32 val;
{
	const struct rr_fielddef *d = &rr_fields[f];
	u16 bits = (val << d->shift) & d->mask;

	if (d->reg == ISP1763_HC_OTG_CTRL_SET || d->reg == ISP1763_HC_OTG_CTRL_CLEAR) {
		if (bits)
			RR_WR(d->reg, bits);
		return;
	}
	RR_WR(d->reg, (RR_RD(d->reg) & ~d->mask) | bits);
}

#define rr_set(f)	rr_write(f, 0xffffffffUL)
#define rr_clear(f)	rr_write(f, 0)
#define rr_is_set(f)	(rr_read(f) != 0)

/* busy-wait for a field to become nonzero (want 1) or zero (want 0) */
static int
rr_wait(f, want, timeout_us)
int f, want;
u32 timeout_us;
{
	u32 t = 0;

	for (;;) {
		if ((rr_read(f) != 0) == want)
			return 0;
		if (t >= timeout_us)
			return -ETIME;
		drv_usecwait(10);
		t += 10;
	}
}

#define rr_set_and_wait(f, us)		(rr_set(f), rr_wait(f, 1, us))
#define rr_set_and_wait_swap(f, us)	(rr_set(f), rr_wait(f, 0, us))
#define rr_clear_and_wait(f, us)	(rr_clear(f), rr_wait(f, 0, us))

/* ---- chip memory ------------------------------------------------------- */

/*
 * The chip's memory is reached through HC_MEMORY (the address) and
 * HC_DATA (the words, the address advancing).  Its bytes are in USB
 * order: the first byte of a word is its low half.
 */
static void
rr_mem_read(addr, dst, bytes)
u16 addr;
u8 *dst;
u32 bytes;
{
	u16 w;

	RR_WR(ISP1763_HC_MEMORY, addr);
	drv_usecwait(1);
	while (bytes >= 2) {
		w = RR_RD(ISP1763_HC_DATA);
		*dst++ = w & 0xff;
		*dst++ = w >> 8;
		bytes -= 2;
	}
	if (bytes) {
		w = RR_RD(ISP1763_HC_DATA);
		*dst = w & 0xff;
	}
}

static void
rr_mem_write(addr, src, bytes)
u16 addr;
u8 *src;
u32 bytes;
{
	RR_WR(ISP1763_HC_MEMORY, addr);
	drv_usecwait(1);
	while (bytes >= 2) {
		RR_WR(ISP1763_HC_DATA, src[0] | (src[1] << 8));
		src += 2;
		bytes -= 2;
	}
	if (bytes)
		RR_WR(ISP1763_HC_DATA, src[0]);
}

/* Philips transfer descriptor: eight little-endian double words */
struct ptd {
	u32 dw0, dw1, dw2, dw3, dw4, dw5, dw6, dw7;
};

static void
rr_ptd_read(ptd_offset, slot, ptd)
u32 ptd_offset;
int slot;
struct ptd *ptd;
{
	u32 *d = (u32 *)ptd, lo, hi;
	int i;

	RR_WR(ISP1763_HC_MEMORY, ptd_offset + slot * sizeof(*ptd));
	drv_usecwait(1);
	for (i = 0; i < 8; i++) {
		lo = RR_RD(ISP1763_HC_DATA);
		hi = RR_RD(ISP1763_HC_DATA);
		d[i] = lo | (hi << 16);
	}
}

static void
rr_ptd_write(ptd_offset, slot, ptd)
u32 ptd_offset;
int slot;
struct ptd *ptd;
{
	u32 *d = (u32 *)ptd;
	int i;

	RR_WR(ISP1763_HC_MEMORY, ptd_offset + slot * sizeof(*ptd));
	drv_usecwait(1);
	for (i = 0; i < 8; i++) {
		RR_WR(ISP1763_HC_DATA, d[i] & 0xffff);
		RR_WR(ISP1763_HC_DATA, d[i] >> 16);
	}
}

/* ---- the transfer descriptors ------------------------------------------ */

/* DW0 */
#define DW0_VALID_BIT			1UL
#define TO_DW0_LENGTH(x)		((u32)(x) << 3)
#define TO_DW0_MAXPACKET(x)		((u32)(x) << 18)
#define TO_DW0_MULTI(x)			((u32)(x) << 29)
#define TO_DW0_ENDPOINT(x)		((u32)(x) << 31)
/* DW1 */
#define TO_DW1_DEVICE_ADDR(x)		((u32)(x) << 3)
#define TO_DW1_PID_TOKEN(x)		((u32)(x) << 10)
#define DW1_TRANS_BULK			(2UL << 12)
#define DW1_TRANS_INT			(3UL << 12)
#define DW1_TRANS_SPLIT			(1UL << 14)
#define DW1_SE_USB_LOSPEED		(2UL << 16)
#define TO_DW1_PORT_NUM(x)		((u32)(x) << 18)
#define TO_DW1_HUB_NUM(x)		((u32)(x) << 25)
/* DW2 */
#define TO_DW2_DATA_START_ADDR(x)	((u32)(x) << 8)
#define TO_DW2_RL(x)			((u32)(x) << 25)
#define FROM_DW2_RL(x)			(((x) >> 25) & 0xf)
/* DW3 */
#define FROM_DW3_NRBYTESTRANSFERRED(x)		((x) & 0x3fff)
#define FROM_DW3_SCS_NRBYTESTRANSFERRED(x)	((x) & 0x07ff)
#define TO_DW3_NAKCOUNT(x)		((u32)(x) << 19)
#define FROM_DW3_NAKCOUNT(x)		(((x) >> 19) & 0xf)
#define TO_DW3_CERR(x)			((u32)(x) << 23)
#define FROM_DW3_CERR(x)		(((x) >> 23) & 0x3)
#define TO_DW3_DATA_TOGGLE(x)		((u32)(x) << 25)
#define FROM_DW3_DATA_TOGGLE(x)		(((x) >> 25) & 0x1)
#define TO_DW3_PING(x)			((u32)(x) << 26)
#define FROM_DW3_PING(x)		(((x) >> 26) & 0x1)
#define DW3_ERROR_BIT			(1UL << 28)
#define DW3_BABBLE_BIT			(1UL << 29)
#define DW3_HALT_BIT			(1UL << 30)
#define DW3_ACTIVE_BIT			(1UL << 31)

#define INT_UNDERRUN			4
#define INT_BABBLE			2
#define INT_EXACT			1

#define SETUP_PID	2
#define IN_PID		1
#define OUT_PID		0

/* Errata 1 */
#define RL_COUNTER	0
#define NAK_COUNTER	0
#define ERR_COUNTER	3

struct rr_urb;

struct rr_qtd {
	u8	packet_type;
	u8	*data_buffer;
	u32	payload_addr;
	struct list_head qtd_list;
	struct rr_urb *urb;
	u32	length;
	u32	actual_length;
	/*
	 * QTD_ENQUEUED:	waiting for transfer (inactive)
	 * QTD_PAYLOAD_ALLOC:	chip mem has been allocated for payload
	 * QTD_XFER_STARTED:	valid ptd has been written to the chip
	 * QTD_XFER_COMPLETE:	payload has been transferred successfully
	 * QTD_RETIRE:		transfer error/abort qtd
	 */
#define QTD_ENQUEUED		0
#define QTD_PAYLOAD_ALLOC	1
#define QTD_XFER_STARTED	2
#define QTD_XFER_COMPLETE	3
#define QTD_RETIRE		4
	int	status;
	char	used;
};

/* queue head, one for each active endpoint */
struct rr_qh {
	struct list_head qh_list;
	struct list_head qtd_list;
	int	epnum;
	u32	toggle;
	u32	ping;
	int	slot;
	char	used;
};

struct rr_urb_item {
	struct list_head urb_list;
	struct rr_urb *urb;
	char	used;
};

/* a transfer: what usb_urb_fill set up in U-Boot */
struct rr_urb {
	struct usb_device *dev;
	unsigned long pipe;
	int	type;			/* PIPE_CONTROL, PIPE_BULK, PIPE_INTERRUPT */
	int	status;			/* -EINPROGRESS while running, else 0 or -error */
	u8	*transfer_buffer;
	u32	transfer_buffer_length;
	u32	actual_length;
	u8	*setup_packet;
	int	interval;		/* bInterval */
	struct rr_qh *qh;
};

struct rr_slotinfo {
	struct rr_qh *qh;
	struct rr_qtd *qtd;
};

struct rr_chunk {
	unsigned start, size, free;
};

enum rr_qh_type { QH_CONTROL, QH_BULK, QH_INTERRUPT, QH_END };

static struct rr_slotinfo atl_slots[RR_SLOTS], int_slots[RR_SLOTS];
static u32 atl_done_map, int_done_map;
static struct rr_chunk memory_pool[RR_BLOCKS];
static struct list_head qh_list[QH_END];
static clock_t reset_done;		/* lbolt when a port reset or resume is through */

/* the pools: enough for a 64 KB bulk transfer in flight beside the rest */
#define RR_NQTD		48
#define RR_NQH		16
#define RR_NITEM	16
static struct rr_qtd qtd_pool[RR_NQTD];
static struct rr_qh qh_pool[RR_NQH];
static struct rr_urb_item item_pool[RR_NITEM];

#define WARN_ON(c)	do { if (c) printf("rapidroad: %s:%d: %s\n", __FILE__, __LINE__, #c); } while (0)

static int
ffs32(m)
u32 m;
{
	int i;

	for (i = 0; i < 32; i++)
		if (m & (1UL << i))
			return i;
	return 0;
}

/* ---- memory management of the 20 KB on the chip from 0x1000 ------------ */

static void
init_memory()
{
	static const unsigned blocks[3] = { 8, 2, 4 };
	static const unsigned sizes[3] = { 256, 1024, 4096 };
	unsigned i, j, curr, addr = RR_PAYLOAD_OFFSET;

	for (i = 0, curr = 0; i < 3; i++)
		for (j = 0; j < blocks[i]; j++, curr++) {
			memory_pool[curr].start = addr;
			memory_pool[curr].size = sizes[i];
			memory_pool[curr].free = 1;
			addr += sizes[i];
		}
	WARN_ON(addr - memory_pool[0].start > RR_PAYLOAD_SIZE);
}

static void
alloc_mem(qtd)
struct rr_qtd *qtd;
{
	int i;

	WARN_ON(qtd->payload_addr);
	if (!qtd->length)
		return;
	for (i = 0; i < RR_BLOCKS; i++)
		if (memory_pool[i].size >= qtd->length && memory_pool[i].free) {
			memory_pool[i].free = 0;
			qtd->payload_addr = memory_pool[i].start;
			return;
		}
}

static void
free_mem(qtd)
struct rr_qtd *qtd;
{
	int i;

	if (!qtd->payload_addr)
		return;
	for (i = 0; i < RR_BLOCKS; i++)
		if (memory_pool[i].start == qtd->payload_addr) {
			WARN_ON(memory_pool[i].free);
			memory_pool[i].free = 1;
			qtd->payload_addr = 0;
			return;
		}
	WARN_ON(1);
	qtd->payload_addr = 0;
}

/* ---- the pools --------------------------------------------------------- */

static struct rr_qh *
qh_alloc()
{
	int i;

	for (i = 0; i < RR_NQH; i++)
		if (!qh_pool[i].used) {
			struct rr_qh *qh = &qh_pool[i];

			memset(qh, 0, sizeof *qh);
			qh->used = 1;
			INIT_LIST_HEAD(&qh->qh_list);
			INIT_LIST_HEAD(&qh->qtd_list);
			qh->slot = -1;
			return qh;
		}
	return (struct rr_qh *)0;
}

static struct rr_qtd *
qtd_alloc(urb, packet_type)
struct rr_urb *urb;
int packet_type;
{
	int i;

	for (i = 0; i < RR_NQTD; i++)
		if (!qtd_pool[i].used) {
			struct rr_qtd *qtd = &qtd_pool[i];

			memset(qtd, 0, sizeof *qtd);
			qtd->used = 1;
			INIT_LIST_HEAD(&qtd->qtd_list);
			qtd->urb = urb;
			qtd->packet_type = packet_type;
			qtd->status = QTD_ENQUEUED;
			return qtd;
		}
	return (struct rr_qtd *)0;
}

static void
qtd_free(qtd)
struct rr_qtd *qtd;
{
	WARN_ON(qtd->payload_addr);
	qtd->used = 0;
}

static struct rr_urb_item *
item_alloc()
{
	int i;

	for (i = 0; i < RR_NITEM; i++)
		if (!item_pool[i].used) {
			item_pool[i].used = 1;
			return &item_pool[i];
		}
	return (struct rr_urb_item *)0;
}

/* ---- the chip ---------------------------------------------------------- */

/* reset a non-running (STS_HALT == 1) controller */
static int
ehci_reset()
{
	return rr_set_and_wait_swap(CMD_RESET, 250 * 1000);
}

static int
rr_hc_setup()
{
	u32 scratch;
	int result, i;

	rr_write(HC_SCRATCH, 0xcafe);
	rr_read(HC_CHIP_ID_HIGH);		/* change the bus pattern */
	scratch = rr_read(HC_SCRATCH);
	if (scratch != 0xcafe) {
		printf("rapidroad: scratch test failed, 0x%lx\n", scratch);
		return -ENODEV;
	}

	/*
	 * The RESET_HC bit of SW_RESET is supposed to reset the host
	 * controller without touching the CPU interface registers, but on
	 * the ISP1761 at least it behaves as RESET_ALL, so the host
	 * controller is reset through the EHCI USB command register.
	 */
	rr_clear(ISO_BUF_FILL);
	rr_clear(INT_BUF_FILL);
	rr_clear(ATL_BUF_FILL);

	rr_set(HC_ATL_PTD_SKIPMAP);
	rr_set(HC_INT_PTD_SKIPMAP);
	rr_set(HC_ISO_PTD_SKIPMAP);

	result = ehci_reset();
	if (result)
		return result;

	/* ATX reset */
	rr_set(SW_RESET_RESET_ATX);
	mdelay(10);
	rr_clear(SW_RESET_RESET_ATX);

	rr_set(HW_OTG_DISABLE);
	rr_set(HW_SW_SEL_HC_DC_CLEAR);
	rr_set(HW_HC_2_DIS_CLEAR);
	rr_set(HW_DM_PULLDOWN);
	rr_set(HW_DP_PULLDOWN);
	mdelay(10);

	rr_set(HW_INTF_LOCK);

	rr_set(HC_INT_IRQ_ENABLE);
	rr_set(HC_ATL_IRQ_ENABLE);

	for (i = 0; i < QH_END; i++)
		INIT_LIST_HEAD(&qh_list[i]);
	return 0;
}

static int
rr_run()
{
	u32 chipid_h, chipid_l, chip_rev;
	int retval;

	chipid_h = rr_read(HC_CHIP_ID_HIGH);
	chipid_l = rr_read(HC_CHIP_ID_LOW);
	chip_rev = rr_read(HC_CHIP_REV);
	printf("rapidroad: USB ISP %02lx%02lx HW rev. %ld started\n", chipid_h, chipid_l, chip_rev);

	rr_clear(ISO_BUF_FILL);
	rr_clear(INT_BUF_FILL);
	rr_clear(ATL_BUF_FILL);

	rr_set(HC_ATL_PTD_SKIPMAP);
	rr_set(HC_INT_PTD_SKIPMAP);
	rr_set(HC_ISO_PTD_SKIPMAP);
	drv_usecwait(1);
	rr_clear(HC_ATL_PTD_DONEMAP);
	rr_clear(HC_INT_PTD_DONEMAP);
	rr_clear(HC_ISO_PTD_DONEMAP);

	rr_set(HW_OTG_DISABLE);
	RR_WR(ISP1763_HC_OTG_CTRL_CLEAR, 1 << 7);
	RR_WR(ISP1763_HC_OTG_CTRL_CLEAR, 1 << 15);
	mdelay(10);

	rr_set(HC_INT_IRQ_ENABLE);
	rr_set(HC_ATL_IRQ_ENABLE);

	/* the interrupt line is not passed on by the X-Surf-100 (its
	 * register 0x4800 stays 0): the done maps are polled */
	rr_set(HW_GLOBAL_INTR_EN);

	rr_clear(HC_ATL_IRQ_MASK_AND);
	rr_clear(HC_INT_IRQ_MASK_AND);
	rr_clear(HC_ISO_IRQ_MASK_AND);

	rr_set(HC_ATL_IRQ_MASK_OR);
	rr_set(HC_INT_IRQ_MASK_OR);
	rr_set(HC_ISO_IRQ_MASK_OR);

	rr_write(HC_ATL_PTD_LASTPTD, 0x8000);
	rr_write(HC_INT_PTD_LASTPTD, 0x8000);
	rr_write(HC_ISO_PTD_LASTPTD, 0x0001);

	rr_set(ATL_BUF_FILL);
	rr_set(INT_BUF_FILL);

	rr_clear(CMD_LRESET);
	rr_clear(CMD_RESET);

	retval = rr_set_and_wait(CMD_RUN, 250 * 1000);
	if (retval)
		return retval;

	retval = rr_set_and_wait(FLAG_CF, 250 * 1000);
	if (retval)
		return retval;
	return 0;
}

/*
 * The chip from reset: the CPU interface (a 16-bit bus, port 1 a host),
 * then the host controller.  0, or -1.
 */
long
rr_lowlevel_init()
{
	int i;

	memset((char *)atl_slots, 0, sizeof atl_slots);
	memset((char *)int_slots, 0, sizeof int_slots);
	atl_done_map = int_done_map = 0;
	reset_done = 0;
	for (i = 0; i < RR_NQTD; i++)
		qtd_pool[i].used = 0;
	for (i = 0; i < RR_NQH; i++)
		qh_pool[i].used = 0;
	for (i = 0; i < RR_NITEM; i++)
		item_pool[i].used = 0;
	init_memory();

	/* reset the whole chip, the CPU interface configuration included */
	rr_set(SW_RESET_RESET_ALL);
	mdelay(100);

	rr_clear(HW_DATA_BUS_WIDTH);		/* 16 bits */

	/* port 1 is a host: the OTG parts out of the way */
	rr_set(HW_SW_SEL_HC_DC);
	rr_set(HW_VBUS_DRV);
	rr_set(HW_SEL_CP_EXT);

	if (rr_hc_setup() < 0)
		return -1;
	if (rr_run() < 0) {
		printf("rapidroad: the host controller would not start\n");
		return -1;
	}
	return 0;
}

void
rr_lowlevel_stop()
{
	rr_clear(PORT_POWER);
	rr_clear(CMD_RUN);
}

/* ---- PTD construction -------------------------------------------------- */

static u32
base_to_chip(base)
u32 base;
{
	return (base - 0x400) >> 3;
}

static int
last_qtd_of_urb(qtd, qh)
struct rr_qtd *qtd;
struct rr_qh *qh;
{
	struct rr_urb *urb;

	if (list_is_last(&qtd->qtd_list, &qh->qtd_list))
		return 1;
	urb = qtd->urb;
	qtd = list_entry(qtd->qtd_list.next, struct rr_qtd, qtd_list);
	return qtd->urb != urb;
}

static void
create_ptd_atl(qh, qtd, ptd)
struct rr_qh *qh;
struct rr_qtd *qtd;
struct ptd *ptd;
{
	struct usb_device *dev = qtd->urb->dev;
	unsigned long pipe = qtd->urb->pipe;
	u32 maxpacket, multi, rl = RL_COUNTER, nak = NAK_COUNTER;

	memset((char *)ptd, 0, sizeof *ptd);

	/* according to 3.6.2, max packet len can not be > 0x400 */
	maxpacket = usb_maxpacket(dev, pipe);
	multi = 1 + ((maxpacket >> 11) & 0x3);
	maxpacket &= 0x7ff;

	/* DW0 */
	ptd->dw0 = DW0_VALID_BIT;
	ptd->dw0 |= TO_DW0_LENGTH(qtd->length);
	ptd->dw0 |= TO_DW0_MAXPACKET(maxpacket);
	ptd->dw0 |= TO_DW0_ENDPOINT(usb_pipeendpoint(pipe));

	/* DW1 */
	ptd->dw1 = usb_pipeendpoint(pipe) >> 1;
	ptd->dw1 |= TO_DW1_DEVICE_ADDR(usb_pipedevice(pipe));
	ptd->dw1 |= TO_DW1_PID_TOKEN(qtd->packet_type);

	if (usb_pipebulk(pipe))
		ptd->dw1 |= DW1_TRANS_BULK;
	else if (usb_pipeint(pipe))
		ptd->dw1 |= DW1_TRANS_INT;

	if (dev->speed != USB_SPEED_HIGH) {
		/* split transaction through the nearest high-speed hub up
		 * the tree (the root hub's own transaction translator for
		 * a device on its port) */
		struct usb_device *h = dev->parent;
		long portnr = dev->portnr;

		ptd->dw1 |= DW1_TRANS_SPLIT;
		if (dev->speed == USB_SPEED_LOW)
			ptd->dw1 |= DW1_SE_USB_LOSPEED;

		while (h && h->parent && h->speed != USB_SPEED_HIGH) {
			portnr = h->portnr;
			h = h->parent;
		}
		ptd->dw1 |= TO_DW1_PORT_NUM(portnr);
		ptd->dw1 |= TO_DW1_HUB_NUM(h ? h->devnum : 0);

		/* SE bit for Split INT transfers */
		if (usb_pipeint(pipe) && dev->speed == USB_SPEED_LOW)
			ptd->dw1 |= DW1_SE_USB_LOSPEED;

		rl = 0;
		nak = 0;
	} else {
		ptd->dw0 |= TO_DW0_MULTI(multi);
		if (usb_pipecontrol(pipe) || usb_pipebulk(pipe))
			ptd->dw3 |= TO_DW3_PING(qh->ping);
	}
	/* DW2 */
	ptd->dw2 = 0;
	ptd->dw2 |= TO_DW2_DATA_START_ADDR(base_to_chip(qtd->payload_addr));
	ptd->dw2 |= TO_DW2_RL(rl);

	/* DW3 */
	ptd->dw3 |= TO_DW3_NAKCOUNT(nak);
	ptd->dw3 |= TO_DW3_DATA_TOGGLE(qh->toggle);

	if (usb_pipecontrol(pipe)) {
		if (qtd->data_buffer == qtd->urb->setup_packet)
			ptd->dw3 &= ~TO_DW3_DATA_TOGGLE(1);
		else if (last_qtd_of_urb(qtd, qh))
			ptd->dw3 |= TO_DW3_DATA_TOGGLE(1);
	}

	ptd->dw3 |= DW3_ACTIVE_BIT;
	/* Cerr */
	ptd->dw3 |= TO_DW3_CERR(ERR_COUNTER);
}

static void
transform_add_int(qh, qtd, ptd)
struct rr_qh *qh;
struct rr_qtd *qtd;
struct ptd *ptd;
{
	int interval = qtd->urb->interval;
	u32 usof, period;

	/*
	 * Most of this is guessing (the Linux driver's words): the ISP1761
	 * datasheet is unclear.  This follows the datasheet and the USB 2.0
	 * specification as far as can be told.
	 */
	if (qtd->urb->dev->speed == USB_SPEED_HIGH) {
		/* interval in units of microframes (1/8 ms) */
		period = interval >> 3;

		if (interval > 4)
			usof = 0x01;	/* one bit set => interval 1 ms * uFrame-match */
		else if (interval > 2)
			usof = 0x22;	/* two bits set => interval 1/2 ms */
		else if (interval > 1)
			usof = 0x55;	/* four bits set => interval 1/4 ms */
		else
			usof = 0xff;	/* all bits set => interval 1/8 ms */
	} else {
		/* interval in units of frames (1 ms) */
		period = interval;
		/* execute the start split on any of the four first uFrames */
		usof = 0x0f;
		/* and the complete split on any uFrame */
		ptd->dw5 = 0xff;
	}

	/* ensure equal or shorter period than requested */
	period = period >> 1;
	/* mask off too large values and lowest unused 3 bits */
	period &= 0xf8;

	ptd->dw2 |= period;
	ptd->dw4 = usof;
}

static void
create_ptd_int(qh, qtd, ptd)
struct rr_qh *qh;
struct rr_qtd *qtd;
struct ptd *ptd;
{
	create_ptd_atl(qh, qtd, ptd);
	transform_add_int(qh, qtd, ptd);
}

/* the transfer is over: its status stands, the caller's poll sees it */
static void
rr_urb_done(urb)
struct rr_urb *urb;
{
	if (urb->status == -EINPROGRESS)
		urb->status = 0;
}

static void
start_bus_transfer(ptd_offset, slot, slots, qtd, qh, ptd)
u32 ptd_offset;
int slot;
struct rr_slotinfo *slots;
struct rr_qtd *qtd;
struct rr_qh *qh;
struct ptd *ptd;
{
	u32 skip_map;

	WARN_ON(slot < 0 || slot > RR_SLOTS - 1);
	WARN_ON(qtd->length && !qtd->payload_addr);
	WARN_ON(slots[slot].qtd);
	WARN_ON(slots[slot].qh);
	WARN_ON(qtd->status != QTD_PAYLOAD_ALLOC);

	drv_usecwait(1);

	/* make sure the done map has not triggered from some unlinked transfer */
	if (ptd_offset == RR_ATL_PTD_OFFSET) {
		skip_map = rr_read(HC_ATL_PTD_SKIPMAP);
		rr_write(HC_ATL_PTD_SKIPMAP, skip_map | (1UL << slot));
		atl_done_map |= rr_read(HC_ATL_PTD_DONEMAP);
		atl_done_map &= ~(1UL << slot);
	} else {
		skip_map = rr_read(HC_INT_PTD_SKIPMAP);
		rr_write(HC_INT_PTD_SKIPMAP, skip_map | (1UL << slot));
		int_done_map |= rr_read(HC_INT_PTD_DONEMAP);
		int_done_map &= ~(1UL << slot);
	}

	skip_map &= ~(1UL << slot);
	qh->slot = slot;
	qtd->status = QTD_XFER_STARTED;
	slots[slot].qtd = qtd;
	slots[slot].qh = qh;

	rr_ptd_write(ptd_offset, slot, ptd);

	if (ptd_offset == RR_ATL_PTD_OFFSET)
		rr_write(HC_ATL_PTD_SKIPMAP, skip_map);
	else
		rr_write(HC_INT_PTD_SKIPMAP, skip_map);
}

static int
is_short_bulk(qtd)
struct rr_qtd *qtd;
{
	return usb_pipebulk(qtd->urb->pipe) && qtd->actual_length < qtd->length;
}

static void
collect_qtds(qh, urb_list)
struct rr_qh *qh;
struct list_head *urb_list;
{
	struct rr_qtd *qtd, *qtd_next;
	struct rr_urb_item *item;
	struct list_head *p;
	int last_qtd;

	for (p = qh->qtd_list.next; p != &qh->qtd_list; ) {
		qtd = list_entry(p, struct rr_qtd, qtd_list);
		p = p->next;
		qtd_next = p == &qh->qtd_list ? (struct rr_qtd *)0 : list_entry(p, struct rr_qtd, qtd_list);

		if (qtd->status < QTD_XFER_COMPLETE)
			break;

		last_qtd = last_qtd_of_urb(qtd, qh);

		if (!last_qtd && qtd->status == QTD_RETIRE)
			qtd_next->status = QTD_RETIRE;

		if (qtd->status == QTD_XFER_COMPLETE) {
			if (qtd->actual_length) {
				switch (qtd->packet_type) {
				case IN_PID:
					rr_mem_read(qtd->payload_addr, qtd->data_buffer, qtd->actual_length);
					/* FALLTHROUGH */
				case OUT_PID:
					qtd->urb->actual_length += qtd->actual_length;
					/* FALLTHROUGH */
				case SETUP_PID:
					break;
				}
			}

			if (is_short_bulk(qtd)) {
				if (!last_qtd)
					qtd_next->status = QTD_RETIRE;
			}
		}

		if (qtd->payload_addr)
			free_mem(qtd);

		if (last_qtd) {
			if (qtd->status == QTD_RETIRE && qtd->urb->status == -EINPROGRESS)
				qtd->urb->status = -EPIPE;

			/* defer urb_done() */
			item = item_alloc();
			if (item == (struct rr_urb_item *)0)
				break;	/* try again on next call */
			item->urb = qtd->urb;
			list_add_tail(&item->urb_list, urb_list);
		}

		list_del(&qtd->qtd_list);
		qtd_free(qtd);
	}
}

#define ENQUEUE_DEPTH	2
static void
enqueue_qtds(qh)
struct rr_qh *qh;
{
	u32 ptd_offset;
	struct rr_slotinfo *slots;
	int curr_slot, free_slot, n;
	struct ptd ptd;
	struct rr_qtd *qtd;
	struct list_head *p;

	if (list_empty(&qh->qtd_list))
		return;

	if (usb_pipeint(list_entry(qh->qtd_list.next, struct rr_qtd, qtd_list)->urb->pipe)) {
		ptd_offset = RR_INT_PTD_OFFSET;
		slots = int_slots;
	} else {
		ptd_offset = RR_ATL_PTD_OFFSET;
		slots = atl_slots;
	}

	free_slot = -1;
	for (curr_slot = 0; curr_slot < RR_SLOTS; curr_slot++) {
		if (free_slot == -1 && !slots[curr_slot].qtd)
			free_slot = curr_slot;
		if (slots[curr_slot].qh == qh)
			break;
	}

	n = 0;
	for (p = qh->qtd_list.next; p != &qh->qtd_list; p = p->next) {
		qtd = list_entry(p, struct rr_qtd, qtd_list);
		if (qtd->status == QTD_ENQUEUED) {
			WARN_ON(qtd->payload_addr);
			alloc_mem(qtd);
			if (qtd->length && !qtd->payload_addr)
				break;

			if (qtd->length && (qtd->packet_type == SETUP_PID || qtd->packet_type == OUT_PID))
				rr_mem_write(qtd->payload_addr, qtd->data_buffer, qtd->length);

			qtd->status = QTD_PAYLOAD_ALLOC;
		}

		if (qtd->status == QTD_PAYLOAD_ALLOC) {
			/* start the transfer for this endpoint if not already done */
			if (curr_slot > RR_SLOTS - 1 && free_slot > -1) {
				if (usb_pipeint(qtd->urb->pipe))
					create_ptd_int(qh, qtd, &ptd);
				else
					create_ptd_atl(qh, qtd, &ptd);

				start_bus_transfer(ptd_offset, free_slot, slots, qtd, qh, &ptd);
				curr_slot = free_slot;
			}

			n++;
			if (n >= ENQUEUE_DEPTH)
				break;
		}
	}
}

static void
schedule_ptds()
{
	struct list_head urb_list, *p, *q;
	struct rr_urb_item *item;
	int i;

	INIT_LIST_HEAD(&urb_list);

	/* finished and retired transfers: payloads out, urb_done() */
	for (i = 0; i < QH_END; i++)
		for (p = qh_list[i].next; p != &qh_list[i]; p = q) {
			q = p->next;
			collect_qtds(list_entry(p, struct rr_qh, qh_list), &urb_list);
		}

	for (p = urb_list.next; p != &urb_list; p = q) {
		q = p->next;
		item = list_entry(p, struct rr_urb_item, urb_list);
		rr_urb_done(item->urb);
		list_del(&item->urb_list);
		item->used = 0;
	}

	/*
	 * Schedule packets for transfer: queued control transfers first,
	 * then interrupt, then bulk, as long as payload memory and PTD
	 * slots are available; double buffering (ENQUEUE_DEPTH 2).
	 */
	for (i = 0; i < QH_END; i++)
		for (p = qh_list[i].next; p != &qh_list[i]; p = q) {
			q = p->next;
			enqueue_qtds(list_entry(p, struct rr_qh, qh_list));
		}
}

#define PTD_STATE_QTD_DONE	1
#define PTD_STATE_QTD_RELOAD	2
#define PTD_STATE_URB_RETIRE	3

static int
check_int_transfer(ptd, urb)
struct ptd *ptd;
struct rr_urb *urb;
{
	u32 dw4 = ptd->dw4 >> 8;
	int i;

	if (ptd->dw3 & DW3_HALT_BIT) {
		urb->status = -EPROTO;	/* default unknown error */

		for (i = 0; i < 8; i++) {
			switch (dw4 & 0x7) {
			case INT_UNDERRUN:
				printf("rapidroad: underrun during uFrame %d\n", i);
				urb->status = -ECOMM;	/* could not write data */
				break;
			case INT_EXACT:
				printf("rapidroad: transaction error uFrame %d\n", i);
				/* timeout, bad CRC, PID error etc. */
				urb->status = -EPROTO;
				break;
			case INT_BABBLE:
				printf("rapidroad: babble error during uFrame %d\n", i);
				urb->status = -EOVERFLOW;
				break;
			}
			dw4 >>= 3;
		}
		return PTD_STATE_URB_RETIRE;
	}
	return PTD_STATE_QTD_DONE;
}

static int
check_atl_transfer(ptd, urb)
struct ptd *ptd;
struct rr_urb *urb;
{
	if (ptd->dw3 & DW3_HALT_BIT) {
		if (ptd->dw3 & DW3_BABBLE_BIT)
			urb->status = -EOVERFLOW;
		else if (FROM_DW3_CERR(ptd->dw3))
			urb->status = -EPIPE;	/* stall */
		else
			urb->status = -EPROTO;	/* unknown */
		return PTD_STATE_URB_RETIRE;
	}

	/* transfer error, *but* active and no HALT -> reload */
	if ((ptd->dw3 & DW3_ERROR_BIT) && (ptd->dw3 & DW3_ACTIVE_BIT))
		return PTD_STATE_QTD_RELOAD;

	/*
	 * NAKs are handled in hardware by the chip, usually when the
	 * device cannot send data fast enough.
	 */
	if (!FROM_DW3_NAKCOUNT(ptd->dw3) && (ptd->dw3 & DW3_ACTIVE_BIT))
		return PTD_STATE_QTD_RELOAD;

	return PTD_STATE_QTD_DONE;
}

static void
handle_done_ptds()
{
	struct rr_slotinfo *slots;
	struct rr_qtd *qtd;
	struct rr_qh *qh;
	struct ptd ptd;
	u32 ptd_offset, skip_map;
	int modified, state, slot;

	skip_map = rr_read(HC_INT_PTD_SKIPMAP);
	int_done_map &= ~skip_map;
	skip_map = rr_read(HC_ATL_PTD_SKIPMAP);
	atl_done_map &= ~skip_map;

	modified = int_done_map || atl_done_map;

	while (int_done_map || atl_done_map) {
		if (int_done_map) {
			/* INT ptd */
			slot = ffs32(int_done_map);
			int_done_map &= ~(1UL << slot);
			slots = int_slots;
			if (!slots[slot].qh) {
				WARN_ON(1);
				continue;
			}
			ptd_offset = RR_INT_PTD_OFFSET;
			rr_ptd_read(RR_INT_PTD_OFFSET, slot, &ptd);
			state = check_int_transfer(&ptd, slots[slot].qtd->urb);
		} else {
			/* ATL ptd */
			slot = ffs32(atl_done_map);
			atl_done_map &= ~(1UL << slot);
			slots = atl_slots;
			if (!slots[slot].qh) {
				WARN_ON(1);
				continue;
			}
			ptd_offset = RR_ATL_PTD_OFFSET;
			rr_ptd_read(RR_ATL_PTD_OFFSET, slot, &ptd);
			state = check_atl_transfer(&ptd, slots[slot].qtd->urb);
		}

		qtd = slots[slot].qtd;
		slots[slot].qtd = (struct rr_qtd *)0;
		qh = slots[slot].qh;
		slots[slot].qh = (struct rr_qh *)0;
		qh->slot = -1;

		WARN_ON(qtd->status != QTD_XFER_STARTED);

		switch (state) {
		case PTD_STATE_QTD_DONE:
			if (usb_pipeint(qtd->urb->pipe) && qtd->urb->dev->speed != USB_SPEED_HIGH)
				qtd->actual_length = FROM_DW3_SCS_NRBYTESTRANSFERRED(ptd.dw3);
			else
				qtd->actual_length = FROM_DW3_NRBYTESTRANSFERRED(ptd.dw3);

			qtd->status = QTD_XFER_COMPLETE;

			if (list_is_last(&qtd->qtd_list, &qh->qtd_list) || is_short_bulk(qtd))
				qtd = (struct rr_qtd *)0;
			else
				qtd = list_entry(qtd->qtd_list.next, struct rr_qtd, qtd_list);

			qh->toggle = FROM_DW3_DATA_TOGGLE(ptd.dw3);
			qh->ping = FROM_DW3_PING(ptd.dw3);
			break;

		case PTD_STATE_QTD_RELOAD:	/* QTD_RETRY, for atls only */
			qtd->status = QTD_PAYLOAD_ALLOC;
			ptd.dw0 |= DW0_VALID_BIT;
			/* RL counter = ERR counter */
			ptd.dw3 &= ~TO_DW3_NAKCOUNT(0xf);
			ptd.dw3 |= TO_DW3_NAKCOUNT(FROM_DW2_RL(ptd.dw2));
			ptd.dw3 &= ~TO_DW3_CERR(3);
			ptd.dw3 |= TO_DW3_CERR(ERR_COUNTER);

			qh->toggle = FROM_DW3_DATA_TOGGLE(ptd.dw3);
			qh->ping = FROM_DW3_PING(ptd.dw3);
			break;

		case PTD_STATE_URB_RETIRE:
			qtd->status = QTD_RETIRE;
			qtd = (struct rr_qtd *)0;
			qh->toggle = 0;
			qh->ping = 0;
			break;

		default:
			WARN_ON(1);
			continue;
		}

		if (qtd && qtd->status == QTD_PAYLOAD_ALLOC) {
			if (slots == int_slots) {
				if (state == PTD_STATE_QTD_RELOAD)
					printf("rapidroad: PTD_STATE_QTD_RELOAD on interrupt packet\n");
				if (state != PTD_STATE_QTD_RELOAD)
					create_ptd_int(qh, qtd, &ptd);
			} else {
				if (state != PTD_STATE_QTD_RELOAD)
					create_ptd_atl(qh, qtd, &ptd);
			}
			start_bus_transfer(ptd_offset, slot, slots, qtd, qh, &ptd);
		}
	}

	if (modified)
		schedule_ptds();
}

/*
 * What the interrupt handler would do, called from the poll loop: the
 * interrupt status acknowledged, the done maps (cleared by the read)
 * gathered and handled.  Polling, the maps are read whether or not the
 * status register says so.
 */
static int
rr_isr()
{
	u32 imask;

	imask = rr_read(HC_INTERRUPT);
	if (imask)
		RR_WR(ISP1763_HC_INTERRUPT, imask);

	int_done_map |= rr_read(HC_INT_PTD_DONEMAP);
	atl_done_map |= rr_read(HC_ATL_PTD_DONEMAP);
	if (!int_done_map && !atl_done_map)
		return 0;

	handle_done_ptds();
	return 1;
}

/* ---- URBs -------------------------------------------------------------- */

static void
qtd_list_free(qtd_list)
struct list_head *qtd_list;
{
	struct list_head *p, *q;
	struct rr_qtd *qtd;

	for (p = qtd_list->next; p != qtd_list; p = q) {
		q = p->next;
		qtd = list_entry(p, struct rr_qtd, qtd_list);
		list_del(&qtd->qtd_list);
		qtd_free(qtd);
	}
}

/*
 * Packetize urb->transfer_buffer into a list of packets of at most a
 * payload block each, with the PID type (SETUP/IN/OUT) of each.
 */
#define max_packet(wMaxPacketSize)	((wMaxPacketSize) & 0x07ff)
static void
packetize_urb(urb, head)
struct rr_urb *urb;
struct list_head *head;
{
	struct rr_qtd *qtd;
	u8 *buf;
	long len, maxpacketsize;
	int packet_type;

	if (!urb->transfer_buffer && urb->transfer_buffer_length) {
		printf("rapidroad: a transfer of %lu bytes without a buffer\n", urb->transfer_buffer_length);
		WARN_ON(1);
	}

	if (usb_pipein(urb->pipe))
		packet_type = IN_PID;
	else
		packet_type = OUT_PID;

	if (usb_pipecontrol(urb->pipe)) {
		qtd = qtd_alloc(urb, SETUP_PID);
		if (!qtd)
			goto cleanup;
		qtd->data_buffer = urb->setup_packet;
		qtd->length = sizeof(struct devrequest);
		list_add_tail(&qtd->qtd_list, head);

		/* for zero length DATA stages, STATUS is always IN */
		if (urb->transfer_buffer_length == 0)
			packet_type = IN_PID;
	}

	maxpacketsize = max_packet(usb_maxpacket(urb->dev, urb->pipe));

	/*
	 * the buffer gets wrapped in one or more qtds; the last one may be
	 * "short" (including zero length) and may serve as a control
	 * status ack
	 */
	buf = urb->transfer_buffer;
	len = urb->transfer_buffer_length;

	for (;;) {
		long this_qtd_len;

		qtd = qtd_alloc(urb, packet_type);
		if (!qtd)
			goto cleanup;

		this_qtd_len = len > RR_MAXXFER ? RR_MAXXFER : len;
		qtd->data_buffer = buf;
		qtd->length = this_qtd_len;
		list_add_tail(&qtd->qtd_list, head);

		len -= this_qtd_len;
		buf += this_qtd_len;

		if (len <= 0)
			break;
	}

	/*
	 * control requests may need a terminating data "status" ack;
	 * bulk ones a terminating short packet (zero length) - not asked
	 * for by this stack
	 */
	if (urb->transfer_buffer_length != 0 && usb_pipecontrol(urb->pipe)) {
		packet_type = packet_type == IN_PID ? OUT_PID : IN_PID;
		qtd = qtd_alloc(urb, packet_type);
		if (!qtd)
			goto cleanup;
		/* never any data in such packets */
		qtd->data_buffer = (u8 *)0;
		qtd->length = 0;
		list_add_tail(&qtd->qtd_list, head);
	}
	return;

cleanup:
	qtd_list_free(head);
}

static int
rr_urb_enqueue(urb)
struct rr_urb *urb;
{
	struct rr_qh *qh = (struct rr_qh *)0;
	struct list_head *ep_queue, new_qtds, *p;
	int qh_in_queue, epnum;

	switch (usb_pipetype(urb->pipe)) {
	case PIPE_CONTROL:
		ep_queue = &qh_list[QH_CONTROL];
		break;
	case PIPE_BULK:
		ep_queue = &qh_list[QH_BULK];
		break;
	case PIPE_INTERRUPT:
		ep_queue = &qh_list[QH_INTERRUPT];
		break;
	case PIPE_ISOCHRONOUS:
		printf("rapidroad: isochronous USB packets not yet supported\n");
		return -EPIPE;
	default:
		printf("rapidroad: unknown pipe type\n");
		return -EPIPE;
	}

	if (usb_pipein(urb->pipe))
		urb->actual_length = 0;

	INIT_LIST_HEAD(&new_qtds);
	packetize_urb(urb, &new_qtds);
	if (list_empty(&new_qtds))
		return -ENOMEM;

	epnum = usb_pipeendpoint(urb->pipe);

	qh_in_queue = 0;
	for (p = ep_queue->next; p != ep_queue; p = p->next) {
		qh = list_entry(p, struct rr_qh, qh_list);
		if (qh->epnum == epnum) {
			qh_in_queue = 1;
			break;
		}
	}

	if (!qh_in_queue) {
		qh = qh_alloc();
		if (!qh) {
			qtd_list_free(&new_qtds);
			return -ENOMEM;
		}
		qh->epnum = epnum;
		list_add_tail(&qh->qh_list, ep_queue);
	}
	urb->qh = qh;

	list_splice_tail(&new_qtds, &qh->qtd_list);
	schedule_ptds();
	return 0;
}

static void
kill_transfer(urb, qh)
struct rr_urb *urb;
struct rr_qh *qh;
{
	u32 skip_map;

	WARN_ON(qh->slot == -1);

	/*
	 * the slot is reclaimed by force: some transfers never return,
	 * interrupt transfers and NAKed bulk transfers for instance
	 */
	if (usb_pipecontrol(urb->pipe) || usb_pipebulk(urb->pipe)) {
		if (qh->slot != -1) {
			skip_map = rr_read(HC_ATL_PTD_SKIPMAP);
			skip_map |= 1UL << qh->slot;
			rr_write(HC_ATL_PTD_SKIPMAP, skip_map);
			drv_usecwait(1);
			atl_slots[qh->slot].qh = (struct rr_qh *)0;
			atl_slots[qh->slot].qtd = (struct rr_qtd *)0;
		}
	} else {
		if (qh->slot != -1) {
			skip_map = rr_read(HC_INT_PTD_SKIPMAP);
			skip_map |= 1UL << qh->slot;
			rr_write(HC_INT_PTD_SKIPMAP, skip_map);
			int_slots[qh->slot].qh = (struct rr_qh *)0;
			int_slots[qh->slot].qtd = (struct rr_qtd *)0;
		}
	}
	qh->slot = -1;
}

/*
 * Retire the qtds beginning at 'qtd' and belonging all to the same urb,
 * killing any active transfer belonging to the urb in the process.
 */
static void
dequeue_urb_from_qtd(qh, qtd)
struct rr_qh *qh;
struct rr_qtd *qtd;
{
	struct rr_urb *urb = qtd->urb;
	struct list_head *p;

	for (p = &qtd->qtd_list; p != &qh->qtd_list; p = p->next) {
		qtd = list_entry(p, struct rr_qtd, qtd_list);
		if (qtd->urb != urb)
			break;
		if (qtd->status == QTD_XFER_STARTED)
			kill_transfer(urb, qh);
		qtd->status = QTD_RETIRE;
	}
}

static int
rr_urb_dequeue(urb, status)
struct rr_urb *urb;
int status;
{
	struct rr_qh *qh = urb->qh;
	struct rr_qtd *qtd;
	struct list_head *p;

	if (!qh)
		return -EINVAL;

	for (p = qh->qtd_list.next; p != &qh->qtd_list; p = p->next) {
		qtd = list_entry(p, struct rr_qtd, qtd_list);
		if (qtd->urb == urb) {
			dequeue_urb_from_qtd(qh, qtd);
			break;
		}
	}

	urb->status = status;
	schedule_ptds();
	return 0;
}

/* ---- the root hub ------------------------------------------------------ */

/* the request type codes the hub code sends, request | requesttype << 8 */
#define DeviceRequest		((USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE) << 8)
#define DeviceOutRequest	((USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE) << 8)
#define ClearHubFeature		(((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE) << 8) | USB_REQ_CLEAR_FEATURE)
#define ClearPortFeature	(((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER) << 8) | USB_REQ_CLEAR_FEATURE)
#define GetHubDescriptor	(((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE) << 8) | USB_REQ_GET_DESCRIPTOR)
#define GetHubStatus		(((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE) << 8) | USB_REQ_GET_STATUS)
#define GetPortStatus		(((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER) << 8) | USB_REQ_GET_STATUS)
#define SetHubFeature		(((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE) << 8) | USB_REQ_SET_FEATURE)
#define SetPortFeature		(((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER) << 8) | USB_REQ_SET_FEATURE)

/* wHubCharacteristics */
#define HUB_CHAR_INDV_PORT_LPSM	0x0001
#define HUB_CHAR_NO_LPSM	0x0002
#define HUB_CHAR_INDV_PORT_OCPM	0x0008

/* the root hub's descriptors, in wire (little-endian) order */
static const u8 rh_device_desc[18] = {
	0x12, USB_DT_DEVICE,
	0x00, 0x02,		/* bcdUSB 2.0 */
	USB_CLASS_HUB, 0x00,
	0x00,			/* bDeviceProtocol: no TT in the descriptor */
	0x40,			/* bMaxPacketSize0 */
	0x6b, 0x1d,		/* idVendor 0x1d6b (Linux Foundation) */
	0x02, 0x00,		/* idProduct 0x0002 */
	0x00, 0x01,		/* bcdDevice 1.0 */
	0x01, 0x02, 0x00,	/* iManufacturer, iProduct, iSerialNumber */
	0x01			/* bNumConfigurations */
};
static const u8 rh_config_desc[25] = {
	/* configuration */
	0x09, USB_DT_CONFIG, 0x19, 0x00, 0x01, 0x01, 0x00, 0xc0, 0x00,
	/* interface */
	0x09, USB_DT_INTERFACE, 0x00, 0x00, 0x01, USB_CLASS_HUB, 0x00, 0x00, 0x00,
	/* endpoint: the status change interrupt endpoint 1 IN */
	0x07, USB_DT_ENDPOINT, 0x81, 0x03, (USB_MAXCHILDREN + 1 + 7) / 8, 0x00, 0x0c
};
static const u8 rh_string0[4] = { 4, USB_DT_STRING, 0x09, 0x04 };
static const u8 rh_string1[20] = { 20, USB_DT_STRING,
	'R', 0, 'a', 0, 'p', 0, 'i', 0, 'd', 0, 'R', 0, 'o', 0, 'a', 0, 'd', 0 };
static const u8 rh_string2[34] = { 34, USB_DT_STRING,
	'I', 0, 'S', 0, 'P', 0, '1', 0, '7', 0, '6', 0, '3', 0, ' ', 0,
	'r', 0, 'o', 0, 'o', 0, 't', 0, ' ', 0, 'h', 0, 'u', 0, 'b', 0 };

static void
rr_hub_descriptor(buf)
u8 *buf;
{
	u16 temp;

	buf[0] = 9;			/* bLength: 7 + 2 for one port */
	buf[1] = USB_DT_HUB;
	buf[2] = 1;			/* bNbrPorts */
	/* per-port overcurrent reporting and power switching */
	temp = HUB_CHAR_INDV_PORT_OCPM | HUB_CHAR_INDV_PORT_LPSM;
	buf[3] = temp & 0xff;
	buf[4] = temp >> 8;
	buf[5] = 10;			/* bPwrOn2PwrGood: 20 ms */
	buf[6] = 0;			/* bHubContrCurrent */
	buf[7] = 0x00;			/* DeviceRemovable */
	buf[8] = 0xff;			/* PortPwrCtrlMask */
}

static clock_t
rr_ticks(ms)
long ms;
{
	clock_t t = drv_usectohz(ms * 1000);

	return t > 0 ? t : 1;
}

static void
check_reset_complete(index)
int index;
{
	if (!rr_is_set(PORT_CONNECT))
		return;

	/* if the reset finished and the port is still not enabled: a
	 * full-speed device, which an EHCI would hand to a companion
	 * controller; this chip's transaction translator takes it */
	if (!rr_is_set(PORT_PE)) {
		printf("rapidroad: port %d full speed\n", index + 1);
		rr_set(PORT_OWNER);
		rr_clear(PORT_CSC);
	} else
		printf("rapidroad: port %d high speed\n", index + 1);
}

static long
rr_hub_control(dev, pipe, buffer, length, setup)
struct usb_device *dev;
unsigned long pipe;
void *buffer;
long length;
struct devrequest *setup;
{
	u16 typeReq, wValue, wIndex;
	u8 *buf = buffer;
	const u8 *src = (const u8 *)0;
	long src_len = 0;
	int retval;
	u32 status;
	u8 hubdesc[9];

	if (!setup)
		return -EINVAL;

	typeReq = setup->request | (setup->requesttype << 8);
	wValue = le2cpu16(setup->value);
	wIndex = le2cpu16(setup->index);

	switch (typeReq) {
	case DeviceOutRequest | USB_REQ_SET_ADDRESS:
	case DeviceOutRequest | USB_REQ_SET_CONFIGURATION:
		/* nothing to do */
		break;
	case DeviceRequest | USB_REQ_GET_DESCRIPTOR:
		switch (wValue & 0xff00) {
		case USB_DT_DEVICE << 8:
			src = rh_device_desc;
			src_len = sizeof rh_device_desc;
			break;
		case USB_DT_CONFIG << 8:
			src = rh_config_desc;
			src_len = sizeof rh_config_desc;
			break;
		case USB_DT_STRING << 8:
			switch (wValue & 0xff) {
			case 0:
				src = rh_string0;
				src_len = sizeof rh_string0;
				break;
			case 1:
				src = rh_string1;
				src_len = sizeof rh_string1;
				break;
			case 2:
				src = rh_string2;
				src_len = sizeof rh_string2;
				break;
			default:
				goto error;
			}
			break;
		default:
			goto error;
		}
		break;
	case ClearHubFeature:
		switch (wValue) {
		case C_HUB_LOCAL_POWER:
		case C_HUB_OVER_CURRENT:
			/* no hub-wide feature/status flags */
			break;
		default:
			goto error;
		}
		break;
	case ClearPortFeature:
		if (wIndex != 1)
			goto error;
		wIndex--;

		switch (wValue) {
		case USB_PORT_FEAT_ENABLE:
			rr_clear(PORT_PE);
			break;
		case USB_PORT_FEAT_C_ENABLE:
			break;
		case USB_PORT_FEAT_SUSPEND:
			if (rr_is_set(PORT_RESET))
				goto error;
			if (rr_is_set(PORT_SUSPEND)) {
				if (!rr_is_set(PORT_PE))
					goto error;
				/* resume signaling for 20 msec */
				rr_clear(PORT_CSC);
				rr_set(PORT_RESUME);
				reset_done = lbolt + rr_ticks(40);
			}
			break;
		case USB_PORT_FEAT_C_SUSPEND:
			/* auto-cleared */
			break;
		case USB_PORT_FEAT_POWER:
			rr_clear(PORT_POWER);
			break;
		case USB_PORT_FEAT_C_CONNECTION:
			rr_set(PORT_CSC);
			break;
		case USB_PORT_FEAT_C_OVER_CURRENT:
			break;
		case USB_PORT_FEAT_C_RESET:
			/* GetPortStatus clears reset */
			break;
		default:
			goto error;
		}
		rr_read(CMD_RUN);
		break;
	case GetHubDescriptor:
		rr_hub_descriptor(hubdesc);
		src = hubdesc;
		src_len = sizeof hubdesc;
		break;
	case GetHubStatus:
		/* no hub-wide feature/status flags */
		memset(buf, 0, 4);
		length = 4;
		break;
	case GetPortStatus:
		if (wIndex != 1)
			goto error;
		wIndex--;
		status = 0;

		/* wPortChange bits */
		if (rr_is_set(PORT_CSC))
			status |= (u32)USB_PORT_STAT_C_CONNECTION << 16;

		/* whoever resumes must GetPortStatus to complete it */
		if (rr_is_set(PORT_RESUME)) {
			status |= (u32)USB_PORT_STAT_C_SUSPEND << 16;

			if (!reset_done)
				reset_done = lbolt + rr_ticks(20);
			else if (lbolt > reset_done) {
				/* stop resume signaling */
				rr_clear(PORT_CSC);
				retval = rr_clear_and_wait(PORT_RESUME, 2000);
				if (retval != 0) {
					printf("rapidroad: port %d resume error %d\n", wIndex + 1, retval);
					goto error;
				}
			}
		}

		/* whoever resets must GetPortStatus to complete it */
		if (rr_is_set(PORT_RESET) && lbolt > reset_done) {
			status |= (u32)USB_PORT_STAT_C_RESET << 16;
			reset_done = 0;

			/* force the reset to complete; some hardware needs
			 * 550+ us to clear the bit */
			retval = rr_clear_and_wait(PORT_RESET, 750);
			if (retval != 0) {
				printf("rapidroad: port %d reset error %d\n", wIndex + 1, retval);
				goto error;
			}
			check_reset_complete(wIndex);
		}

		if (rr_is_set(PORT_OWNER))
			printf("rapidroad: PORT_OWNER is set\n");

		if (rr_is_set(PORT_CONNECT)) {
			status |= USB_PORT_STAT_CONNECTION;
			/* status may be from the integrated TT */
			status |= USB_PORT_STAT_HIGH_SPEED;
		}
		if (rr_is_set(PORT_PE))
			status |= USB_PORT_STAT_ENABLE;
		if (rr_is_set(PORT_SUSPEND) && rr_is_set(PORT_RESUME))
			status |= USB_PORT_STAT_SUSPEND;
		if (rr_is_set(PORT_RESET))
			status |= USB_PORT_STAT_RESET;
		if (rr_is_set(PORT_POWER))
			status |= USB_PORT_STAT_POWER;

		buf[0] = status & 0xff;
		buf[1] = (status >> 8) & 0xff;
		buf[2] = (status >> 16) & 0xff;
		buf[3] = (status >> 24) & 0xff;
		length = 4;
		break;
	case SetHubFeature:
		switch (wValue) {
		case C_HUB_LOCAL_POWER:
		case C_HUB_OVER_CURRENT:
			break;
		default:
			goto error;
		}
		break;
	case SetPortFeature:
		wIndex &= 0xff;
		if (wIndex != 1)
			goto error;
		wIndex--;

		if (rr_is_set(PORT_OWNER))
			break;

		switch (wValue) {
		case USB_PORT_FEAT_ENABLE:
			rr_set(PORT_PE);
			break;
		case USB_PORT_FEAT_SUSPEND:
			if (!rr_is_set(PORT_PE) || rr_is_set(PORT_RESET))
				goto error;
			rr_set(PORT_SUSPEND);
			break;
		case USB_PORT_FEAT_POWER:
			rr_set(PORT_POWER);
			break;
		case USB_PORT_FEAT_RESET:
			if (rr_is_set(PORT_RESUME))
				goto error;
			/* line status bits may report this as low speed,
			 * which is fine with the transaction translator */
			if (rr_is_set(PORT_CONNECT) && !rr_is_set(PORT_PE) && rr_read(PORT_LSTATUS) == 1) {
				rr_set(PORT_OWNER);
			} else {
				rr_set(PORT_RESET);
				rr_clear(PORT_PE);
				reset_done = lbolt + rr_ticks(50);
			}
			break;
		default:
			goto error;
		}
		break;

	default:
		printf("rapidroad: root hub: unknown request 0x%x\n", typeReq);
		goto error;
	}

	if (src_len) {
		if (src_len < length)
			length = src_len;
		if (length > 0)
			bcopy((caddr_t)src, (caddr_t)buffer, length);
	}

	dev->act_len = length;
	dev->status = 0;
	return 0;

error:
	/* "stall" on error */
	dev->act_len = 0;
	dev->status = USB_ST_STALLED;
	return -1;
}

/* ---- submitting: what U-Boot's usb_urb_submit does, polled ------------- */

/*
 * The transfer is enqueued and the done maps polled until it is over or
 * the time is up: busily for the first two milliseconds (most control
 * transfers to a high-speed device are through by then), then a tick at
 * a time.  dev->status and dev->act_len tell the stack what happened.
 */
static long
rr_submit(dev, pipe, type, buffer, len, setup, interval)
struct usb_device *dev;
unsigned long pipe;
int type;
void *buffer;
long len;
struct devrequest *setup;
long interval;
{
	struct rr_urb urb;
	clock_t deadline;
	int spins = 0, ret;

	if (dev->parent == (struct usb_device *)0) {
		/* the root hub */
		dev->speed = USB_SPEED_HIGH;
		if (type != PIPE_CONTROL) {
			dev->act_len = 0;
			dev->status = USB_ST_STALLED;
			return -1;
		}
		return rr_hub_control(dev, pipe, buffer, len, setup);
	}

	memset((char *)&urb, 0, sizeof urb);
	urb.dev = dev;
	urb.pipe = pipe;
	urb.type = type;
	urb.status = -EINPROGRESS;
	urb.transfer_buffer = buffer;
	urb.transfer_buffer_length = len;
	urb.setup_packet = (u8 *)setup;
	urb.interval = interval;

	ret = rr_urb_enqueue(&urb);
	if (ret < 0) {
		printf("rapidroad: could not enqueue a transfer (%d)\n", ret);
		dev->act_len = 0;
		dev->status = USB_ST_CRC_ERR;
		return -1;
	}

	deadline = lbolt + rr_ticks(USB_TIMEOUT_MS(pipe));
	for (;;) {
		rr_isr();
		if (urb.status != -EINPROGRESS)
			break;
		if (lbolt >= deadline) {
			rr_urb_dequeue(&urb, -ETIME);
			break;
		}
		if (spins < 40) {
			spins++;
			drv_usecwait(50);
		} else
			delay(1);
	}

	dev->act_len = urb.actual_length;
	switch (urb.status) {
	case 0:
		dev->status = 0;
		break;
	case -EPIPE:
		dev->status = USB_ST_STALLED;
		break;
	case -EOVERFLOW:
		dev->status = USB_ST_BABBLE_DET;
		break;
	case -ETIME:
		dev->status = USB_ST_NAK_REC | USB_ST_CRC_ERR;
		break;
	default:
		dev->status = USB_ST_CRC_ERR;
		break;
	}
	return urb.status ? -1 : 0;
}

long
rr_submit_control_msg(dev, pipe, buffer, len, setup)
struct usb_device *dev;
unsigned long pipe;
void *buffer;
long len;
struct devrequest *setup;
{
	return rr_submit(dev, pipe, PIPE_CONTROL, buffer, len, setup, 0L);
}

long
rr_submit_bulk_msg(dev, pipe, buffer, len)
struct usb_device *dev;
unsigned long pipe;
void *buffer;
long len;
{
	return rr_submit(dev, pipe, PIPE_BULK, buffer, len, (struct devrequest *)0, 0L);
}

long
rr_submit_int_msg(dev, pipe, buffer, len, interval)
struct usb_device *dev;
unsigned long pipe;
void *buffer;
long len, interval;
{
	return rr_submit(dev, pipe, PIPE_INTERRUPT, buffer, len, (struct devrequest *)0, interval);
}
