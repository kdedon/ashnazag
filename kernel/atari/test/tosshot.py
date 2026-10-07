#!/usr/bin/env python3
"""tosshot.py -- check the TOS environment's screen dumps.

    tosshot.py desktop DESKTOP.png      the GEM desktop is drawn and clean
    tosshot.py same A.png B.png         B shows what A shows
    tosshot.py differ A.png B.png       B shows something else

Prints [OK] or [FAIL] lines; exit 1 on a failure.
"""
import struct
import sys
import zlib


def load(path):
    """(width, height, rows of (r, g, b) tuples) of an 8-bit PNG."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        sys.exit('%s: not a PNG' % path)
    pos, idat, plte = 8, b'', None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, lace = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            plte = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b'IDAT':
            idat += body
    if depth != 8 or lace or ctype not in (2, 3, 6):
        sys.exit('%s: unsupported PNG' % path)
    bpp = {2: 3, 3: 1, 6: 4}[ctype]
    raw, stride, prev, rows, o = zlib.decompress(idat), w * bpp, bytearray(w * bpp), [], 0
    for _ in range(h):
        f, line = raw[o], bytearray(raw[o + 1:o + 1 + stride])
        o += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if ctype == 3:
            rows.append([plte[v] for v in line])
        else:
            rows.append([tuple(line[i:i + 3]) for i in range(0, stride, bpp)])
    return w, h, rows


def desktop(path):
    """A menu bar over a desktop of one colour that icons barely cover."""
    w, h, rows = load(path)
    white, black = (255, 255, 255), (0, 0, 0)
    # the menu bar: the first row from the top that is black nearly across
    bar = next((y for y in range(h // 4) if rows[y].count(black) > w // 2), None)
    if bar is None or bar < 8:
        return 'no menu bar line'
    text = sum(rows[y].count(black) for y in range(bar))
    if text < 200:
        return 'no menu titles'
    count = {}
    for y in range(bar + 1, h):
        for p in rows[y]:
            count[p] = count.get(p, 0) + 1
    # the screen without its borders: the desktop colour's columns
    colour, n = max(count.items(), key=lambda kv: kv[1])
    if set(count) <= {white, black}:
        # two colours: the desktop is a dither, black and white in turn
        x0, x1 = 0, w
        n = sum(1 for y in range(bar + 1, h) for x in range(w - 1) if rows[y][x] != rows[y][x + 1])
        colour = None
    elif colour in (white, black):
        return 'no desktop colour'
    else:
        xs = [x for x in range(w) if rows[h - 1][x] == colour or rows[bar + 2][x] == colour]
        x0, x1 = min(xs), max(xs) + 1
    area = (x1 - x0) * (h - bar - 1)
    if n < area * 80 // 100:
        return 'desktop %d%% background, under 80%%' % (100 * n // area)
    # icons and labels are small; a garbled draw leaves tall white blocks
    for x in range(x0, x1):
        run = 0
        for y in range(bar + 1, h):
            run = run + 1 if rows[y][x] == white else 0
            if run > 64:
                return 'a white block at x %d' % x
    return None


def diff(a, b):
    """The fraction of pixels that differ."""
    wa, ha, ra = load(a)
    wb, hb, rb = load(b)
    if (wa, ha) != (wb, hb):
        return 1.0
    n = sum(1 for y in range(ha) for x in range(wa) if ra[y][x] != rb[y][x])
    return n / float(wa * ha)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == 'desktop':
        why = desktop(sys.argv[2])
        ok, msg = why is None, 'desktop drawn' + (': ' + why if why else '')
    elif len(sys.argv) == 4 and sys.argv[1] in ('same', 'differ'):
        d = diff(sys.argv[2], sys.argv[3])
        ok = d < 0.01 if sys.argv[1] == 'same' else d > 0.2
        msg = 'screen %s (%.1f%% differs)' % ('restored' if sys.argv[1] == 'same' else 'switched', 100 * d)
    else:
        sys.exit(__doc__)
    print('[OK] ' + msg if ok else '[FAIL] ' + msg)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
