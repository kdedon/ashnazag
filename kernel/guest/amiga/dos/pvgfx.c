/*
 * graphics.library's line and rectangle cores on the CPU, for planar
 * bitmaps in chip RAM.  Each pixel takes the plane's minterm from the
 * RastPort as the blitter would: A the dot, B the line pattern, C the
 * pixel.  Anything else returns 0 and the original code runs.
 */
typedef unsigned int U;
typedef unsigned short W;
typedef unsigned char B;

#define CHIPTOP 0x200000U

/* the planes Mask and Depth select, or 0 if any is outside chip RAM */
static U bmplanes(const B *bm, U mask, U *pl, U *rows)
{
    U i, n = 0, d, bpr;
    if (!bm) return 0;
    bpr = *(const W *)bm;
    *rows = *(const W *)(bm + 2);
    d = bm[5];
    if (d > 8 || !bpr) return 0;
    for (i = 0; i < 8; i++) {
        pl[i] = 0;
        if (i >= d || !(mask & 1 << i)) continue;
        pl[i] = *(const U *)(bm + 8 + 4 * i);
        if (pl[i] + bpr * *rows > CHIPTOP || pl[i] + bpr * *rows < pl[i]) return 0;
        n++;
    }
    return n ? bpr : (U)-1;
}

static U planes(const B *rp, U *pl, U *rows)
{
    return bmplanes(*(B *const *)(rp + 4), rp[24], pl, rows);
}

static void dot(const B *rp, const U *pl, U bpr, U rows, int x, int y, U b)
{
    U i, o, c;
    B *p, m;
    if (x < 0 || y < 0 || (U)y >= rows || (U)x >= bpr * 8) return;
    o = y * bpr + (x >> 3);
    m = 0x80 >> (x & 7);
    for (i = 0; i < 8; i++) {
        if (!pl[i]) continue;
        p = (B *)pl[i] + o;
        c = (*p & m) != 0;
        if (rp[40 + i] >> (4 + 2 * b + c) & 1) *p |= m;
        else *p &= ~m;
    }
}

/* from cp to x, y; the pattern counter and FRST_DOT as graphics leaves them */
U pv_line(B *rp, int x1, int y1)
{
    W *cp = (W *)(rp + 36), pat = *(W *)(rp + 34);
    int x = (short)cp[0], y = (short)cp[1], dx, dy, sx, sy, ma, mi, err, k, ly = -1, xmaj;
    U pl[8], bpr, rows, cnt = rp[30], first, one;
    x1 = (short)x1; y1 = (short)y1;
    if (!(bpr = planes(rp, pl, &rows))) return 0;
    cp[0] = x1; cp[1] = y1;
    first = (rp[33] & 1) || rp[28] != 2;
    one = rp[33] & 2;
    rp[33] &= ~1;
    dx = x1 - x; sx = dx < 0 ? -1 : 1; dx = dx < 0 ? -dx : dx;
    dy = y1 - y; sy = dy < 0 ? -1 : 1; dy = dy < 0 ? -dy : dy;
    xmaj = dx > dy;
    ma = xmaj ? dx : dy; mi = xmaj ? dy : dx;
    rp[30] = cnt - ma;
    if (bpr == (U)-1) return 1;
    err = 2 * mi - ma;
    for (k = 0; ; k++) {
        if ((k || first) && (!one || y != ly))
            dot(rp, pl, bpr, rows, x, y, pat >> ((cnt - k) & 15) & 1);
        ly = y;
        if (k == ma) break;
        if (err >= 0) {
            if (xmaj) y += sy; else x += sx;
            err += 2 * mi - 2 * ma;
        } else
            err += 2 * mi;
        if (xmaj) x += sx; else y += sy;
    }
    return 1;
}

/* the rectangle at msg+4 (bitmap coordinates), every pixel A and B set */
U pv_rect(B *rp, B *msg)
{
    const W *r = (const W *)(msg + 4);
    U pl[8], bpr, rows, i, o, c;
    int x, y, x0 = (short)r[0], y0 = (short)r[1], x1 = (short)r[2], y1 = (short)r[3];
    B *p, m, mt;
    if (!(bpr = planes(rp, pl, &rows))) return 0;
    if (bpr == (U)-1) return 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if ((U)x1 >= bpr * 8) x1 = bpr * 8 - 1;
    if ((U)y1 >= rows) y1 = rows - 1;
    for (i = 0; i < 8; i++) {
        if (!pl[i]) continue;
        mt = rp[40 + i];
        for (y = y0; y <= y1; y++) {
            o = y * bpr;
            for (x = x0; x <= x1; x++) {
                p = (B *)pl[i] + o + (x >> 3);
                m = 0x80 >> (x & 7);
                if (!(x & 7) && x + 7 <= x1) {
                    /* whole bytes: the minterm of C alone */
                    c = *p;
                    *p = (mt & 0x80 ? c : 0) | (mt & 0x40 ? ~c : 0);
                    x += 7;
                    continue;
                }
                c = (*p & m) != 0;
                if (mt >> (6 + c) & 1) *p |= m;
                else *p &= ~m;
            }
        }
    }
    return 1;
}

