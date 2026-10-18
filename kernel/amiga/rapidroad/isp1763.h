/* SPDX-License-Identifier: GPL-2.0 */
/*
 * isp1763.h - the ST-Ericsson/NXP ISP1763A host controller as the
 * RapidRoad carries it: registers, their fields, the chip memory
 * layout.  From Linux's and U-Boot's isp1760 driver
 * (drivers/usb/isp1760/isp1760-regs.h, isp1760-core.c):
 *
 * Copyright 2021 Linaro, Rui Miguel Silva
 * Copyright 2014 Laurent Pinchart
 * Copyright 2007 Sebastian Siewior
 *
 * The chip's registers are 16 bits wide at even byte addresses.  The
 * X-Surf-100 shows them with a four byte spacing: register r is the word
 * at rr_base + r * 4, where rr_base is one of the card's two windows,
 * settled at probe time by reading the chip ID (see rrmod.c).
 */
#ifndef ISP1763_H
#define ISP1763_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

extern char *rr_base;		/* the register window */

#define RR_REG(r)	(*(__volatile__ u16 *)(rr_base + ((r) << 2)))
#define RR_RD(r)	RR_REG(r)
#define RR_WR(r, v)	(RR_REG(r) = (u16)(v))

/* EHCI operational registers */
#define ISP1763_HC_USBCMD		0x8c
#define ISP1763_HC_USBSTS		0x90
#define ISP1763_HC_FRINDEX		0x98
#define ISP1763_HC_CONFIGFLAG		0x9c
#define ISP1763_HC_PORTSC1		0xa0
#define ISP1763_HC_ISO_PTD_DONEMAP	0xa4
#define ISP1763_HC_ISO_PTD_SKIPMAP	0xa6
#define ISP1763_HC_ISO_PTD_LASTPTD	0xa8
#define ISP1763_HC_INT_PTD_DONEMAP	0xaa
#define ISP1763_HC_INT_PTD_SKIPMAP	0xac
#define ISP1763_HC_INT_PTD_LASTPTD	0xae
#define ISP1763_HC_ATL_PTD_DONEMAP	0xb0
#define ISP1763_HC_ATL_PTD_SKIPMAP	0xb2
#define ISP1763_HC_ATL_PTD_LASTPTD	0xb4

/* configuration registers */
#define ISP1763_HC_HW_MODE_CTRL		0xb6
#define ISP1763_HC_CHIP_REV		0x70
#define ISP1763_HC_CHIP_ID		0x72
#define ISP1763_HC_SCRATCH		0x78
#define ISP1763_HC_RESET		0xb8
#define ISP1763_HC_BUFFER_STATUS	0xba
#define ISP1763_HC_MEMORY		0xc4
#define ISP1763_HC_DATA			0xc6

/* interrupt registers */
#define ISP1763_HC_INTERRUPT		0xd4
#define ISP1763_HC_INTERRUPT_ENABLE	0xd6
#define ISP1763_HC_ISO_IRQ_MASK_OR	0xd8
#define ISP1763_HC_INT_IRQ_MASK_OR	0xda
#define ISP1763_HC_ATL_IRQ_MASK_OR	0xdc
#define ISP1763_HC_ISO_IRQ_MASK_AND	0xde
#define ISP1763_HC_INT_IRQ_MASK_AND	0xe0
#define ISP1763_HC_ATL_IRQ_MASK_AND	0xe2

/* OTG control: a write of a bit sets (0xe4) or clears (0xe6) it, a
 * read of either shows the state */
#define ISP1763_HC_OTG_CTRL_SET		0xe4
#define ISP1763_HC_OTG_CTRL_CLEAR	0xe6

