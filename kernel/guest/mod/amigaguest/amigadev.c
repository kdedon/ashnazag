#include "amigadev.h"

static unsigned short
setclear(unsigned short old, unsigned long value, unsigned short mask)
{
	if (value & 0x8000)
		return old | (value & mask);
	return old & ~(value & mask);
}

void
amigadev_reset(struct amigadev *d)
{
	int i, j;
	d->intena = d->intreq = d->dmacon = d->adkcon = 0;
	d->pal = 1;
	d->vbl_phase = d->frames = d->line = 0;
	for (i = 0; i < 2; i++) {
		d->cia[i].pending = d->cia[i].mask = d->cia[i].irq = 0;
		d->cia[i].serial = d->cia[i].serial_left = 0;
		d->cia[i].tod = d->cia[i].alarm = d->cia[i].tod_latch = 0;
		d->cia[i].tod_stopped = d->cia[i].tod_latched = 0;
		for (j = 0; j < 2; j++) {
			d->cia[i].latch[j] = d->cia[i].counter[j] = 0xffff;
			d->cia[i].control[j] = 0;
			d->cia[i].port[j] = d->cia[i].direction[j] = 0;
		}
	}
}

void
amigadev_configure(struct amigadev *d, int pal)
{
	d->pal = pal != 0;
	d->vbl_phase = 0;
}

static void
cia_irq(struct amigacia *c)
{
	if (c->pending & c->mask)
		c->irq = 0x80;
}

static void
sync_irq(struct amigadev *d)
{
	if (d->cia[0].irq)
		d->intreq |= 0x0008;
	if (d->cia[1].irq)
		d->intreq |= 0x2000;
}

int
amigadev_ipl(struct amigadev *d)
{
	unsigned short p;
	sync_irq(d);
	if (!(d->intena & 0x4000))
		return 0;
	p = d->intena & d->intreq;
	if (p & 0x2000) return 6;
	if (p & 0x1800) return 5;
	if (p & 0x0780) return 4;
	if (p & 0x0070) return 3;
	if (p & 0x0008) return 2;
	if (p & 0x0007) return 1;
	return 0;
}

/* Canonical byte lanes keep unrelated peripheral probes distinct. */
static int
cia_address(unsigned long address, int size, int *reg)
{
	if (size != 1)
		return -1;
	*reg = (int)((address >> 8) & 15);
	if ((address & ~0xf00UL) == 0xbfe001UL)
		return 0;
	if ((address & ~0xf00UL) == 0xbfd000UL)
		return 1;
	return -1;
}

int
amigadev_read(struct amigadev *d, unsigned long address, int size,
    unsigned long *value)
{
	int i, reg, timer;
	struct amigacia *c;
	unsigned long v;
	if (address >= 0xdff000UL && address < 0xdff200UL) {
		if ((size != 1 && size != 2 && size != 4) ||
		    (size != 1 && (address & 1)) || address + size > 0xdff200UL)
			return -1;
		if (size == 4) {
			unsigned long hi, lo;
			amigadev_read(d, address, 2, &hi);
			amigadev_read(d, address + 2, 2, &lo);
			*value = (hi << 16) | lo;
			return 0;
		}
		sync_irq(d);
		switch (address & 0x1fe) {
		/* the blitter is always idle */
		case 0x002: v = d->dmacon | 0x2000; break;
		case 0x004:
			v = (d->pal ? 0x2200 : 0x3200) | (d->line >> 8) |
			    ((d->frames & 1) ? 0x8000 : 0); break;
		/* each read sees the next line at a scattered horizontal
		 * position, so waits on either end */
		case 0x006:
			d->line = (d->line + 1) % (d->pal ? 312 : 262);
			v = d->line << 8 | d->line * 0x4f % 0xe3; break;
		case 0x008: v = 0; break;
		case 0x00a: case 0x00c: v = 0; break;
		case 0x00e: v = 0; break;
		case 0x010: v = d->adkcon; break;
		case 0x012: case 0x014: v = 0xffff; break;
		case 0x016: v = 0xff00; break;
		case 0x018: v = 0x3000; break;
		case 0x01a: v = 0; break;
		case 0x01c: v = d->intena; break;
		case 0x01e: v = d->intreq; break;
		case 0x07c: v = 0x00f8; break;
		default: v = 0; break;
		}
		if (size == 1) v = (address & 1) ? (v & 0xff) : (v >> 8);
		*value = v;
		return 0;
	}
	i = cia_address(address, size, &reg);
	if (i < 0) return -1;
	c = &d->cia[i];
	if (reg < 2) {
		v = (c->port[reg] & c->direction[reg]) | (~c->direction[reg] & 0xff);
	} else if (reg < 4) {
		v = c->direction[reg - 2];
	} else if (reg >= 4 && reg <= 7) {
		timer = (reg - 4) / 2;
		v = c->counter[timer];
		if (reg & 1) v >>= 8;
		v &= 0xff;
	} else if (reg >= 8 && reg <= 10) {
		if (reg == 10 && !c->tod_latched) {
			c->tod_latch = c->tod;
			c->tod_latched = 1;
		}
		v = ((c->tod_latched ? c->tod_latch : c->tod) >> ((reg - 8) * 8)) & 0xff;
		if (reg == 8) c->tod_latched = 0;
	} else if (reg == 11) {
		v = 0;
	} else if (reg == 12) {
		v = c->serial;
	} else if (reg == 13) {
		v = c->pending | c->irq;
		c->pending = c->irq = 0;
	} else if (reg >= 14) {
		v = c->control[reg - 14];
	} else return -1;
	*value = v;
	return 0;
}

