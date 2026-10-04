#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "amigadev.h"
#include "../../guest/include/amigaio.h"

#define _AMIGA_H
typedef char *caddr_t;
#define USTKCLEAR 1
struct { int u_sigflag; } u;
struct amigactr {
    struct amigadev ac_dev;
    struct amigaenter ac_config;
    unsigned long ac_epoch;
    unsigned char ac_gary[4];
    struct { unsigned long as_fault, as_lastpc, as_lastaddr; } ac_stat;
};
struct guest_proc { struct amigactr a; unsigned short gp_vpend; };
#define AMIGAP(gp) (&(gp)->a)

/* Fixed-width registers preserve target truncation on a 64-bit host. */
static struct registers {
    uint32_t d[8], a[7], usp, pc;
    uint16_t sr, fv;
} regs;
#define GR_D(r, n) (regs.d[n])
#define GR_A(r, n) (regs.a[n])
#define GR_USP(r) (regs.usp)
#define GR_PC(r) (regs.pc)
#define GR_SR(r) (regs.sr)
#define GR_FV(r) (regs.fv)
#define G16(p) get16((const unsigned char *)(p))
#define G32(p) get32((const unsigned char *)(p))
#define P16(p, v) put16((unsigned char *)(p), (v))

static unsigned char code[32], frame[128];
static unsigned int code_size;
static int tails, spldepth;
static unsigned long get16(const unsigned char *p)
{
    return (unsigned long)p[0] << 8 | p[1];
}
static unsigned long get32(const unsigned char *p)
{
    return get16(p) << 16 | get16(p + 2);
}
static void put16(unsigned char *p, unsigned long value)
{
    p[0] = value >> 8;
    p[1] = value;
}
static void put32(unsigned char *p, unsigned long value)
{
    put16(p, value >> 16);
    put16(p + 2, value);
}
static int copyin(char *from, char *to, unsigned int count)
{
    uintptr_t address = (uintptr_t)from;
    if (address < 0x1000 || address - 0x1000 > code_size ||
        count > code_size - (address - 0x1000))
        return -1;
    memcpy(to, code + address - 0x1000, count);
    return 0;
}
static int amiga_spl(void) { return spldepth++; }
static void amiga_splx(int s) { assert(spldepth == s + 1); spldepth = s; }
static void amiga_afterio(struct guest_proc *gp) { (void)gp; }
static void guest_trapret(void) { assert(spldepth == 0); tails++; }

#include "../../guest/mod/amigaguest/amigafault.c"

