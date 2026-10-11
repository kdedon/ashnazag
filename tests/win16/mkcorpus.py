#!/usr/bin/env python3
# mkcorpus.py -- x86diff's corpus: the initial states of the first N
# tests of each 80386 suite file (MOO, gzipped), and the addresses each
# test's final state lists.
#
#   python3 tests/win16/mkcorpus.py [-n N] out FILE.MOO.gz...
import gzip
import struct
import sys

n = 150
args = sys.argv[1:]
if args[:1] == ["-n"]:
    n = int(args[1])
    args = args[2:]
out = open(args[0], "wb")
# RG32 bits: cr0 cr3 eax ebx ecx edx esi edi ebp esp cs ds es fs gs ss eip eflags
GPR = {2: 0, 3: 3, 4: 1, 5: 2, 6: 6, 7: 7, 8: 5, 9: 4}		# -> AX CX DX BX SP BP SI DI
SEG = {10: 1, 11: 3, 12: 0, 13: 4, 14: 5, 15: 2}		# -> ES CS SS DS FS GS
total = 0


def chunks(b, o, end):
    while o + 8 <= end:
        t = b[o:o + 4]
        ln = struct.unpack_from("<I", b, o + 4)[0]
        yield t, o + 8, ln
        o += 8 + ln


for path in args[1:]:
    b = gzip.open(path).read()
    k = 0
    for t, o, ln in chunks(b, 0, len(b)):
        if t != b"TEST" or k >= n:
            continue
        ini = fin = None
        for t2, o2, l2 in chunks(b, o + 4, o + ln):
            if t2 == b"INIT":
                ini = (o2, l2)
            elif t2 == b"FINA":
                fin = (o2, l2)
        if not ini or not fin:
            continue
        cr0 = 0
        gpr = [0] * 8
        seg = [0] * 6
        ip = fl = 0
        ram = []
        for t3, o3, l3 in chunks(b, ini[0], ini[0] + ini[1]):
            if t3 == b"RG32":
                mask = struct.unpack_from("<I", b, o3)[0]
                p = o3 + 4
                for i in range(20):
                    if not mask & 1 << i:
                        continue
                    v = struct.unpack_from("<I", b, p)[0]
                    p += 4
                    if i == 0:
                        cr0 = v
                    elif i in GPR:
                        gpr[GPR[i]] = v
                    elif i in SEG:
                        seg[SEG[i]] = v & 0xffff
                    elif i == 16:
                        ip = v
                    elif i == 17:
                        fl = v
            elif t3 == b"RAM ":
                cnt = struct.unpack_from("<I", b, o3)[0]
                for i in range(cnt):
                    a, v = struct.unpack_from("<IB", b, o3 + 4 + 5 * i)
                    ram.append((a, v))
        wrote = []
        for t3, o3, l3 in chunks(b, fin[0], fin[0] + fin[1]):
            if t3 == b"RAM ":
                cnt = struct.unpack_from("<I", b, o3)[0]
                wrote += [struct.unpack_from("<I", b, o3 + 4 + 5 * i)[0] for i in range(cnt)]
        out.write(struct.pack(">I8I6HIIH", cr0, *gpr, *seg, ip, fl, len(ram)))
        for a, v in ram:
            out.write(struct.pack(">IB", a, v))
        out.write(struct.pack(">H", len(wrote)))
        for a in wrote:
            out.write(struct.pack(">I", a))
        k += 1
    total += k
print("%d cases" % total)
