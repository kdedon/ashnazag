#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "dsio.h"
#include "inputshare.h"
static struct mig_input q;
static struct mig_input_state s;
static int emit(unsigned int type, unsigned int code, long value)
{
    return mig_input_event(&q, &s, EVK_ADB, 0, type, code, value);
}
int main(void)
{
    unsigned int i;
    assert(sizeof(unsigned int) == 4);
    assert(sizeof(struct mig_input_event) == 12);
    assert(offsetof(struct mig_input, event) == 32);
    assert(sizeof q <= MIG_INPUT_MAP_SIZE);
    mig_input_init(&q);
    assert(!emit(IE_KEY, 0, 1) && q.head == 0);
    q.ready = 1;
    assert(emit(IE_KEY, 0, 1));
    assert(q.event[0].type == 1 && q.event[0].code == 0x20);
    assert(!emit(IE_KEY, 0, 1));
    assert(emit(IE_KEY, 0, 0) && q.event[1].code == 0xa0);
    assert(emit(IE_KEY, 0x38, 1) && q.event[2].qualifier == 1);
    assert(emit(IE_KEY, 0x38, 0) && q.event[3].qualifier == 0);
    assert(emit(IE_BTN, 1, 1) && q.event[4].code == 0x68);
    assert(q.event[4].qualifier == 0xc000);
    assert(emit(IE_REL, IE_RELX, -99999) && q.event[5].x == -32768);
    assert(q.event[5].qualifier == 0xc000);
    assert(emit(IE_BTN, 1, 0) && q.event[6].code == 0xe8);
    assert(!emit(IE_BTN, 4, 1) && !emit(IE_KEY, 128, 1));
    assert(mig_input_translate(EVK_IKBD, 0x1e) == 0x20);
    assert(mig_input_translate(EVK_AMIGA, 0) == 0);
    assert(mig_input_translate(99, 0) == -1);
    assert(emit(IE_KEY, 0x52, 1) && (q.event[7].qualifier & 0x100));
    mig_input_reset(&q, &s);
    assert(q.reset == 1 && !s.qualifier && !emit(IE_KEY, 0, 1));
    mig_input_reset(&q, &s);
    assert(q.reset == 1);
    q.tail = q.head; q.ack = q.reset;
    for (i = 0; i < MIG_INPUT_COUNT; i++) assert(emit(IE_REL, IE_RELY, 1));
    assert(!emit(IE_KEY, 0, 1) && q.reset == 2 && !s.keys[0x20]);
    q.tail = q.head; q.ack = q.reset;
    q.head = q.tail = 0xfffffffeU;
    assert(emit(IE_REL, IE_RELX, 1));
    assert(emit(IE_REL, IE_RELX, 2));
    assert(q.head == 0);
    assert(q.event[254].x == 1 && q.event[255].x == 2);
    assert(emit(IE_KEY, 0, 1));
    q.generation++;
    assert(emit(IE_KEY, 0, 1));
    assert(!emit(IE_DROP, 0, 0) && q.reset == 3);
    q.tail = q.head; q.ack = q.reset;
    assert(emit(IE_KEY, 0x36, 1));
    assert(!emit(IE_KEY, 0x7d, 1));
    assert(!emit(IE_KEY, 0x36, 0));
    assert(s.qualifier & 8);
    assert(emit(IE_KEY, 0x7d, 0) && !(s.qualifier & 8));
    puts("[ok] input translation, qualifiers, focus reset, overflow, wrap, restart");
    return 0;
}
