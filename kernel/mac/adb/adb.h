/*
 * adb.h -- Quadra 800 ADB: VIA1 shift-register transceiver, device
 * table, and the keyboard/mouse consumer interface.
 *
 * Built twice: for the kernel (AMIX cross compiler, -traditional) and on
 * the host with -DADB_HOST against a simulated VIA.
 */
#ifndef ADB_H
#define ADB_H

#define VOL	__volatile__

/* VIA1 registers, 0x200 apart */
#define V_ORB	0x0000
#define V_DDRB	0x0400
#define V_SR	0x1400
#define V_ACR	0x1600
#define V_IFR	0x1A00
#define V_IER	0x1C00

/* port B: PB3 = /ADB interrupt (input), PB4 = ST0, PB5 = ST1 */
#define PB_INT	0x08
#define PB_ST0	0x10
#define PB_ST1	0x20
#define PB_ST	(PB_ST0|PB_ST1)
#define ST_CMD	0x00
#define ST_EVEN	PB_ST0
#define ST_ODD	PB_ST1
#define ST_IDLE	PB_ST

/* ACR bits 4..2: 011 shift in, 111 shift out, both on the external clock */
#define ACR_SRMASK	0x1C
#define ACR_SRIN	0x0C
#define ACR_SROUT	0x1C

#define IFR_SR	0x04

#ifdef ADB_HOST
extern unsigned char sim_rd();
extern void sim_wr();
#define VRD(r)		sim_rd(r)
#define VWR(r, v)	sim_wr((r), (v))
#else
#define VIA1_BASE	0x50F00000
#define VRD(r)		(*(VOL unsigned char *)(VIA1_BASE + (r)))
#define VWR(r, v)	(*(VOL unsigned char *)(VIA1_BASE + (r)) = (v))
#endif

/* command byte */
#define ADB_RESET	0x00
#define ADB_FLUSH(a)	(((a) << 4) | 0x01)
#define ADB_LISTEN(a, r) (((a) << 4) | 0x08 | (r))
#define ADB_TALK(a, r)	(((a) << 4) | 0x0C | (r))
#define ADB_ADDR(c)	(((c) >> 4) & 0x0F)
#define ADB_ISTALK(c)	(((c) & 0x0C) == 0x0C)
#define ADB_ISLISTEN(c)	(((c) & 0x0C) == 0x08)
#define ADB_REG(c)	((c) & 0x03)

#define ADB_ADDR_KBD	2
#define ADB_ADDR_MOUSE	3

/* one request; the caller owns the storage */
struct adbreq {
	unsigned char	r_cmd;
	unsigned char	r_len;		/* listen: bytes to send */
	unsigned char	r_data[8];	/* listen data; talk reply */
	VOL int		r_n;		/* reply bytes, -1 no reply */
	VOL int		r_flags;
	void		(*r_done)();	/* (req), from adb_soft at IPL 1 */
};
#define RF_QUEUED	0x01
#define RF_DONE		0x02
#define RF_ABORT	0x04		/* watchdog or budget gave up */

/* device table, index = current address */
struct adbdev {
	unsigned char	d_orig;		/* default address = device class */
	unsigned char	d_handler;
};
extern struct adbdev adb_dev[16];
extern int adb_ndev, adb_ready, adb_polladdr;

/* bus layer */
void	adb_init();		/* io_init: reset, enumerate, start; never hangs */
void	adb_intr();		/* SR interrupt, IPL 4 */
void	adb_soft();		/* completions and device data, IPL 1 */
void	adb_tick();		/* every clock tick: watchdog, key repeat */
int	adb_queue();		/* (req) -> 0, or -1 when the queue is full */
int	adb_op_sync();		/* (req) -> reply bytes, -1 none, -2 stalled */

/*
 * Consumers.  Shapes follow the A/UX key/mouse layer that uinter sits on:
 *   key:   (*fn)(unit, KC_CHAR, code, more)   code = raw ADB key code,
 *          bit 7 = up; 0x7F twice (down, up) for the power key.
 *   mouse: (*fn)(unit, MOUSE_CHANGE, r0, changed)  r0 = talk-R0 word
 *          (button|dy << 8 | dx), changed: 1 button, 2 motion.
 * Installing a key consumer takes the keyboard away from the console.
 */
#define KC_CHAR		2
#define MOUSE_CHANGE	1

void	(*adb_keyhook())();	/* (fn) -> previous; 0 = console */
void	(*adb_mousehook())();	/* (fn) -> previous */
extern unsigned char adb_mouse_button;
extern short adb_mouse_x, adb_mouse_y;

/* console input sink: one byte (ASCII or part of an escape sequence) */
extern void (*adb_ttyin)();

/* device decoders */
void	adbkbd_input();		/* (addr, data, n) */
void	adbkbd_tick();
void	adbkbd_cons();		/* (b) console path, with repeat, for a consumer */
void	adbkbd_setleds();	/* (mask) LED_* */
void	adbms_input();		/* (addr, data, n) */
#define LED_NUM		0x01
#define LED_CAPS	0x02
#define LED_SCROLL	0x04

/* statistics */
extern unsigned long adb_nintr, adb_nspur, adb_ntmo, adb_nsrq, adb_nwdog, adb_nlost;

#ifndef ADB_HOST
extern int printf();
#endif

#endif