int
amigadev_write(struct amigadev *d, unsigned long address, int size,
    unsigned long value)
{
	int i, reg, timer;
	struct amigacia *c;
	if (address == 0xdff05bUL && size == 1)
		return 0;
	if (address >= 0xdff000UL && address < 0xdff200UL) {
		if ((size != 2 && size != 4) || (address & 1) ||
		    address + size > 0xdff200UL) return -1;
		if (size == 4) {
			amigadev_write(d, address, 2, value >> 16);
			amigadev_write(d, address + 2, 2, value & 0xffff);
			return 0;
		}
		/* other registers drive the display, blitter, audio and disk: ignored */
		switch (address & 0x1fe) {
		case 0x096:
			d->dmacon = setclear(d->dmacon, value, 0x07ff); break;
		case 0x09a:
			d->intena = setclear(d->intena, value, 0x7fff); break;
		case 0x09c:
			d->intreq = setclear(d->intreq, value, 0x3fff);
			sync_irq(d); break;
		case 0x09e:
			d->adkcon = setclear(d->adkcon, value, 0x7fff); break;
		}
		return 0;
	}
	i = cia_address(address, size, &reg);
	if (i < 0) return -1;
	c = &d->cia[i];
	value &= 0xff;
	if (reg < 2) {
		c->port[reg] = value;
	} else if (reg < 4) {
		c->direction[reg - 2] = value;
	} else if (reg >= 4 && reg <= 7) {
		timer = (reg - 4) / 2;
		if (reg & 1) {
			c->latch[timer] = (c->latch[timer] & 0xff) | (value << 8);
			if (!(c->control[timer] & 1) || (c->control[timer] & 8))
				c->counter[timer] = c->latch[timer];
			if (c->control[timer] & 8) c->control[timer] |= 1;
		} else c->latch[timer] = (c->latch[timer] & 0xff00) | value;
	} else if (reg >= 8 && reg <= 10) {
		unsigned long shift = (reg - 8) * 8;
		unsigned long *target = (c->control[1] & 0x80) ? &c->alarm : &c->tod;
		*target = (*target & ~(0xffUL << shift)) | (value << shift);
		if (!(c->control[1] & 0x80)) {
			if (reg == 10) c->tod_stopped = 1;
			if (reg == 8) c->tod_stopped = 0;
		}
	} else if (reg == 11) {
		return 0;
	} else if (reg == 12) {
		c->serial = value;
		if (c->control[0] & 0x40) c->serial_left = 16;
	} else if (reg == 13) {
		if (value & 0x80) c->mask |= value & 0x1f;
		else c->mask &= ~(value & 0x1f);
		cia_irq(c);
	} else if (reg >= 14) {
		timer = reg - 14;
		if (value & 0x10) c->counter[timer] = c->latch[timer];
		c->control[timer] = value & ~0x10;
	} else return -1;
	sync_irq(d);
	return 0;
}

/* Division bounds work even when the caller advances many E-clock ticks. */
static unsigned long
timer_tick(struct amigacia *c, int timer, unsigned long ticks)
{
	unsigned long first, period, underflows;
	if (!(c->control[timer] & 1) || ticks == 0) return 0;
	first = (unsigned long)c->counter[timer] + 1;
	if (ticks < first) {
		c->counter[timer] -= ticks;
		return 0;
	}
	c->pending |= 1 << timer;
	cia_irq(c);
	c->counter[timer] = c->latch[timer];
	if (c->control[timer] & 8) {
		c->control[timer] &= ~1;
		return 1;
	}
	period = (unsigned long)c->latch[timer] + 1;
	underflows = 1 + (ticks - first) / period;
	c->counter[timer] -= (ticks - first) % period;
	return underflows;
}

static void
tod_tick(struct amigacia *c, unsigned long ticks)
{
	unsigned long distance;
	if (c->tod_stopped || !ticks) return;
	distance = (c->alarm - c->tod) & 0xffffffUL;
	if (!distance) distance = 0x1000000UL;
	if (ticks >= distance) {
		c->pending |= 4;
		cia_irq(c);
	}
	c->tod = (c->tod + ticks) & 0xffffffUL;
}

void
amigadev_tick(struct amigadev *d, unsigned long ticks)
{
	int i, mode;
	unsigned long ta, frequency, frames, lines, oldline;
	for (i = 0; i < 2; i++) {
		struct amigacia *c = &d->cia[i];
		ta = timer_tick(c, 0, (c->control[0] & 0x20) ? 0 : ticks);
		mode = (c->control[1] >> 5) & 3;
		timer_tick(c, 1, mode == 0 ? ticks : mode == 1 ? 0 : ta);
		if ((c->control[0] & 0x40) && c->serial_left) {
			if (ta >= c->serial_left) {
				c->serial_left = 0;
				c->pending |= 8;
				cia_irq(c);
			} else c->serial_left -= ta;
		}
	}
	/* VERTB and the TOD counters follow the host clock at the frame rate */
	frequency = d->pal ? 709379UL : 715909UL;
	lines = d->pal ? 312 : 262;
	oldline = d->vbl_phase * lines / frequency;
	d->vbl_phase += (ticks % frequency) * (d->pal ? 50 : 60);
	frames = (ticks / frequency) * (d->pal ? 50 : 60) + d->vbl_phase / frequency;
	d->vbl_phase %= frequency;
	d->frames += frames;
	if (frames) d->intreq |= 0x20;
	tod_tick(&d->cia[0], frames);
	tod_tick(&d->cia[1], frames * lines + d->vbl_phase * lines / frequency - oldline);
	sync_irq(d);
}
