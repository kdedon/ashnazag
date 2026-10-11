/*
 * amfm.c -- AmigaDOS MFM tracks.  Each field is split into its odd
 * bits, then its even bits; a clock bit is 1 between two 0 data bits.
 * A raw read starts anywhere, so sectors are found at any bit offset.
 */
#include <string.h>
#include "amfm.h"

#define M 0x55555555UL
#define SYNC2 0x44894489UL

static unsigned long
get32(const unsigned char *b, long p)
{
    long q = p >> 3;
    int s = p & 7;
    unsigned long v;

    v = (unsigned long)b[q] << 24 | (unsigned long)b[q + 1] << 16 |
        (unsigned long)b[q + 2] << 8 | b[q + 3];
    if (s)
        v = (v << s | b[q + 4] >> (8 - s)) & 0xffffffffUL;
    return v;
}

/* a long split at bit p: odd bits there, even bits n bytes on */
static unsigned long
split(const unsigned char *b, long p, int n)
{
    return (get32(b, p) & M) << 1 | (get32(b, p + 8L * n) & M);
}

/* the XOR of n MFM longs from bit p, data bits only */
static unsigned long
sum(const unsigned char *b, long p, int n)
{
    unsigned long s = 0;

    while (n--) {
        s ^= get32(b, p);
        p += 32;
    }
    return s & M;
}

/*
 * The track's nsec sectors from len raw bytes into out.  0, or the
 * first error of a sector that never decoded.
 */
int
amfm_decode(const unsigned char *raw, long len, int track, int nsec, unsigned char *out)
{
    unsigned long r = 0, info, have = 0, want = (1UL << nsec) - 1, d;
    long bits = len * 8 - 40, i, s;
    int sec, k, err = AMFM_NOSECHDR;

    for (i = 0; i + 8640 <= bits && have != want; i++) {
        r = (r << 1 | (raw[i >> 3] >> (7 - (i & 7)) & 1)) & 0xffffffffUL;
        if (r != SYNC2)
            continue;
        s = i + 1;
        info = split(raw, s, 4);
        sec = info >> 8 & 0xff;
        if (info >> 24 != 0xff || (int)(info >> 16 & 0xff) != track || sec >= nsec)
            continue;
        if (sum(raw, s, 10) != split(raw, s + 320, 4)) {
            err = AMFM_BADHDRSUM;
            continue;
        }
        if (sum(raw, s + 448, 256) != split(raw, s + 384, 4)) {
            err = AMFM_BADSECSUM;
            continue;
        }
        for (k = 0; k < 128; k++) {
            d = split(raw, s + 448 + 32L * k, 512);
            out[sec * 512 + 4 * k] = d >> 24;
            out[sec * 512 + 4 * k + 1] = d >> 16;
            out[sec * 512 + 4 * k + 2] = d >> 8;
            out[sec * 512 + 4 * k + 3] = d;
        }
        have |= 1UL << sec;
        i = s + 8640 - 1;
        r = 0;
    }
    if (have == want)
        return 0;
    return have ? AMFM_TOOFEWSECS : err;
}

/* a data long's MFM, clock bits from its neighbours */
static unsigned char *
putl(unsigned char *p, unsigned long x)
{
    unsigned long prev = p[-1] & 1;

    x &= M;
    x |= ~(x << 1 | x >> 1 | prev << 31) & 0xaaaaaaaaUL;
    *p++ = x >> 24; *p++ = x >> 16; *p++ = x >> 8; *p++ = x;
    return p;
}

static unsigned long
be32(const unsigned char *b)
{
    return (unsigned long)b[0] << 24 | (unsigned long)b[1] << 16 | (unsigned long)b[2] << 8 | b[3];
}

/* the track's nsec sectors as written: gap, sectors, a word; returns the length */
long
amfm_encode(int track, int nsec, const unsigned char *data, unsigned char *raw)
{
    unsigned char *p = raw;
    unsigned long info, hsum, dsum;
    int sec, k;

    memset(p, 0xaa, AMFM_GAP);
    p += AMFM_GAP;
    for (sec = 0; sec < nsec; sec++, data += 512) {
        *p++ = 0xaa; *p++ = 0xaa; *p++ = 0xaa; *p++ = 0xaa;
        *p++ = 0x44; *p++ = 0x89; *p++ = 0x44; *p++ = 0x89;
        info = 0xff000000UL | (unsigned long)track << 16 | sec << 8 | (nsec - sec);
        p = putl(p, info >> 1);
        p = putl(p, info);
        for (k = 0; k < 8; k++)		/* the label: zeros */
            p = putl(p, 0);
        hsum = ((info >> 1) ^ info) & M;
        dsum = 0;
        for (k = 0; k < 128; k++)
            dsum ^= (be32(data + 4 * k) >> 1 ^ be32(data + 4 * k)) & M;
        p = putl(p, hsum >> 1);
        p = putl(p, hsum);
        p = putl(p, dsum >> 1);
        p = putl(p, dsum);
        for (k = 0; k < 128; k++)
            p = putl(p, be32(data + 4 * k) >> 1);
        for (k = 0; k < 128; k++)
            p = putl(p, be32(data + 4 * k));
    }
    p[0] = p[-1] & 1 ? 0x2a : 0xaa;
    p[1] = 0xa8;
    p += 2;
    return p - raw;
}

/* the bytes after the first $4489, at any bit offset: what a word-synced read gives */
long
amfm_align(const unsigned char *raw, long len, unsigned char *out, long outlen)
{
    unsigned long r = 0;
    long i, n, bits = len * 8;

    for (i = 0; i < bits; i++) {
        r = (r << 1 | (raw[i >> 3] >> (7 - (i & 7)) & 1)) & 0xffff;
        if (r == 0x4489)
            break;
    }
    if (i >= bits)
        return -1;
    i++;
    for (n = 0; n < outlen && i + 8 * n + 40 <= bits; n++)
        out[n] = get32(raw, i + 8 * n) >> 24;
    return n;
}