/* BltClear: d0 bytes, or rows and bytes per row with flags bit 1; bit 2 fills with flags' high word */
U pv_clear(W *mem, U count, U flags)
{
    U n = flags & 2 ? (count >> 16) * (count & 0xffff) : count;
    W v = flags & 4 ? flags >> 16 : 0;
    if (!mem || (U)mem + n > CHIPTOP || (U)mem & 1) return 0;
    for (n >>= 1; n; n--) *mem++ = v;
    return 1;
}

/* a minterm on 8 pixels at once: bit 4a+2b+c of m */
static B mt8(U m, B a, B b, B c)
{
    B r = 0;
    if (m & 0x80) r |= a & b & c;
    if (m & 0x40) r |= a & b & ~c;
    if (m & 0x20) r |= a & ~b & c;
    if (m & 0x10) r |= a & ~b & ~c;
    if (m & 0x08) r |= ~a & b & c;
    if (m & 0x04) r |= ~a & b & ~c;
    if (m & 0x02) r |= ~a & ~b & c;
    if (m & 0x01) r |= ~a & ~b & ~c;
    return r;
}

/* 8 bits from bit p of a row, the first at bit 7; p may be up to 7 left of the row */
static B bits8(const B *row, int p)
{
    if (p < 0) return row[0] >> -p;
    row += p >> 3;
    return (B)(((row[0] << 8 | row[1]) << (p & 7)) >> 8);
}

/* bytes x0..x1 of a row covering pixels x0..x1, with edge masks */
static B edge(int bx, int x0, int x1)
{
    B em = 0xff;
    if (bx == x0 >> 3) em &= 0xff >> (x0 & 7);
    if (bx == x1 >> 3) em &= 0xff << (7 - (x1 & 7));
    return em;
}

/*
 * BltPattern's work for one rectangle (bitmap coordinates): A the mask
 * (bit sx at the left edge) or all ones, B the area pattern aligned to
 * xo, yo, C the pixel.
 */
U pv_pattern(B *rp, const B *mask, U mod, int yo, const W *r, int xo, int sx, int sy)
{
    const W *pat = *(W *const *)(rp + 8);
    int x0 = (short)r[0], y0 = (short)r[1], x1 = (short)r[2], y1 = (short)r[3], y, bx, p;
    U pl[8], bpr, rows, i, pm = (1U << (rp[29] & 7)) - 1, rw = 0xffff;
    B *d, a, b, em;
    if ((rp[29] & 0x80) || !(bpr = planes(rp, pl, &rows))) return 0;
    if (bpr == (U)-1) return 1;
    if (x0 < 0) sx -= x0, x0 = 0;
    if (y0 < 0) sy -= y0, y0 = 0;
    if ((U)x1 >= bpr * 8) x1 = bpr * 8 - 1;
    if ((U)y1 >= rows) y1 = rows - 1;
    for (y = y0; y <= y1; y++) {
        const B *m = mask ? mask + (sy + y - y0) * mod : 0;
        if (pat) {
            rw = pat[(y + yo) & pm];
            rw = (rw >> (xo & 15) | rw << (16 - (xo & 15))) & 0xffff;
        }
        for (bx = x0 >> 3; bx <= x1 >> 3; bx++) {
            em = edge(bx, x0, x1);
            p = sx + bx * 8 - x0;
            a = m ? bits8(m, p) : 0xff;
            b = bx & 1 ? rw : rw >> 8;
            for (i = 0; i < 8; i++) {
                if (!pl[i]) continue;
                d = (B *)pl[i] + y * bpr + bx;
                *d = (*d & ~em) | (mt8(rp[40 + i], a, b, *d) & em);
            }
        }
    }
    return 1;
}

