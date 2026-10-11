#ifndef AMIGADEV_H
#define AMIGADEV_H

struct amigacia {
	unsigned short latch[2], counter[2];
	unsigned char control[2], pending, mask, irq;
	unsigned char port[2], direction[2], serial, serial_left;
	unsigned long tod, alarm, tod_latch;
	unsigned char tod_stopped, tod_latched;
};

/* the A3000 board's SCSI DMA chip and its WD33C93, with no devices on the bus */
struct amigascsi {
	unsigned char on, cntr, sasr, irq, reg[32];
};

struct amigadev {
	unsigned short intena, intreq, dmacon, adkcon, line;
	unsigned long vbl_phase, frames;
	unsigned char pal;
	struct amigacia cia[2];
	struct amigascsi scsi;
};

#define AMIGA_NOTHER 32
/* register accesses, counted with AMIGAF_CENSUS and logged at exit */
struct amigacensus {
	unsigned long custom[256][2], cia[2][16][2];
	unsigned long other[AMIGA_NOTHER][3];	/* address, count, first PC */
	unsigned long nfmt, nssw, nfail, nchip, nfastram, low[16];	/* faults left to the kernel, by reason */
	unsigned long miss[AMIGA_NOTHER][3];	/* PC, count, address of decoder misses */
	unsigned long priv[AMIGA_NOTHER][3];	/* PC, count, opcode of privileged traps */
	unsigned long blit[AMIGA_NOTHER][2];	/* PC, count of blitter starts */
};

void amigadev_reset(struct amigadev *);
void amigadev_configure(struct amigadev *, int, int);
int amigadev_read(struct amigadev *, unsigned long, int, unsigned long *);
int amigadev_write(struct amigadev *, unsigned long, int, unsigned long);
void amigadev_tick(struct amigadev *, unsigned long);
int amigadev_ipl(struct amigadev *);

#endif
