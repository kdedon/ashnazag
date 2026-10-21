/*
 * Code loaded from disk gets the Kickstart's patches: dos.library's
 * LoadSeg and NewLoadSeg patch each segment list they return.
 */
#include "../hostcall.h"
typedef unsigned int U;
typedef unsigned short W;
typedef unsigned char B;

extern void seg_stub(void);
extern U xcall(void *, void *, U *);

static B *execbase(void) { B *b; __asm__("move.l 4.w,%0" : "=a"(b)); return b; }
static U exec(int lvo, U d0, U d1, U a0, U a1)
{
    U r[14];
    int i;
    for (i = 0; i < 14; i++) r[i] = 0;
    r[0] = d0; r[1] = d1; r[8] = a0; r[9] = a1;
    return xcall(execbase(), execbase() + lvo, r);
}

/* the patches in a segment list; the caches are pushed if any were made */
void pv_seg(U seg)
{
    const W *t = (const W *)(MIG_PVSEG + 4), *e;
    W *p, *end;
    U n = 0, k;
    if (*(const U *)MIG_PVSEG != MIG_PVSEG_MAGIC)
        return;
    for (; seg; seg = *(U *)(seg << 2)) {
        p = (W *)((seg << 2) + 4);
        end = (W *)((B *)p + *(U *)((seg << 2) - 4) - 8);
        for (; p < end; p++)
            for (e = t; *e; e += 1 + 2 * *e) {
                if (*p != e[1] || p + *e > end)
                    continue;
                for (k = 1; k < *e && p[k] == e[1 + k]; k++)
                    ;
                if (k < *e)
                    continue;
                for (k = 0; k < *e; k++)
                    p[k] = e[1 + *e + k];
                p += *e - 1;
                n++;
                break;
            }
    }
    if (n)
        exec(-636, 0, 0, 0, 0);
}

/* LoadSeg and NewLoadSeg through seg_stub, from trampolines naming the original */
void pv_hook(B *dos)
{
    static const short lvo[] = { -150, -768 };
    W *t;
    U i, k, r[14];
    if (*(const U *)MIG_PVSEG != MIG_PVSEG_MAGIC)
        return;
    if (!(t = (W *)exec(-198, 12 * 2, 0x10001, 0, 0)))
        return;
    exec(-132, 0, 0, 0, 0);
    for (i = 0; i < 2; i++, t += 6) {
        t[0] = 0x2f3c;
        *(U *)&t[1] = *(U *)(dos + lvo[i] + 2);
        t[3] = 0x4ef9;
        *(U *)&t[4] = (U)seg_stub;
    }
    exec(-636, 0, 0, 0, 0);
    for (t -= 12, i = 0; i < 2; i++, t += 6) {
        for (k = 0; k < 14; k++) r[k] = 0;
        r[9] = (U)dos; r[8] = (U)(int)lvo[i]; r[0] = (U)t;
        xcall(execbase(), execbase() - 420, r);
    }
    exec(-138, 0, 0, 0, 0);
}

/* [original][return]: the original returns here, then the list is patched */
__asm__(
"	.globl seg_stub\n"
"seg_stub:\n"
"	move.l (%sp),-(%sp)\n"
"	move.l #1f,4(%sp)\n"
"	rts\n"
"1:	tst.l %d0\n"
"	beq.s 2f\n"
"	movem.l %d0-%d1/%a0-%a1,-(%sp)\n"
"	move.l %d0,-(%sp)\n"
"	jsr pv_seg\n"
"	addq.l #4,%sp\n"
"	movem.l (%sp)+,%d0-%d1/%a0-%a1\n"
"2:	rts\n");