/* BltTemplate's work for one rectangle of bitmap bm: A the template from bit xs of row ys, B set */
U pv_template(const B *src, int xs, int ys, U mod, const B *rp, const B *bm, int xd, int yd, int w, int h)
{
    U pl[8], bpr, rows, i;
    int y, bx, x1;
    B *d, a, em;
    xs = (short)xs; ys = (short)ys; mod = (short)mod; xd = (short)xd; yd = (short)yd; w = (short)w; h = (short)h;
    if (!(bpr = bmplanes(bm, rp[24], pl, &rows))) return 0;
    if (bpr == (U)-1 || w <= 0 || h <= 0) return 1;
    if (xd < 0 || yd < 0 || (U)(xd + w) > bpr * 8 || (U)(yd + h) > rows) return 0;
    x1 = xd + w - 1;
    for (y = 0; y < h; y++) {
        const B *m = src + (ys + y) * (int)(short)mod;
        for (bx = xd >> 3; bx <= x1 >> 3; bx++) {
            em = edge(bx, xd, x1);
            a = bits8(m, xs + bx * 8 - xd);
            for (i = 0; i < 8; i++) {
                if (!pl[i]) continue;
                d = (B *)pl[i] + (yd + y) * bpr + bx;
                *d = (*d & ~em) | (mt8(rp[40 + i], a, 0xff, *d) & em);
            }
        }
    }
    return 1;
}

/* the exclusive fill of n rows of bpr bytes, each right to left from a clear carry */
U pv_fill(B *t, U n, U bpr)
{
    B *q, v, o, k;
    U c;
    if ((U)t + n * bpr > CHIPTOP || !bpr) return 0;
    for (; n; n--, t += bpr)
        for (q = t + bpr, c = 0; q > t; ) {
            v = *--q;
            for (o = 0, k = 1; k; k <<= 1) {
                if (v & k) c ^= 1;
                if (c) o |= k;
            }
            *q = o;
        }
    return 1;
}

/* BltBitMap: B the source (a plane of 0 or -1 a constant), C the destination; planes done, or -1 */
int pv_bbm(const B *sb, int xs, int ys, const B *db, int xd, int yd, int w, int h, U mt, U mask)
{
    U sbpr = *(const W *)sb, dbpr = *(const W *)db, depth = sb[5] < db[5] ? sb[5] : db[5], i, n = 0;
    int y, k, bx, b0, b1, step;
    const B *s;
    B *d, em, v;
    xs = (short)xs; ys = (short)ys; xd = (short)xd; yd = (short)yd; w = (short)w; h = (short)h;
    if (w <= 0 || h <= 0) return 0;
    if (xs < 0 || ys < 0 || xd < 0 || yd < 0 || depth > 8 ||
        (U)(xd + w) > dbpr * 8 || (U)(yd + h) > *(const W *)(db + 2) ||
        (U)(xs + w) > sbpr * 8 || (U)(ys + h) > *(const W *)(sb + 2))
        return -1;
    for (i = 0; i < depth; i++) {
        U sp = *(const U *)(sb + 8 + 4 * i), dp = *(const U *)(db + 8 + 4 * i);
        if (!(mask & 1 << i)) continue;
        if (dp < 0x400 || dp + dbpr * (yd + h) > CHIPTOP) return -1;
        if (sp && sp != (U)-1 && sp + sbpr * (ys + h) > CHIPTOP) return -1;
    }
    for (i = 0; i < depth; i++) {
        U sp = *(const U *)(sb + 8 + 4 * i);
        B *dp = *(B *const *)(db + 8 + 4 * i);
        if (!(mask & 1 << i)) continue;
        n++;
        /* overlapping copies run from the far end, as the blitter's descending mode */
        step = (U)dp + yd * dbpr + (xd >> 3) > sp + ys * sbpr + (xs >> 3) ? -1 : 1;
        for (k = 0; k < h; k++) {
            y = step > 0 ? k : h - 1 - k;
            s = (const B *)sp + (ys + y) * sbpr;
            d = dp + (yd + y) * dbpr;
            b0 = xd >> 3; b1 = (xd + w - 1) >> 3;
            for (bx = step > 0 ? b0 : b1; bx >= b0 && bx <= b1; bx += step) {
                em = edge(bx, xd, xd + w - 1);
                v = sp == 0 ? 0 : sp == (U)-1 ? 0xff : bits8(s, xs + bx * 8 - xd);
                d[bx] = (d[bx] & ~em) | (mt8(mt, 0xff, v, d[bx]) & em);
            }
        }
    }
    return n;
}

/*
 * Entries at a fixed address for the launcher's stubs: all registers kept,
 * Z clear when drawn.
 */
