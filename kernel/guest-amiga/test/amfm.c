/* MFM tracks: encoded, read back from any start and bit offset, damaged */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amfm.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* len bytes of the revolution rev, from bit start */
static void
readrev(const unsigned char *rev, long revlen, long start, unsigned char *out, long len)
{
    long i, b;

    memset(out, 0, len);
    for (i = 0; i < len * 8; i++) {
        b = (start + i) % (revlen * 8);
        if (rev[b >> 3] >> (7 - (b & 7)) & 1)
            out[i >> 3] |= 0x80 >> (i & 7);
    }
}

int
main(void)
{
    static unsigned char data[22 * 512], back[22 * 512], raw[32768], rev[25336], rd[32768], al[32768];
    long w, revlen, start;
    int nsec, t, n;

    srand(7);
    for (nsec = 11; nsec <= 22; nsec += 11) {
        revlen = AMFM_REV(nsec);
        for (t = 0; t < 8; t++) {
            int track = rand() % 160, i;
            for (i = 0; i < nsec * 512; i++)
                data[i] = rand();
            w = amfm_encode(track, nsec, data, raw);
            CHECK(w == AMFM_WLEN(nsec) && w > revlen);
            /* the write wraps: its tail overwrites the gap at its start */
            memcpy(rev, raw + (w - revlen), revlen);
            start = (long)(rand() % (revlen * 8));
            readrev(rev, revlen, start, rd, AMFM_RLEN(nsec));
            memset(back, 0, sizeof back);
            CHECK(amfm_decode(rd, AMFM_RLEN(nsec), track, nsec, back) == 0);
            CHECK(memcmp(back, data, nsec * 512) == 0);
            CHECK(amfm_decode(rd, AMFM_RLEN(nsec), track ^ 1, nsec, back) == AMFM_NOSECHDR);
            n = amfm_align(rd, AMFM_RLEN(nsec), al, 1000);
            CHECK(n == 1000 && al[0] == 0x44 && al[1] == 0x89);
            /* a data bit flipped in one sector: that sector's sum fails */
            rd[AMFM_RLEN(nsec) / 2] ^= 0x10;
            n = amfm_decode(rd, AMFM_RLEN(nsec), track, nsec, back);
            CHECK(n == 0 || n == AMFM_TOOFEWSECS);
        }
    }
    memset(rd, 0xaa, 20000);
    CHECK(amfm_decode(rd, 20000, 0, 11, back) == AMFM_NOSECHDR);
    CHECK(amfm_align(rd, 20000, al, 10) == -1);
    if (failures)
        return 1;
    printf("PASS amfm encode/decode at any start, align, errors\n");
    return 0;
}
