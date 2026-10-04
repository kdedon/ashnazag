#!/usr/bin/env python3
# run.py -- host test of the Apple Partition Map scan (apm.c).
#
#   python3 run.py WORKDIR [AUX_DISK_HEAD]
#
# Builds apmtest with the host cc, writes synthetic disk images into WORKDIR,
# checks the slice table of each, and, if given, prints and checks the table
# of the A/UX 3.1 disk image's first blocks.
import os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
work = sys.argv[1]
auxhead = sys.argv[2] if len(sys.argv) > 2 else None
os.makedirs(work, exist_ok=True)
exe = os.path.join(work, 'apmtest')
subprocess.check_call(['cc', '-std=gnu89', '-w', '-o', exe,
                       os.path.join(HERE, 'apmtest.c'), os.path.join(HERE, '..', 'apm.c')])

def ddm(nblocks, bsize=512):
    b = struct.pack('>HHI', 0x4552, bsize, nblocks)
    return b + bytes(512 - len(b))

def bzb(flags, fstype, cluster=0):
    return struct.pack('>IBBHH', 0xABADBABE, cluster, fstype, 1, flags)

def pme(nmap, start, count, name, ptype, bz=b''):
    e = struct.pack('>HHIII', 0x504D, 0, nmap, start, count)
    e += name.encode().ljust(32, b'\0') + ptype.encode().ljust(32, b'\0')
    e += struct.pack('>II', 0, count)             # pmLgDataStart, pmDataCnt
    e += struct.pack('>I', 0x37)                  # status
    e = e.ljust(0x88, b'\0') + bz
    return e.ljust(512, b'\0')

def image(path, blocks):
    with open(path, 'wb') as f:
        for b in blocks:
            f.write(b)

def run(path):
    return subprocess.check_output([exe, path]).decode().splitlines()

fails = 0
def check(tag, got, want):
    global fails
    ok = got == want
    print('%-10s %s' % (tag, 'PASS' if ok else 'FAIL'))
    if not ok:
        fails += 1
        print('  got: ', got)
        print('  want:', want)

# 1. A/UX layout (as Apple HD SC Setup writes it): bzb roles, no slice numbers
ents = [('Apple', 'Apple_partition_map', 1, 63, b''),
        ('Macintosh', 'Apple_Driver43', 64, 32, b''),
        ('Eschatology 1', 'Apple_UNIX_SVR2', 200000, 6144, bzb(0x2000, 2)),
        ('MacOS', 'Apple_HFS', 150000, 50000, b''),
        ('UNIX Root&Usr slice 0', 'Apple_UNIX_SVR2', 96, 100000, bzb(0xC000, 1)),
        ('Swap', 'Apple_UNIX_SVR2', 100096, 49904, bzb(0x2000, 3)),
        ('Extra', 'Apple_Free', 206144, 1000, b'')]
p = os.path.join(work, 'aux.img')
image(p, [ddm(207144)] + [pme(len(ents), s, c, n, t, z) for n, t, s, c, z in ents])
check('aux', run(p), [
    's0 0 207144 whole 0 |',
    's1 96 100000 bzbrole 5 UNIX Root&Usr slice 0|Apple_UNIX_SVR2',
    's2 100096 49904 bzbrole 6 Swap|Apple_UNIX_SVR2',
    's4 200000 6144 unix 3 Eschatology 1|Apple_UNIX_SVR2',
    's5 150000 50000 other 4 MacOS|Apple_HFS'])

# 2. explicit bzb slice numbers win over roles; slice > 7 falls back to order
ents = [('Apple', 'Apple_partition_map', 1, 63, b''),
        ('AMIX root', 'Apple_UNIX_SVR2', 64, 1000, bzb(0x8000 | 1, 1)),
        ('A/UX root', 'Apple_UNIX_SVR2', 1064, 1000, bzb(0x8000 | 5, 1)),
        ('AMIX swap', 'Apple_UNIX_SVR2', 2064, 500, bzb(0x0000 | 2, 3)),
        ('far', 'Apple_UNIX_SVR2', 2564, 10, bzb(0x0000 | 12, 1)),
        ('usr', 'Apple_UNIX_SVR2', 2574, 100, bzb(0x4000, 1))]
