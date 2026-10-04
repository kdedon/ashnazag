#include "amigablit.h"

static unsigned short
logic(unsigned short a, unsigned short b, unsigned short c, unsigned int op)
{
	unsigned int i, out = 0;
	for (i = 0; i < 8; i++)
		if (op & (1U << i))
			out |= (i & 4 ? a : ~a) & (i & 2 ? b : ~b) & (i & 1 ? c : ~c);
	return (unsigned short)out;
}

static unsigned short
shift(unsigned short value, unsigned short prev, int n, int desc)
{
	unsigned long pair;
	if (!n) return value;
	pair = desc ? ((unsigned long)value << 16) | prev :
	    ((unsigned long)prev << 16) | value;
	return (unsigned short)(pair >> (desc ? 16 - n : n));
}

static unsigned short
fill(unsigned short value, int exclusive, int *carry)
{
	unsigned int b, out = 0;
	for (b = 1; b <= 0x8000; b <<= 1) {
		int bit = (value & b) != 0;
		if (exclusive ? bit ^ *carry : bit | *carry) out |= b;
		*carry ^= bit;
	}
	return (unsigned short)out;
}

/* Check every row before any destination writes. */
static int
span(long p, long stride, unsigned long width, unsigned long height,
    unsigned long chipsize, int desc)
{
	unsigned long y;
	long last;
	for (y = 0; y < height; y++, p += stride) {
		last = p + (desc ? -1L : 1L) * (long)(width - 1) * 2;
		if (p < 0 || last < 0 || (unsigned long)p > chipsize - 2 ||
		    (unsigned long)last > chipsize - 2) return -1;
	}
	return 0;
}

int
amigablit_run(r, ecs, chipsize, read16, write16, opaque, zero)
	unsigned short *r;
	int ecs;
	unsigned long chipsize;
	int (*read16)(void *, unsigned long, unsigned short *);
	int (*write16)(void *, unsigned long, unsigned short);
	void *opaque;
	int *zero;
{
	static int ptr[4] = { 0x50 / 2, 0x4c / 2, 0x48 / 2, 0x54 / 2 };
	static int mod[4] = { 0x64 / 2, 0x62 / 2, 0x60 / 2, 0x66 / 2 };
	static int data[3] = { 0x74 / 2, 0x72 / 2, 0x70 / 2 };
	long p[4], stride[4];
	unsigned long width, height, x, y;
	unsigned short raw[3], a, b, out, aprev = 0, bprev = 0;
	unsigned int con0 = r[0x40 / 2], con1 = r[0x42 / 2];
	int use[4], desc = (con1 & 2) != 0, i, carry, allzero = 1;
	if (chipsize < 2 || chipsize > 0x200000UL || !read16 || !write16 || !zero)
		return -1;
	if ((con1 & 0x0fe1) || ((con1 & 0x18) && !desc) || (con1 & 0x18) == 0x18)
		return -1;
	width = ecs ? r[0x5e / 2] & 0x7ff : r[0x58 / 2] & 63;
	height = ecs ? r[0x5c / 2] & 0x7fff : r[0x58 / 2] >> 6;
	if (!width) width = ecs ? 2048 : 64;
	if (!height) height = ecs ? 32768 : 1024;
	if (width * height > 0x100000UL) return -1;
	for (i = 0; i < 4; i++) {
		use[i] = (con0 & (0x800 >> i)) != 0;
		p[i] = (long)(((unsigned long)r[ptr[i]] << 16) | (r[ptr[i] + 1] & 0xfffe));
		stride[i] = (long)width * 2 + (short)(r[mod[i]] & 0xfffe);
		if (desc) stride[i] = -stride[i];
		if (use[i] && span(p[i], stride[i], width, height, chipsize, desc)) return -1;
	}
	for (i = 0; i < 3; i++) raw[i] = r[data[i]];
	for (y = 0; y < height; y++) {
		carry = (con1 & 4) != 0;
		for (x = 0; x < width; x++) {
			for (i = 0; i < 3; i++)
				if (use[i] && read16(opaque, (unsigned long)p[i], &raw[i])) return -2;
			a = raw[0];
			if (x == 0) a &= r[0x44 / 2];
			if (x + 1 == width) a &= r[0x46 / 2];
			b = shift(raw[1], use[1] ? bprev : 0, con1 >> 12, desc);
			out = logic(shift(a, aprev, con0 >> 12, desc), b, raw[2], con0 & 255);
			aprev = a; bprev = raw[1];
			if (con1 & 0x18) out = fill(out, (con1 & 0x10) != 0, &carry);
			if (out) allzero = 0;
			if (use[3] && write16(opaque, (unsigned long)p[3], out)) return -2;
			for (i = 0; i < 4; i++)
				if (use[i]) p[i] += desc ? -2 : 2;
		}
		for (i = 0; i < 4; i++)
			if (use[i]) p[i] += desc ? -(long)(short)(r[mod[i]] & 0xfffe) :
			    (long)(short)(r[mod[i]] & 0xfffe);
	}
	for (i = 0; i < 4; i++)
		if (use[i]) {
			r[ptr[i]] = (unsigned short)((unsigned long)p[i] >> 16);
			r[ptr[i] + 1] = (unsigned short)p[i];
		}
	for (i = 0; i < 3; i++) if (use[i]) r[data[i]] = raw[i];
	*zero = allzero;
	return 0;
}