/* the register fields the driver uses, rr_fields[] in isp1763.c */
enum rr_field {
	/* HC_PORTSC1 */
	PORT_OWNER, PORT_POWER, PORT_LSTATUS, PORT_RESET, PORT_SUSPEND,
	PORT_RESUME, PORT_PE, PORT_CSC, PORT_CONNECT,
	/* HC_USBCMD */
	CMD_LRESET, CMD_RESET, CMD_RUN,
	/* HC_USBSTS */
	STS_PCD,
	/* HC_FRINDEX */
	HC_FRINDEX,
	/* HC_CONFIGFLAG */
	FLAG_CF,
	/* ISO/INT/ATL PTD maps */
	HC_ISO_PTD_DONEMAP, HC_ISO_PTD_SKIPMAP, HC_ISO_PTD_LASTPTD,
	HC_INT_PTD_DONEMAP, HC_INT_PTD_SKIPMAP, HC_INT_PTD_LASTPTD,
	HC_ATL_PTD_DONEMAP, HC_ATL_PTD_SKIPMAP, HC_ATL_PTD_LASTPTD,
	/* HC_HW_MODE_CTRL */
	HW_DATA_BUS_WIDTH, HW_DACK_POL_HIGH, HW_DREQ_POL_HIGH, HW_INTF_LOCK,
	HW_INTR_HIGH_ACT, HW_INTR_EDGE_TRIG, HW_GLOBAL_INTR_EN,
	/* HC_CHIP_ID, HC_CHIP_REV */
	HC_CHIP_ID_HIGH, HC_CHIP_ID_LOW, HC_CHIP_REV,
	/* HC_SCRATCH */
	HC_SCRATCH,
	/* HC_RESET */
	SW_RESET_RESET_ATX, SW_RESET_RESET_ALL,
	/* HC_BUFFER_STATUS */
	ISO_BUF_FILL, INT_BUF_FILL, ATL_BUF_FILL,
	/* HC_MEMORY */
	MEM_START_ADDR,
	/* HC_INTERRUPT */
	HC_INTERRUPT,
	/* HC_INTERRUPT_ENABLE */
	HC_INT_IRQ_ENABLE, HC_ATL_IRQ_ENABLE,
	/* interrupt masks */
	HC_ISO_IRQ_MASK_OR, HC_INT_IRQ_MASK_OR, HC_ATL_IRQ_MASK_OR,
	HC_ISO_IRQ_MASK_AND, HC_INT_IRQ_MASK_AND, HC_ATL_IRQ_MASK_AND,
	/* HC_OTG_CTRL_SET */
	HW_HC_2_DIS, HW_OTG_DISABLE, HW_SW_SEL_HC_DC, HW_VBUS_DRV,
	HW_SEL_CP_EXT, HW_DM_PULLDOWN, HW_DP_PULLDOWN, HW_DP_PULLUP,
	/* HC_OTG_CTRL_CLEAR */
	HW_HC_2_DIS_CLEAR, HW_OTG_DISABLE_CLEAR, HW_SW_SEL_HC_DC_CLEAR,
	HW_VBUS_DRV_CLEAR, HW_SEL_CP_EXT_CLEAR, HW_DM_PULLDOWN_CLEAR,
	HW_DP_PULLDOWN_CLEAR, HW_DP_PULLUP_CLEAR,
	HC_FIELD_MAX
};

struct rr_fielddef {
	u8	reg;
	u8	shift;
	u16	mask;		/* in place */
};

/*
 * Chip memory: PTDs from 0x400 (ISO, INT, ATL: 16 slots of 32 bytes
 * each), payload from 0x1000: 8 blocks of 256 bytes, 2 of 1024, 4 of
 * 4096 (20 KB).
 */
#define RR_PTD_OFFSET		0x0400
#define RR_ISO_PTD_OFFSET	0x0400
#define RR_INT_PTD_OFFSET	0x0800
#define RR_ATL_PTD_OFFSET	0x0c00
#define RR_PAYLOAD_OFFSET	0x1000
#define RR_PAYLOAD_SIZE		0x5000
#define RR_SLOTS		16
#define RR_BLOCKS		(8 + 2 + 4)
#define RR_MAXXFER		4096	/* a transfer descriptor's most */

/* rrmod.c */
extern unsigned long rr_board;		/* the X-Surf-100's base, 0: none */
int rr_probe();				/* () 0 when the chip answers */
long rr_lowlevel_init();		/* () 0, or -1 */
void rr_lowlevel_stop();

#endif /* ISP1763_H */
