#!/usr/bin/env python3
"""Exercise the production DMA teardown functions with host register mocks."""
import os
import pathlib
import resource
import subprocess
import sys
import tempfile

source = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1] / 'ds.c'
text = source.read_text()

def function(name):
    start = text.index('\n' + name + '(')
    start = text.rfind('\n', 0, start) + 1
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

fixture = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#define DS_ATARI 1
#define DS_NSESS 3
#define DS_PGSHIFT 12
struct dsvid {
    unsigned char v_reg[200], v_snd[80];
    unsigned long v_spb, v_spos;
};
struct dssess {
    unsigned long s_gtop, s_size, s_memsize;
    pid_t s_vpid;
    struct dsvid *s_vid;
    char *s_mem, *s_shadow;
    unsigned long *s_pfn;
};
static struct dssess ds_sess[DS_NSESS + 1], *ds_front;
static struct { unsigned long d_page; } ds_disp;
static unsigned long hardware;
static int saves, writes;
static void kmem_free(void *p, unsigned long n) { free(p); }
static void ds_sndsave(struct dssess *s) { assert(s->s_vid); saves++; }
static void ata_vput(unsigned long a, int size, unsigned long v) {
    int shift = a == 0xFF8201UL ? 16 : a == 0xFF8203UL ? 8 : 0;
    assert(size == 1);
    hardware = (hardware & ~(255UL << shift)) | ((v & 255) << shift);
    writes++;
}
static unsigned long shadow(struct dssess *s) {
    return (unsigned long)s->s_vid->v_reg[1] << 16 |
        (unsigned long)s->s_vid->v_reg[3] << 8 | s->s_vid->v_reg[13];
}
static void seed(struct dssess *s, struct dsvid *d, pid_t pid) {
    memset(s, 0, sizeof *s); memset(d, 0, sizeof *d);
    s->s_vpid = pid; s->s_gtop = 0x400000; s->s_vid = d;
    d->v_reg[1] = 0x12; d->v_reg[3] = 0x34; d->v_reg[13] = 0x56;
    d->v_spb = 0x123456; d->v_spos = 0x234567; d->v_snd[1] = 0x83;
}
'''
fixture += function('ds_unshadow') + '\n' + function('ds_ramdetach')
fixture += r'''
int main(void) {
    struct dsvid front, hidden, other;
    struct dssess *a = &ds_sess[1], *b = &ds_sess[2], *c = &ds_sess[3];
    ds_disp.d_page = 0x780000;
    seed(a, &front, 41); seed(b, &hidden, 41); seed(c, &other, 42);
    ds_front = a; hardware = shadow(a);
    ds_ramdetach(41);
    assert(hardware == ds_disp.d_page && saves == 1 && writes == 3);
    assert(shadow(a) == ds_disp.d_page && shadow(b) == ds_disp.d_page);
    assert(a->s_gtop == 0 && b->s_gtop == 0);
    assert(front.v_spb == 0 && front.v_spos == 0 && front.v_snd[1] == 0x80);
    assert(hidden.v_spb == 0 && hidden.v_spos == 0 && hidden.v_snd[1] == 0x80);
    assert(c->s_gtop == 0x400000 && shadow(c) == 0x123456);
    /* Refronting restores the cached base, which must now reference the pool. */
    ds_front = b; hardware = shadow(b);
    assert(hardware == ds_disp.d_page);
    ds_ramdetach(41);
    assert(saves == 1 && writes == 3);
    /* An ended slot can retain owner metadata after its video shadow is freed. */
    a->s_gtop = 0x400000; a->s_vid = 0;
    ds_ramdetach(41);
    assert(a->s_gtop == 0 && saves == 1 && writes == 3);
    c->s_memsize = c->s_size = 4096;
    c->s_mem = c->s_shadow = malloc(4096);
    c->s_pfn = malloc(4);
    c->s_vid = malloc(sizeof *c->s_vid);
    assert(c->s_mem && c->s_pfn && c->s_vid);
    ds_unshadow(c);
    assert(c->s_gtop == 0 && c->s_vid == 0 && c->s_mem == 0 && c->s_pfn == 0);
    ds_ramdetach(42);
    assert(saves == 1 && writes == 3);
    puts("PASS detach: active/hidden bases, refront, stale/end slots, owner isolation, repeated close");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ds-detach-') as tmp:
    tmp = pathlib.Path(tmp)
    (tmp / 'test.c').write_text(fixture)
    subprocess.run(['nice', '-n', '19', os.environ.get('CC', 'cc'), '-std=gnu89',
                    str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    subprocess.run([str(tmp / 'test')], check=True, timeout=10)
