#ifndef AMIGADEV_H
#define AMIGADEV_H

struct amigacia {
	unsigned short latch[2], counter[2];
	unsigned char control[2], pending, mask, irq;
	unsigned char port[2], direction[2], serial, serial_left;
	unsigned long tod, alarm, tod_latch;
	unsigned char tod_stopped, tod_latched;
};

struct amigadev {
	unsigned short intena, intreq, dmacon, adkcon;
	unsigned short custom[256];
	unsigned long palette[256], frame_phase, frames;
	unsigned char pal, blit_pending, blit_zero;
	struct amigacia cia[2];
};

void amigadev_reset(struct amigadev *);
void amigadev_configure(struct amigadev *, int);
int amigadev_read(struct amigadev *, unsigned long, int, unsigned long *);
int amigadev_write(struct amigadev *, unsigned long, int, unsigned long);
void amigadev_tick(struct amigadev *, unsigned long);
int amigadev_ipl(struct amigadev *);

#endif