p = os.path.join(work, 'explicit.img')
image(p, [ddm(2674)] + [pme(len(ents), s, c, n, t, z) for n, t, s, c, z in ents])
check('explicit', run(p), [
    's0 0 2674 whole 0 |',
    's1 64 1000 bzbslice 2 AMIX root|Apple_UNIX_SVR2',
    's2 2064 500 bzbslice 4 AMIX swap|Apple_UNIX_SVR2',
    's3 2574 100 bzbrole 6 usr|Apple_UNIX_SVR2',
    's4 2564 10 unix 5 far|Apple_UNIX_SVR2',
    's5 1064 1000 bzbslice 3 A/UX root|Apple_UNIX_SVR2'])

# 3. pdisk-style: no bzb, roles by name; lower-case type; driver/free skipped
ents = [('Apple', 'Apple_partition_map', 1, 63, b''),
        ('Driver', 'Apple_Driver_ATA', 64, 20, b''),
        ('data', 'apple_unix_svr2', 84, 16, b''),
        ('Swap', 'Apple_UNIX_SVR2', 100, 900, b''),
        ('Root', 'Apple_UNIX_SVR2', 1000, 9000, b''),
        ('free', 'Apple_Free', 10000, 5, b''),
        ('Scratch', 'Apple_Scratch', 10005, 5, b'')]
p = os.path.join(work, 'pdisk.img')
image(p, [ddm(10010)] + [pme(len(ents), s, c, n, t, z) for n, t, s, c, z in ents])
check('pdisk', run(p), [
    's0 0 10010 whole 0 |',
    's1 1000 9000 namerole 5 Root|Apple_UNIX_SVR2',
    's2 100 900 namerole 4 Swap|Apple_UNIX_SVR2',
    's4 84 16 unix 3 data|apple_unix_svr2',
    's5 10005 5 other 7 Scratch|Apple_Scratch'])

# 4. more data partitions than slices: s4..s7 filled in map order, rest dropped
ents = [('Apple', 'Apple_partition_map', 1, 63, b'')] + \
       [('h%d' % i, 'Apple_HFS', 100 + i * 10, 10, b'') for i in range(6)]
p = os.path.join(work, 'many.img')
image(p, [ddm(200)] + [pme(len(ents), s, c, n, t, z) for n, t, s, c, z in ents])
check('many', run(p), [
    's0 0 200 whole 0 |',
    's4 100 10 other 2 h0|Apple_HFS', 's5 110 10 other 3 h1|Apple_HFS',
    's6 120 10 other 4 h2|Apple_HFS', 's7 130 10 other 5 h3|Apple_HFS'])

# 5. no map, 6. unsupported block size, 7. map without DDM signature
p = os.path.join(work, 'blank.img'); image(p, [bytes(512)] * 4)
check('blank', run(p), ['nomap'])
p = os.path.join(work, 'cd.img')
image(p, [ddm(100, 2048), pme(1, 1, 63, 'Apple', 'Apple_partition_map')])
check('bsize2048', run(p), ['nomap'])
p = os.path.join(work, 'noddm.img')
image(p, [bytes(512), pme(2, 1, 63, 'Apple', 'Apple_partition_map'),
          pme(2, 64, 500, 'Root', 'Apple_UNIX_SVR2', bzb(0x8000, 1))])
check('noddm', run(p), ['s0 0 0 whole 0 |',
                        's1 64 500 bzbrole 2 Root|Apple_UNIX_SVR2'])

# 8. truncated image: read error propagates
p = os.path.join(work, 'short.img'); image(p, [ddm(100)])
check('short', run(p), ['error 5'])

if auxhead:
    got = run(auxhead)
    print('A/UX 3.1 image map:')
    for l in got:
        print('  ' + l)
    check('auxdisk', got, [
        's0 0 2048000 whole 0 |',
        's1 96 1769468 bzbrole 5 UNIX Root&Usr slice 0|Apple_UNIX_SVR2',
        's2 1769564 131070 bzbrole 6 Swap|Apple_UNIX_SVR2',
        's4 2041856 6144 unix 3 Eschatology 1|Apple_UNIX_SVR2',
        's5 1900634 141222 other 4 MacOS|Apple_HFS'])

print('TOTAL failures: %d' % fails)
sys.exit(1 if fails else 0)