__asm__(
"	.section .pvvec,\"ax\"\n"
"	jmp line_e\n"
"	jmp rect_e\n"
"	jmp clear_e\n"
"	jmp pattern_e\n"
"	jmp fill_e\n"
"	jmp bbm_e\n"
"	jmp template_e\n"
"	.text\n"
"line_e:\n"
"	movem.l %d0-%d1/%a0-%a1,-(%sp)\n"
"	move.l %d1,-(%sp)\n"
"	move.l %d0,-(%sp)\n"
"	move.l %a1,-(%sp)\n"
"	jsr pv_line\n"
"	lea 12(%sp),%sp\n"
"	tst.l %d0\n"
"	movem.l (%sp)+,%d0-%d1/%a0-%a1\n"
"	rts\n"
"rect_e:\n"
"	movem.l %d0-%d1/%a0-%a1,-(%sp)\n"
"	move.l %a1,-(%sp)\n"
"	move.l %a2,-(%sp)\n"
"	jsr pv_rect\n"
"	addq.l #8,%sp\n"
"	tst.l %d0\n"
"	movem.l (%sp)+,%d0-%d1/%a0-%a1\n"
"	rts\n"
"clear_e:\n"
"	movem.l %d0-%d1/%a0-%a1,-(%sp)\n"
"	move.l %d1,-(%sp)\n"
"	move.l %d0,-(%sp)\n"
"	move.l %a1,-(%sp)\n"
"	jsr pv_clear\n"
"	lea 12(%sp),%sp\n"
"	tst.l %d0\n"
"	movem.l (%sp)+,%d0-%d1/%a0-%a1\n"
"	rts\n"
"pattern_e:\n"
"	movem.l %d0-%d1/%a0-%a1,-(%sp)\n"
"	moveq #3,%d0\n"
"1:	move.l 40(%sp),-(%sp)\n"
"	dbf %d0,1b\n"
"	move.l 20(%sp),-(%sp)\n"
"	move.l 20(%sp),-(%sp)\n"
"	move.l %a1,-(%sp)\n"
"	move.l %a0,-(%sp)\n"
"	jsr pv_pattern\n"
"	lea 32(%sp),%sp\n"
"	tst.l %d0\n"
"	movem.l (%sp)+,%d0-%d1/%a0-%a1\n"
"	rts\n"
"fill_e:\n"
"	movem.l %d0-%d1/%a0-%a1,-(%sp)\n"
"	move.l %d4,-(%sp)\n"
"	move.l %d1,-(%sp)\n"
"	move.l %a1,-(%sp)\n"
"	jsr pv_fill\n"
"	lea 12(%sp),%sp\n"
"	tst.l %d0\n"
"	movem.l (%sp)+,%d0-%d1/%a0-%a1\n"
"	rts\n"
"template_e:\n"
"	movem.l %d0-%d1/%a0-%a1,-(%sp)\n"
"	move.l %d5,-(%sp)\n"
"	move.l %d4,-(%sp)\n"
"	move.l %d3,-(%sp)\n"
"	move.l %d2,-(%sp)\n"
"	move.l %a4,-(%sp)\n"
"	move.l %a1,-(%sp)\n"
"	move.l %a2,-(%sp)\n"
"	move.l %d1,-(%sp)\n"
"	move.l %d0,-(%sp)\n"
"	move.l %a0,-(%sp)\n"
"	jsr pv_template\n"
"	lea 40(%sp),%sp\n"
"	tst.l %d0\n"
"	movem.l (%sp)+,%d0-%d1/%a0-%a1\n"
"	rts\n"
"bbm_e:\n"
"	movem.l %d0-%d1/%a0-%a1,-(%sp)\n"
"	move.l %d7,-(%sp)\n"
"	move.l %d6,-(%sp)\n"
"	move.l %d5,-(%sp)\n"
"	move.l %d4,-(%sp)\n"
"	move.l %d3,-(%sp)\n"
"	move.l %d2,-(%sp)\n"
"	move.l %a1,-(%sp)\n"
"	move.l %d1,-(%sp)\n"
"	move.l %d0,-(%sp)\n"
"	move.l %a0,-(%sp)\n"
"	jsr pv_bbm\n"
"	lea 40(%sp),%sp\n"
"	moveq #-1,%d1\n"
"	cmp.l %d1,%d0\n"
"	beq.s 1f\n"
"	move.l %d0,(%sp)\n"
"	move.w #0,%ccr\n"
"	movem.l (%sp)+,%d0-%d1/%a0-%a1\n"
"	rts\n"
"1:	move.w #4,%ccr\n"
"	movem.l (%sp)+,%d0-%d1/%a0-%a1\n"
"	rts\n");
