/*
 * fastmem.c -- memcpy and memset for startamiga.  The system's move a byte
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
        /* saves its own registers: gcc cannot allocate around eight clobbers */
        __asm__ volatile (
            "   move.l %0,%%a0\n"
            "   move.l %1,%%a1\n"
            "   move.l %2,%%d0\n"
            "   movem.l %%d2-%%d7/%%a2,-(%%sp)\n"
            "1: movem.l (%%a0)+,%%d1-%%d7/%%a2\n"
            "   movem.l %%d1-%%d7/%%a2,(%%a1)\n"
            "   lea 32(%%a1),%%a1\n"
            "   subq.l &1,%%d0\n"
            "   jne 1b\n"
            "   movem.l (%%sp)+,%%d2-%%d7/%%a2"
            :
            : "g" (s), "g" (d), "g" (k)
            : "d0", "d1", "a0", "a1", "memory");
        s += k << 5;
        d += k << 5;
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
            "   move.l %0,%%a1\n"
            "   move.l %1,%%d1\n"
            "   move.l %2,%%d0\n"
            "   movem.l %%d2-%%d7/%%a2,-(%%sp)\n"
            "   move.l %%d1,%%d2\n"
            "   move.l %%d1,%%d3\n"
            "   move.l %%d1,%%d4\n"
            "   move.l %%d1,%%d5\n"
            "   move.l %%d1,%%d6\n"
            "   move.l %%d1,%%d7\n"
            "   move.l %%d1,%%a2\n"
            "1: movem.l %%d1-%%d7/%%a2,(%%a1)\n"
            "   lea 32(%%a1),%%a1\n"
            "   subq.l &1,%%d0\n"
            "   jne 1b\n"
            "   movem.l (%%sp)+,%%d2-%%d7/%%a2"
            :
            : "g" (d), "g" (v), "g" (k)
            : "d0", "d1", "a1", "memory");
        d += k << 5;
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
