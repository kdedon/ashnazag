#!/usr/bin/env python3
"""Static checks of bsdsocket.library: one relocatable hunk, an auto-init
resident tag, and a vector table whose entries all point into the code."""
import struct
import sys

LVOS = 4 + 46


def check(path):
    raw = open(path, 'rb').read()
    w = lambda o: struct.unpack_from('>I', raw, o)[0]
    if w(0) != 1011 or w(8) != 1 or w(24) != 1001:
        raise SystemExit('not a single-hunk load file')
    size = w(28) * 4
    code = raw[32:32 + size]
    relocs = set()
    o = 32 + size
    if w(o) == 1004:
        n = w(o + 4)
        relocs = {w(o + 12 + 4 * i) for i in range(n)}
        o += 12 + 4 * n + 4
    if w(o) != 1010:
        raise SystemExit('missing HUNK_END')
    c = lambda off: struct.unpack_from('>I', code, off)[0]
    tag = code.find(b'\x4a\xfc')
    while tag >= 0 and not (tag + 2 in relocs and c(tag + 2) == tag):
        tag = code.find(b'\x4a\xfc', tag + 2)
    if tag < 0:
        raise SystemExit('no resident tag')
    flags, version, kind = code[tag + 10], code[tag + 11], code[tag + 12]
    if flags != 0x80 or version != 4 or kind != 9:
        raise SystemExit('resident tag: flags %x version %d type %d' % (flags, version, kind))
    name = code[c(tag + 14):code.index(b'\0', c(tag + 14))]
    if name != b'bsdsocket.library':
        raise SystemExit('resident name %r' % name)
    init = c(tag + 22)
    if c(init) < 36 + 4 * LVOS or c(init) > 65535:
        raise SystemExit('base size %d' % c(init))
    vec = c(init + 4)
    for i in range(LVOS):
        if vec + 4 * i not in relocs or c(vec + 4 * i) >= size:
            raise SystemExit('vector %d not relocated into the code' % i)
    if c(vec + 4 * LVOS) != 0xffffffff:
        raise SystemExit('vector table is not %d entries' % LVOS)
    if init + 12 not in relocs:
        raise SystemExit('init function not relocated')
    print('[ok] %s: %d bytes, %d vectors, base %d bytes' % (path, size, LVOS, c(init)))


if __name__ == '__main__':
    for p in sys.argv[1:]:
        check(p)
