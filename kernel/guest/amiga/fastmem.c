/*
 * fastmem.c -- memcpy and memset for startmig.  The system's move a byte
 * at a time; Mesa and Warp3D's back buffer move megabytes a frame.
 * 68020 and later read and write longs at any address.
 */

void fill32(void *, unsigned long, unsigned long);

void *
memcpy(void *dst, const void *src, __SIZE_TYPE__ n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    unsigned long k;

    while (n && ((unsigned long)d & 3)) {
        *d++ = *s++;
        n--;
    }
    if ((k = n >> 5) != 0) {
        __asm__ volatile (
            "1: movem.l (%0)+,%%d1-%%d7/%%a2\n"
            "   movem.l %%d1-%%d7/%%a2,(%1)\n"
            "   lea 32(%1),%1\n"
            "   subq.l &1,%2\n"
            "   jne 1b"
            : "=a" (s), "=a" (d), "=d" (k)
            : "0" (s), "1" (d), "2" (k)
            : "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a2", "memory");
        n &= 31;
    }
    for (; n >= 4; n -= 4, d += 4, s += 4)
        *(unsigned long *)d = *(const unsigned long *)s;
    while (n--)
        *d++ = *s++;
    return dst;
}

/* n bytes of the long v, from d on */
void
fill32(void *dst, unsigned long v, unsigned long n)
{
    unsigned char *d = dst;
    unsigned long k;

    if ((k = n >> 5) != 0) {
        __asm__ volatile (
            "   move.l %3,%%d1\n"
            "   move.l %3,%%d2\n"
            "   move.l %3,%%d3\n"
            "   move.l %3,%%d4\n"
            "   move.l %3,%%d5\n"
            "   move.l %3,%%d6\n"
            "   move.l %3,%%d7\n"
            "   move.l %3,%%a2\n"
            "1: movem.l %%d1-%%d7/%%a2,(%0)\n"
            "   lea 32(%0),%0\n"
            "   subq.l &1,%1\n"
            "   jne 1b"
            : "=a" (d), "=d" (k)
            : "0" (d), "g" (v), "1" (k)
            : "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a2", "memory");
        n &= 31;
    }
    for (; n >= 4; n -= 4, d += 4)
        *(unsigned long *)d = v;
    for (k = 24; n--; k -= 8)
        *d++ = v >> k;
}

void *
memset(void *dst, int c, __SIZE_TYPE__ n)
{
    unsigned char *d = dst;

    while (n && ((unsigned long)d & 3)) {
        *d++ = c;
        n--;
    }
    fill32(d, (c & 255) * 0x01010101UL, n);
    return dst;
}