static void setup(struct guest_proc *gp, unsigned int op, unsigned long address, int read)
{
    memset(gp, 0, sizeof *gp);
    memset(&regs, 0, sizeof regs);
    memset(frame, 0, sizeof frame);
    memset(code, 0, sizeof code);
    amigadev_reset(&gp->a.ac_dev);
    gp->a.ac_config.ae_chipsize = AMIGA_CHIP_SIZE;
    regs.pc = 0x1000;
    regs.sr = 0x271f;
    regs.fv = 0x7008;
    put16(frame + 76, (read ? 0x100 : 0) | 1);
    put32(frame + 84, address);
    put16(code, op);
    code_size = 2;
    tails = spldepth = u.u_sigflag = 0;
}
static void absolute(unsigned int offset, unsigned long address)
{
    put32(code + offset, address);
    code_size = offset + 4;
}
static void accepted(struct guest_proc *gp)
{
    assert(amiga_fault(gp, (char *)frame, 2) == 0);
    assert(regs.pc == 0x1000 + code_size);
    assert(tails == 1 && u.u_sigflag == USTKCLEAR);
    assert(gp->a.ac_stat.as_fault == 1);
}
static void rejected(struct guest_proc *gp)
{
    struct registers before = regs;
    struct amigadev device = gp->a.ac_dev;
    unsigned long epoch = gp->a.ac_epoch;
    unsigned char oldframe[sizeof frame];
    int result;
    memcpy(oldframe, frame, sizeof frame);
    result = amiga_fault(gp, (char *)frame, 2);
    if (result != 1) fprintf(stderr, "unexpected recovery op=%04lx pc=%lx ssw=%04lx\n", get16(code), (unsigned long)before.pc, get16(frame + 76));
    assert(result == 1);
    assert(memcmp(&before, &regs, sizeof regs) == 0);
    assert(memcmp(&device, &gp->a.ac_dev, sizeof device) == 0);
    assert(memcmp(oldframe, frame, sizeof frame) == 0 && gp->a.ac_epoch == epoch);
    assert(tails == 0 && u.u_sigflag == 0 && spldepth == 0);
}
int main(void)
{
    struct guest_proc gp;
    int i;

    setup(&gp, 0x33fc, 0xdff09a, 0);
    put16(code + 2, 0xc020);
    absolute(4, 0xdff09a);
    accepted(&gp);
    assert(gp.a.ac_dev.intena == 0x4020 && regs.sr == 0x2718);

    setup(&gp, 0x3082, 0xdff09c, 0);
    regs.a[0] = 0xdff09c;
    regs.d[2] = 0xdead8020;
    accepted(&gp);
    assert(gp.a.ac_dev.intreq == 0x20 && regs.d[2] == 0xdead8020);
    assert(regs.a[0] == 0xdff09c && regs.sr == 0x2718);

    setup(&gp, 0x3639, 0xdff01c, 1);
    gp.a.ac_dev.intena = 0x1234;
    regs.d[3] = 0xaaaabbbb;
    absolute(2, 0xdff01c);
    accepted(&gp);
    assert(regs.d[3] == 0xaaaa1234 && regs.sr == 0x2710);

    setup(&gp, 0x1839, 0xbfe401, 1);
    gp.a.ac_dev.cia[0].counter[0] = 0x0080;
    regs.d[4] = 0xabcdef01;
    absolute(2, 0xbfe401);
    accepted(&gp);
    assert(regs.d[4] == 0xabcdef80 && regs.sr == 0x2718);

    setup(&gp, 0x1039, 0xbfe401, 1);
    gp.a.ac_dev.cia[0].counter[0] = 0;
    regs.d[0] = 0x12345678;
    absolute(2, 0xbfe401);
    accepted(&gp);
    assert(regs.d[0] == 0x12345600 && regs.sr == 0x2714);

    setup(&gp, 0x3279, 0xdff01c, 1);
    gp.a.ac_dev.intena = 0x8001;
    absolute(2, 0xdff01c);
    accepted(&gp);
    assert(regs.a[1] == 0xffff8001 && regs.sr == 0x271f);

    setup(&gp, 0x1039, 0xdff01c, 1);
    gp.a.ac_dev.intena = 0x1234;
    absolute(2, 0xdff01c);
    accepted(&gp);
    assert(regs.d[0] == 0x12);
    setup(&gp, 0x13fc, 0xdff09a, 0);
    put16(code + 2, 0xff);
    absolute(4, 0xdff09a);
    rejected(&gp);
    setup(&gp, 0x23fc, 0xdff09a, 0);
    put32(code + 2, 0xc0208020);
    absolute(6, 0xdff09a);
    accepted(&gp);
    assert(gp.a.ac_dev.intena == 0x4020 && gp.a.ac_dev.intreq == 0x20);
    setup(&gp, 0x3018, 0xdff01c, 1);
    regs.a[0] = 0xdff01c;
    accepted(&gp);
    assert(regs.a[0] == 0xdff01e);
    setup(&gp, 0x3018, 0xdff01c, 1);
    regs.a[0] = 0xdff001;
    rejected(&gp);

    for (i = 78; i <= 82; i += 2) {
        setup(&gp, 0x1039, 0xbfed01, 1);
        gp.a.ac_dev.cia[0].pending = 1;
        gp.a.ac_dev.cia[0].irq = 0x80;
        absolute(2, 0xbfed01);
        put16(frame + i, 0x80);
        rejected(&gp);
    }
    setup(&gp, 0x3039, 0xdff01c, 1);
    absolute(2, 0xdff01e);
    rejected(&gp);
    setup(&gp, 0x3039, 0xdff01c, 1);
    absolute(2, 0xdff01c);
    regs.fv = 0x2008;
    rejected(&gp);
    setup(&gp, 0x3039, 0xdff01c, 1);
    absolute(2, 0xdff01c);
    put16(frame + 76, 0x102);
    rejected(&gp);
    setup(&gp, 0x3039, 0xdff01c, 1);
    rejected(&gp);
    setup(&gp, 0x4e71, 0xdff09a, 0);
    put16(frame + 82, 0xc1); put32(frame + 104, 0xdff09a); put32(frame + 108, 0xc020);
    put16(frame + 80, 0xc1); put32(frame + 96, 0xdff09c); put32(frame + 100, 0x8020);
    assert(amiga_fault(&gp, (char *)frame, 2) == 0);
    assert(regs.pc == 0x1000 && gp.a.ac_dev.intena == 0x4020 && gp.a.ac_dev.intreq == 0x20);
    assert(!(get16(frame + 82) & 0x80) && !(get16(frame + 80) & 0x80));
    assert(tails == 1 && spldepth == 0);

    setup(&gp, 0x4e71, 0xdff09a, 0);
    put16(frame + 82, 0xc1); put32(frame + 104, 0xdff09a); put32(frame + 108, 0xc020);
    put16(frame + 80, 0xe1); put32(frame + 96, 0xdff09c); put32(frame + 100, 0x8020);
    rejected(&gp);

    setup(&gp, 0x48f9, 0xdff1fc, 0);
    put16(code + 2, 3); absolute(4, 0xdff1fc);
    regs.d[0] = 0x12345678; regs.d[1] = 0xdeadbeef;
    rejected(&gp);

    setup(&gp, 0x13f9, 0xbfed01, 1);
    gp.a.ac_dev.cia[0].pending = 1; gp.a.ac_dev.cia[0].irq = 0x80;
    absolute(2, 0xbfed01); absolute(6, 0xdead00);
    rejected(&gp);
    /* Brief indexed addressing and arithmetic preserve 32-bit guest flags. */
    setup(&gp, 0xd070, 0xdff01c, 1);
    regs.a[0] = 0xdff010; regs.d[1] = 4; regs.d[0] = 0x7fff;
    gp.a.ac_dev.intena = 1;
    put16(code + 2, 0x1204); code_size = 4;
    accepted(&gp);
    assert(regs.d[0] == 0x8000 && (regs.sr & 0x1f) == 0x0a);

    setup(&gp, 0x0c79, 0xdff01c, 1);
    gp.a.ac_dev.intena = 0x1234;
    put16(code + 2, 0x1234); absolute(4, 0xdff01c);
    accepted(&gp);
    assert((regs.sr & 0x1f) == 0x14);

    setup(&gp, 0x0839, 0xbfe001, 1);
    put16(code + 2, 0); absolute(4, 0xbfe001);
    accepted(&gp);
    assert(!(regs.sr & 4));

    setup(&gp, 0x48f9, 0xdff09a, 0);
    put16(code + 2, 3); absolute(4, 0xdff09a);
    regs.d[0] = 0xc0208020; regs.d[1] = 0;
    accepted(&gp);
    assert(gp.a.ac_dev.intena == 0x4020 && gp.a.ac_dev.intreq == 0x20);

    setup(&gp, 0x4cd8, 0xdff01c, 1);
    put16(code + 2, 3); code_size = 4;
    regs.a[0] = 0xdff01c; gp.a.ac_dev.intena = 0x1234;
    accepted(&gp);
    assert(regs.a[0] == 0xdff024 && regs.d[0] == 0x12340000);

    setup(&gp, 0x3039, 0x200000, 1);
    absolute(2, 0x200000);
    accepted(&gp);
    assert(regs.d[0] == 0xffff);

    setup(&gp, 0x13fc, 0xde0001, 0);
    put16(code + 2, 0x34); absolute(4, 0xde0001);
    accepted(&gp);
    assert(gp.a.ac_gary[1] == 0x34);

    /* WB1 bytes follow address-selected bus lanes. */
    for (i = 0; i < 4; i++) {
        setup(&gp, 0x4e71, 0xde0000 + i, 0);
        put16(frame + 82, 0xa1);
        put32(frame + 104, 0xde0000 + i);
        put32(frame + 108, 0x12345678);
        assert(amiga_fault(&gp, (char *)frame, 2) == 0);
        assert(gp.a.ac_gary[i] == (0x12345678UL >> (24 - i * 8) & 0xff));
        assert(regs.pc == 0x1000 && tails == 1);
    }
    setup(&gp, 0x4e71, 0xde0000, 0);
    put16(frame + 82, 0xc1); put32(frame + 104, 0xde0000);
    put32(frame + 108, 0x12345678);
    assert(amiga_fault(&gp, (char *)frame, 2) == 0);
    assert(gp.a.ac_gary[0] == 0x12 && gp.a.ac_gary[1] == 0x34);

    setup(&gp, 0x4e71, 0xdff09a, 0);
    put16(frame + 82, 0xc2); put32(frame + 104, 0xdff09a);
    put32(frame + 108, 0xc020);
    rejected(&gp);

    puts("[ok] Amiga fault decoder: MOVE, arithmetic, MOVEM, probes, writebacks, and rollback");
    return 0;
}
