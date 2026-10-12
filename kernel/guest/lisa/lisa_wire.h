#ifndef LISA_WIRE_H
#define LISA_WIRE_H
#define LISA_BITMAP 0x4000
#define LISA_CURSOR 0x18000
#define LISA_POLICY 0x180a0
#define LISA_KEYBOARD 0x180c0
#define LISA_INPUT 0x18100
#define LISA_BUSY 0x18600
#define LISA_AUDIO 0x186a0

static unsigned long
lisa_get32(p)
const unsigned char *p;
{
	return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
		((unsigned long)p[2] << 8) | p[3];
}

static void
lisa_put32(p, n)
unsigned char *p;
unsigned long n;
{
	p[0] = n >> 24;
	p[1] = n >> 16;
	p[2] = n >> 8;
	p[3] = n;
}

static void
lisa_barrier()
{
	__asm__ volatile ("" : : : "memory");
}
#endif
