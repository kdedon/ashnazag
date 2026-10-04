#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "migvideo.h"
static unsigned short r[256];
static unsigned char ram[4096];
static struct migframe frame;
static void word(unsigned long p, unsigned int value)
{
    ram[p] = value >> 8; ram[p + 1] = value;
}
static void setup(void)
{
    memset(r, 0, sizeof r); memset(ram, 0, sizeof ram);
    r[0x8e / 2] = 0x80f0; r[0x90 / 2] = 0x8200;
    r[0x92 / 2] = r[0x94 / 2] = 0x38;
    r[0x96 / 2] = 0x300; r[0x100 / 2] = 0x2000;
    r[0xe2 / 2] = 0x100; r[0xe6 / 2] = 0x200;
    r[0x182 / 2] = 0xf00; r[0x184 / 2] = 0x0f0; r[0x186 / 2] = 0x00f;
    ram[0x100] = 0x80; ram[0x200] = 0x40;
}
int main(void)
{
    setup();
    assert(mig_render(r, ram, sizeof ram, &frame) == 1);
    assert(frame.width == 16 && frame.height == 2);
    assert(frame.rgb[0] == 0xf00 && frame.rgb[1] == 0x0f0 && frame.rgb[2] == 0);

    setup();
    r[0x108 / 2] = 2; r[0x10a / 2] = 4;
    ram[0x104] = ram[0x206] = 0x80;
    assert(mig_render(r, ram, sizeof ram, &frame) == 1);
    assert(frame.rgb[16] == 0x00f);

    setup();
    r[0x96 / 2] |= 0x80; r[0x82 / 2] = 0x400;
    word(0x400, 0x8101); word(0x402, 0xfffe);
    word(0x404, 0x0182); word(0x406, 0x00ff);
    word(0x408, 0xffff); word(0x40a, 0xfffe);
    ram[0x102] = 0x80;
    assert(mig_render(r, ram, sizeof ram, &frame) == 1);
    assert(frame.rgb[0] == 0xf00 && frame.rgb[16] == 0x0ff);

    setup();
    r[0x100 / 2] = 0xa000;
    assert(mig_render(r, ram, sizeof ram, &frame) == 1);
    assert(frame.width == 32 && frame.rgb[0] == 0xf00);

    setup();
    r[0x100 / 2] = 0x6000;
    r[0xf6 / 2] = 0x300;
    ram[0x300] = 0x80;
    assert(mig_render(r, ram, sizeof ram, &frame) == 1);
    assert(frame.rgb[0] == 0x700);

    setup();
    r[0x100 / 2] = 0x6800;
    r[0xf2 / 2] = 0x300;
    ram[0x100] = 0xc0; ram[0x200] = 0;
    ram[0x300] = 0x40;
    assert(mig_render(r, ram, sizeof ram, &frame) == 1);
    assert(frame.rgb[0] == 0xf00 && frame.rgb[1] == 0xf01);

    setup();
    r[0x100 / 2] = 0x2400;
    ram[0x100] = ram[0x200] = 0x80;
    r[0x192 / 2] = 0xabc;
    assert(mig_render(r, ram, sizeof ram, &frame) == 1);
    assert(frame.rgb[0] == 0xf00);
    r[0x104 / 2] = 0x40;
    assert(mig_render(r, ram, sizeof ram, &frame) == 1);
    assert(frame.rgb[0] == 0xabc);

    setup();
    r[0x8e / 2] = 0x2c81; r[0x90 / 2] = 0xf4c1;
    r[0x100 / 2] = 0;
    assert(mig_render(r, ram, sizeof ram, &frame) == 1);
    assert(frame.width == 320 && frame.height == 200);
    r[0x90 / 2] = 0x2cc1;
    assert(mig_render(r, ram, sizeof ram, &frame) == 1);
    assert(frame.height == 256);

    setup(); r[0x96 / 2] = 0;
    assert(mig_render(r, ram, sizeof ram, &frame) == 1);
    assert(frame.rgb[0] == 0);
    setup(); r[0xe2 / 2] = 0x1000;
    assert(mig_render(r, ram, sizeof ram, &frame) == -1);
    setup(); r[0x96 / 2] |= 0x80; r[0x82 / 2] = 0x1000;
    assert(mig_render(r, ram, sizeof ram, &frame) == -1);
    setup(); r[0x96 / 2] |= 0x80; r[0x82 / 2] = 0x400;
    word(0x400, 0x0088); word(0x402, 0);
    assert(mig_render(r, ram, sizeof ram, &frame) == -1);
    setup(); r[0x100 / 2] |= 0x40;
    assert(mig_render(r, ram, sizeof ram, &frame) == -1);
    puts("[ok] Amiga display: planar pixels, EHB, HAM6, dual playfield, modulo, copper, geometry and bounds");
    return 0;
}
