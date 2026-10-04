#include <assert.h>
#include <stdio.h>
#include "amigadev.h"

static struct amigadev d;

static void
put(unsigned long address, int size, unsigned long value)
{
	assert(amigadev_write(&d, address, size, value) == 0);
}

static unsigned long
get(unsigned long address, int size)
{
	unsigned long value;
	assert(amigadev_read(&d, address, size, &value) == 0);
	return value;
}

static void
custom(void)
{
	static const unsigned short bits[] = { 1, 8, 0x10, 0x80, 0x800, 0x2000 };
	int i;
	amigadev_reset(&d);
	put(0xdff09a, 2, 0xffff);
	assert(get(0xdff01c, 2) == 0x7fff);
	for (i = 0; i < 6; i++) {
		put(0xdff09c, 2, 0x8000 | bits[i]);
		assert(amigadev_ipl(&d) == i + 1);
	}
	for (i = 5; i >= 0; i--) {
		put(0xdff09c, 2, bits[i]);
		assert(amigadev_ipl(&d) == i);
	}
	put(0xdff09c, 2, 0x8004);
	put(0xdff09a, 2, 0x4000);
	assert(amigadev_ipl(&d) == 0);
	assert(get(0xdff01e, 2) == 4);
	put(0xdff09a, 2, 0xc000);
	assert(amigadev_ipl(&d) == 1);
	put(0xdff096, 2, 0xffff);
	assert(get(0xdff002, 2) == 0x27ff);
	put(0xdff096, 2, 0x201);
	assert(get(0xdff002, 2) == 0x25fe);
	put(0xdff09e, 2, 0x8081);
	put(0xdff09e, 2, 1);
	assert(get(0xdff010, 2) == 0x80);
}

static void
timers(void)
{
	unsigned long base;
	int cia, timer;
	for (cia = 0; cia < 2; cia++) for (timer = 0; timer < 2; timer++) {
		amigadev_reset(&d);
		base = cia ? 0xbfd000 : 0xbfe001;
		put(0xdff09a, 2, 0xe008);
		put(base + 0xd00, 1, 0x80 | (1 << timer));
		put(base + 0x400 + timer * 0x200, 1, 2);
		put(base + 0x500 + timer * 0x200, 1, 0);
		put(base + 0xe00 + timer * 0x100, 1, 0x11);
		assert(get(base + 0xe00 + timer * 0x100, 1) == 1);
		amigadev_tick(&d, 2);
		assert(get(base + 0x400 + timer * 0x200, 1) == 0);
		assert(amigadev_ipl(&d) == 0);
		amigadev_tick(&d, 1);
		assert(amigadev_ipl(&d) == (cia ? 6 : 2));
		put(0xdff09c, 2, 0x3fff);
		assert(amigadev_ipl(&d) == (cia ? 6 : 2));
		assert(get(base + 0xd00, 1) == (unsigned long)(0x80 | (1 << timer)));
		assert(get(base + 0xd00, 1) == 0);
		put(0xdff09c, 2, 0x3fff);
		assert(amigadev_ipl(&d) == 0);
		amigadev_tick(&d, 3000001);
		assert(get(base + 0x400 + timer * 0x200, 1) == 1);
		put(base + 0xe00 + timer * 0x100, 1, 0x19);
		amigadev_tick(&d, 100);
		assert(get(base + 0xe00 + timer * 0x100, 1) == 8);
		assert(get(base + 0x400 + timer * 0x200, 1) == 2);
		put(base + 0x500 + timer * 0x200, 1, 0);
		assert(get(base + 0xe00 + timer * 0x100, 1) == 9);
	}
}

static void
mask_and_latch(void)
{
	amigadev_reset(&d);
	put(0xbfe401, 1, 0);
	put(0xbfe501, 1, 0);
	put(0xbfee01, 1, 1);
	amigadev_tick(&d, 1);
	assert(get(0xdff01e, 2) == 0);
	put(0xbfed01, 1, 0x81);
	put(0xbfed01, 1, 1);
	assert(get(0xbfed01, 1) == 0x81);
	assert(get(0xdff01e, 2) == 8);
	put(0xbfe401, 1, 0x34);
	put(0xbfe501, 1, 0x12);
	assert(get(0xbfe401, 1) == 0);
	put(0xbfee01, 1, 0x10);
	assert(get(0xbfe401, 1) == 0x34);
	assert(get(0xbfe501, 1) == 0x12);
}

static void
unsupported(void)
{
	unsigned long value = 0x1234;
	struct amigadev before;
	amigadev_reset(&d);
	before = d;
	assert(amigadev_read(&d, 0xdff005, 4, &value) == -1);
	assert(value == 0x1234);
	assert(amigadev_write(&d, 0xdff09a, 1, 0xff) == -1);
	assert(amigadev_write(&d, 0xbfe000, 1, 0xff) == -1);
	assert(d.intena == before.intena);
	assert(d.cia[0].control[0] == before.cia[0].control[0]);
	assert(d.cia[0].control[1] == before.cia[0].control[1]);
}

