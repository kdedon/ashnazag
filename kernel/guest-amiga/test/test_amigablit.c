#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "amigablit.h"

static unsigned short regs[256], mem[256];
static int reads, writes, zero, read_error, write_error;

static int rd(void *opaque, unsigned long address, unsigned short *out)
{
	(void)opaque;
	assert(address < sizeof mem && !(address & 1));
	if (read_error) return -1;
	*out = mem[address / 2]; reads++;
	return 0;
}
static int wr(void *opaque, unsigned long address, unsigned short value)
{
	(void)opaque;
	assert(address < sizeof mem && !(address & 1));
	if (write_error) return -1;
	mem[address / 2] = value; writes++;
	return 0;
}
static void setup(unsigned int con0, unsigned int con1, unsigned int size)
{
	memset(regs, 0, sizeof regs); memset(mem, 0, sizeof mem);
	regs[0x40/2] = con0; regs[0x42/2] = con1; regs[0x58/2] = size;
	regs[0x44/2] = regs[0x46/2] = 0xffff;
	regs[0x56/2] = 128;
	reads = writes = 0; zero = -1;
	read_error = write_error = 0;
}
static int run(void)
{
	return amigablit_run(regs, 0, sizeof mem, rd, wr, 0, &zero);
}
static void minterms(void)
{
	unsigned int op, bit, want;
	for (op = 0; op < 256; op++) {
		setup(0x100 | op, 0, 65);
		regs[0x74/2] = 0xf0f0; regs[0x72/2] = 0xcccc; regs[0x70/2] = 0xaaaa;
		want = 0;
		for (bit = 0; bit < 16; bit++) {
			unsigned int index = ((0xf0f0U >> bit) & 1) * 4 +
			    ((0xccccU >> bit) & 1) * 2 + ((0xaaaaU >> bit) & 1);
			want |= ((op >> index) & 1) << bit;
		}
		assert(run() == 0 && mem[64] == want && writes == 1 && reads == 0);
		assert(zero == (want == 0));
	}
}
static void copy_and_mask(void)
{
	setup(0x9f0, 0, 2*64 + 2);
	regs[0x64/2] = 2; regs[0x66/2] = 4;
	mem[0] = 1; mem[1] = 2; mem[3] = 3; mem[4] = 4;
	assert(run() == 0);
	assert(mem[64] == 1 && mem[65] == 2 && mem[68] == 3 && mem[69] == 4);
	assert(regs[0x52/2] == 12 && regs[0x56/2] == 144);
	setup(0x9f0, 2, 67);
	regs[0x52/2] = 4; regs[0x56/2] = 6;
	mem[0] = 1; mem[1] = 2; mem[2] = 3;
	assert(run() == 0 && mem[1] == 1 && mem[2] == 2 && mem[3] == 3);
	setup(0x9f0, 0, 65); mem[0] = 0xffff;
	regs[0x44/2] = 0xff00; regs[0x46/2] = 0x0ff0;
	assert(run() == 0 && mem[64] == 0x0f00);
}
static void shifts_and_fill(void)
{
	setup(0x49f0, 0, 2*64 + 1); mem[0] = 0x1234; mem[1] = 0x5678;
	assert(run() == 0 && mem[64] == 0x0123 && mem[65] == 0x4567);
	setup(0x49f0, 2, 66); regs[0x52/2] = 2; regs[0x56/2] = 130;
	mem[1] = 0x1234; mem[0] = 0x5678;
	assert(run() == 0 && mem[65] == 0x2340 && mem[64] == 0x6781);
	setup(0x5cc, 0x4000, 66); mem[0] = 0x1234; mem[1] = 0x5678;
	assert(run() == 0 && mem[64] == 0x0123 && mem[65] == 0x4567);
	setup(0x1cc, 0x4000, 66); regs[0x72/2] = 0x1234;
	assert(run() == 0 && mem[64] == 0x0123 && mem[65] == 0x0123);
	setup(0x9f0, 0x0a, 65); mem[0] = 0x2418;
	assert(run() == 0 && mem[64] == 0x3c18);
	setup(0x9f0, 0x12, 65); mem[0] = 0x2418;
	assert(run() == 0 && mem[64] == 0x1c08);
	setup(0x9f0, 0x0e, 65); mem[0] = 0x2418;
	assert(run() == 0 && mem[64] == 0xe7ff);
}
static void limits(void)
{
	setup(0x9f0, 0, 2*64 + 1); regs[0x64/2] = 510;
	assert(run() == -1 && writes == 0 && reads == 0 && zero == -1);
	setup(0x9f0, 1, 65);
	assert(run() == -1 && writes == 0);
	setup(0x9f0, 8, 65);
	assert(run() == -1 && writes == 0);
	setup(0x9f0, 0, 65); regs[0x56/2] = sizeof mem;
	assert(run() == -1 && writes == 0 && reads == 0);
	setup(0x9f0, 0, 65); regs[0x5c/2] = 2; regs[0x5e/2] = 3;
	mem[5] = 0xbeef;
	assert(amigablit_run(regs, 1, sizeof mem, rd, wr, 0, &zero) == 0);
	assert(writes == 6 && mem[69] == 0xbeef);
	setup(0x100, 0, 64);
	assert(run() == 0 && writes == 64 && zero == 1);
}
static void modulo_and_failures(void)
{
	setup(0x9f0, 0, 130);
	regs[0x52/2] = 4; regs[0x64/2] = 0xfffa;
	mem[1] = 1; mem[2] = 2; mem[3] = 3;
	assert(run() == 0);
	assert(mem[64] == 2 && mem[65] == 3 && mem[66] == 1 && mem[67] == 2);
	assert(regs[0x52/2] == 0);
	setup(0x9f0, 0, 130); regs[0x64/2] = 0xfffa;
	assert(run() == -1 && reads == 0 && writes == 0);
	setup(0x9f0, 0, 65); read_error = 1;
	assert(run() == -2 && writes == 0 && zero == -1);
	assert(regs[0x52/2] == 0 && regs[0x56/2] == 128);
	setup(0x9f0, 0, 65); write_error = 1;
	assert(run() == -2 && reads == 1 && writes == 0 && zero == -1);
	assert(regs[0x52/2] == 0 && regs[0x56/2] == 128);
	setup(0, 0, 0);
	assert(run() == 0 && reads == 0 && writes == 0 && zero == 1);
	setup(0x100, 0, 0);
	assert(run() == -1 && reads == 0 && writes == 0);
	setup(0x100, 0, 65);
	assert(amigablit_run(regs, 1, sizeof mem, rd, wr, 0, &zero) == -1);
	assert(writes == 0 && reads == 0);
}
int main(void)
{
	minterms(); copy_and_mask(); shifts_and_fill(); limits(); modulo_and_failures();
	puts("amigablit: minterms, copy, masks, shifts, fill, ECS, modulo, errors, bounds pass");
	return 0;
}
