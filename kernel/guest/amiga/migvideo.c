#include <string.h>
#include "migvideo.h"

struct copper {
    unsigned short r[256];
    const unsigned char *ram;
    unsigned long size, pc;
    int budget, stopped;
};
static unsigned long pointer(const unsigned short *r, int reg)
{
    return ((unsigned long)r[reg / 2] << 16 | r[reg / 2 + 1]) & 0x1ffffeUL;
}
static unsigned short word(const unsigned char *p)
{
    return (unsigned short)p[0] << 8 | p[1];
}

/* Copper waits are resolved at the end of each scanline. */
static int copper_line(struct copper *c, int y)
{
    unsigned int a, b, beam, mask, reg;
    while (!c->stopped && (c->r[0x96 / 2] & 0x280) == 0x280) {
        if (c->pc > c->size || c->size - c->pc < 4 || --c->budget < 0)
            return -1;
        a = word(c->ram + c->pc);
        b = word(c->ram + c->pc + 2);
        if (a == 0xffff && b == 0xfffe) { c->stopped = 1; break; }
        if (a & 1) {
            beam = ((y & 255) << 8) | 0xfe;
            mask = (b & 0x7ffe) | 0x8000;
            if (!(b & 0x8000) && (c->r[0x96 / 2] & 0x4000)) break;
            if (b & 1) {
                c->pc += (beam & mask) >= (a & mask) ? 8 : 4;
            } else {
                if ((beam & mask) < (a & mask)) break;
                c->pc += 4;
                if ((a & 0xfe) >= 0xde) break;
            }
            continue;
        }
        reg = a & 0x1fe;
        if (reg < 0x80) return -1;
        c->pc += 4;
        if (reg == 0x88 || reg == 0x8a) {
            c->pc = pointer(c->r, reg == 0x88 ? 0x80 : 0x84);
        } else if (reg == 0x96) {
            if (b & 0x8000) c->r[reg / 2] |= b & 0x7fff;
            else c->r[reg / 2] &= ~(b & 0x7fff);
        } else c->r[reg / 2] = b;
    }
    return 0;
}
static unsigned short color(struct copper *c, int index, unsigned short *held)
{
    int mode = c->r[0x100 / 2], odd, even, i;
    unsigned short rgb;
    if (mode & 0x800) {
        switch (index >> 4) {
        case 0: *held = c->r[0x180 / 2 + (index & 15)] & 0xfff; break;
        case 1: *held = (*held & 0xff0) | (index & 15); break;
        case 2: *held = (*held & 0x0ff) | ((index & 15) << 8); break;
        case 3: *held = (*held & 0xf0f) | ((index & 15) << 4); break;
        }
        return *held;
    }
    if (mode & 0x400) {
        odd = even = 0;
        for (i = 0; i < 3; i++) {
            odd |= ((index >> (2 * i)) & 1) << i;
            even |= ((index >> (2 * i + 1)) & 1) << i;
        }
        index = even && (!odd || (c->r[0x104 / 2] & 0x40)) ? even + 8 : odd;
    }
    rgb = c->r[0x180 / 2 + (index & 31)] & 0xfff;
    return index & 32 ? (rgb & 0xeee) >> 1 : rgb;
}
int mig_render(const unsigned short *registers, const unsigned char *ram,
    unsigned long size, struct migframe *frame)
{
    struct copper c;
    unsigned long p[6], address;
    unsigned int mode, start, stop, ddfstart, ddfstop;
    unsigned short held;
    int y, x, plane, n, width, height, first, last, rowbytes, scroll, bit, index;
    long next;
    memcpy(c.r, registers, sizeof c.r);
    c.ram = ram; c.size = size; c.budget = 16384; c.stopped = 0;
    c.pc = pointer(c.r, 0x80);
    frame->width = frame->height = 0;
    if (copper_line(&c, 0)) return -1;
    first = c.r[0x8e / 2] >> 8;
    last = c.r[0x90 / 2] >> 8;
    if (!(last & 0x80)) last |= 0x100;
    if (last <= first) return 0;
    height = last - first;
    if (height > MIG_HEIGHT) return -1;
    width = 0;
    for (y = 1; y < first; y++)
        if (copper_line(&c, y)) return -1;
    for (y = first; y < last; y++) {
        if (copper_line(&c, y)) return -1;
        mode = c.r[0x100 / 2];
        if (mode & 0x50) return -1;
        n = (mode >> 12) & 7;
        if (n > 6 || ((mode & 0x800) && n != 6)) return -1;
        start = c.r[0x8e / 2] & 255;
        stop = (c.r[0x90 / 2] & 255) | 256;
        if (stop <= start) return -1;
        x = (stop - start) * ((mode & 0x8000) ? 2 : 1);
        if (x > MIG_WIDTH || (width && x != width)) return -1;
        width = x;
        ddfstart = c.r[0x92 / 2] & 0xfc;
        ddfstop = c.r[0x94 / 2] & 0xfc;
        if (ddfstop < ddfstart) return -1;
        rowbytes = ((ddfstop - ddfstart) / 8 + 1) * ((mode & 0x8000) ? 4 : 2);
        for (plane = 0; plane < n; plane++) p[plane] = pointer(c.r, 0xe0 + plane * 4);
        held = c.r[0x180 / 2] & 0xfff;
        for (x = 0; x < width; x++) {
            index = 0;
            if ((c.r[0x96 / 2] & 0x300) == 0x300)
                for (plane = 0; plane < n; plane++) {
                    scroll = (c.r[0x102 / 2] >> ((plane & 1) ? 4 : 0)) & 15;
                    bit = x + scroll;
                    if (bit / 8 >= rowbytes) continue;
                    address = p[plane] + bit / 8;
                    if (address >= size) return -1;
                    index |= ((ram[address] >> (7 - (bit & 7))) & 1) << plane;
                }
            frame->rgb[(y - first) * width + x] = color(&c, index, &held);
        }
        if ((c.r[0x96 / 2] & 0x300) == 0x300)
            for (plane = 0; plane < n; plane++) {
                next = (long)p[plane] + rowbytes +
                    (short)c.r[((plane & 1) ? 0x10a : 0x108) / 2];
                if (next < 0 || (unsigned long)next > size) return -1;
                c.r[(0xe0 + plane * 4) / 2] = (unsigned long)next >> 16;
                c.r[(0xe2 + plane * 4) / 2] = next;
            }
    }
    frame->width = width; frame->height = height;
    return width ? 1 : 0;
}