static void
isolation(void)
{
	struct amigadev other;
	int i, j;
	amigadev_reset(&d);
	amigadev_reset(&other);
	put(0xdff09a, 2, 0xffff);
	put(0xdff09c, 2, 0xffff);
	put(0xdff096, 2, 0xffff);
	put(0xdff09e, 2, 0xffff);
	put(0xbfed01, 1, 0x81);
	put(0xbfe401, 1, 0x34);
	put(0xbfe501, 1, 0x12);
	put(0xbfee01, 1, 0x11);
	amigadev_tick(&d, 0x1235);
	assert(d.cia[0].irq == 0x80);
	assert(amigadev_ipl(&other) == 0);
	assert(other.intena == 0 && other.intreq == 0);
	assert(other.dmacon == 0 && other.adkcon == 0);
	for (i = 0; i < 2; i++) {
		assert(other.cia[i].pending == 0);
		assert(other.cia[i].mask == 0 && other.cia[i].irq == 0);
		for (j = 0; j < 2; j++) {
			assert(other.cia[i].counter[j] == 0xffff);
			assert(other.cia[i].latch[j] == 0xffff);
			assert(other.cia[i].control[j] == 0);
		}
	}
}

static void
frames(void)
{
	unsigned long pos;
	amigadev_reset(&d);
	assert(get(0xdff004, 2) == 0x2200);
	assert(get(0xdff07c, 2) == 0xf8);
	assert(get(0xdff016, 2) & 0x400);
	pos = get(0xdff006, 2);
	assert(get(0xdff006, 2) != pos);
	put(0xdff09a, 2, 0xc020);
	amigadev_tick(&d, 709379);
	assert(d.frames == 50 && d.cia[0].tod == 50 && d.cia[1].tod == 15600);
	assert(amigadev_ipl(&d) == 3);
	put(0xdff058, 2, 0x41);
	assert(!(get(0xdff002, 2) & 0x4000));
	put(0xdff05b, 1, 0xf0);
	put(0xdff180, 2, 0xf42);
	assert(get(0xdff180, 2) == 0);
	amigadev_reset(&d);
	amigadev_configure(&d, 0);
	assert(get(0xdff004, 2) == 0x3200);
	amigadev_tick(&d, 715909);
	assert(d.frames == 60 && d.cia[0].tod == 60 && d.cia[1].tod == 15720);
}

static void
cia_boot(void)
{
	unsigned long latched;
	amigadev_reset(&d);
	assert(get(0xbfe001, 1) == 0xff);
	put(0xbfe001, 1, 0);
	put(0xbfe201, 1, 3);
	assert(get(0xbfe001, 1) == 0xfc);
	put(0xbfe001, 1, 1);
	assert(get(0xbfe001, 1) == 0xfd);
	put(0xbfdd00, 1, 0x84);
	put(0xbfdf00, 1, 0x80);
	put(0xbfd800, 1, 10);
	put(0xbfdf00, 1, 0);
	amigadev_tick(&d, 1000);
	assert(get(0xbfdd00, 1) == 0x84);
	latched = d.cia[1].tod;
	assert(get(0xbfda00, 1) == 0);
	amigadev_tick(&d, 1000);
	assert(get(0xbfd800, 1) == (latched & 0xff));
	assert(get(0xbfd800, 1) == (d.cia[1].tod & 0xff));
	put(0xbfda00, 1, 0);
	latched = d.cia[1].tod;
	amigadev_tick(&d, 1000);
	assert(d.cia[1].tod == latched);
	put(0xbfd900, 1, 0);
	put(0xbfd800, 1, 0);
	amigadev_tick(&d, 1000);
	assert(d.cia[1].tod > 0);
	put(0xbfe401, 1, 1);
	put(0xbfe501, 1, 0);
	put(0xbfe601, 1, 2);
	put(0xbfe701, 1, 0);
	put(0xbfee01, 1, 0x11);
	put(0xbfef01, 1, 0x51);
	amigadev_tick(&d, 5);
	assert(get(0xbfe601, 1) == 0);
	assert(get(0xbfed01, 1) == 1);
	amigadev_tick(&d, 1);
	assert(get(0xbfed01, 1) == 3);
	put(0xbfee01, 1, 0x51);
	put(0xbfec01, 1, 0x55);
	amigadev_tick(&d, 32);
	assert(get(0xbfed01, 1) & 8);
}

int
main(void)
{
	custom();
	timers();
	mask_and_latch();
	unsupported();
	isolation();
	frames();
	cia_boot();
	puts("amigadev: IRQs, timers, isolation, frames, CIA ports/TOD/chaining pass");
	return 0;
}
