# Turn off General Controls' shutdown check, in an AppleDouble header
# file (%General Controls) in place.  On its first accRun the control
# panel's driver (DRVR 49) creates an invisible "Shutdown Check" at the
# root of the startup disk, warns "not shut down properly" if the file
# was already there, and deletes it at Shut Down.  The patch branches
# over the create and the warning; the Shut Down deletion then finds no
# file, which is harmless.
#
#   move.l #2,parID(spec)    ->  bra  <install the Shut Down proc>
#
# usage: shutchk.py [-n] %file    (-n: check only)
#        shutchk.py -t            (self-test)
import sys, os, shutil, struct
from auxguard import fork, resources

# General Controls versions whose driver this was checked against
KNOWN = {b'7.5.7'}
# bne.w over the block; moveq #2,d0; move.l d0,-88(a6)
SITE = bytes.fromhex('66000086 7002 2d40ffa8')
DONE = bytes.fromhex('66000086 60000082')
# the dupFNErr test (cmpi.w #-48,d7) and _ShutDwnInstall, as a cross-check
DUP, SDI = (0x40, bytes.fromhex('0c47ffd0')), (0x94, bytes.fromhex('a895'))

def version(d, base):
    for t, rid, off, ln in resources(d, base):
        if t == b'vers' and rid == 1 and ln > 7:
            return bytes(d[off + 7:off + 7 + d[off + 6]])
    return None

def patch(d):
    """Patch bytearray d; returns 'patched' or 'already', raises otherwise."""
    base, _ = fork(d)
    v = version(d, base)
    if v not in KNOWN:
        raise ValueError('unknown General Controls version %r' % v)
    for t, rid, off, ln in resources(d, base):
        if t != b'DRVR' or rid != 49:
            continue
        b = bytes(d[off:off + ln])
        for sig, res in ((DONE, 'already'), (SITE, 'patched')):
            k = b.find(sig)
            if k < 0 or b.find(sig, k + 1) >= 0:
                continue
            if any(b[k + o:k + o + len(s)] != s for o, s in (DUP, SDI)):
                raise ValueError('DRVR 49: unexpected code around the check')
            d[off + k:off + k + len(DONE)] = DONE
            return res
        raise ValueError('DRVR 49: no shutdown check found')
    raise ValueError('no DRVR 49')

def selftest():
    # a fork holding vers 1 and a DRVR 49 with the check's code shape
    code = bytearray(0xc0)
    code[0x10:0x10 + len(SITE)] = SITE
    code[0x10 + DUP[0]:0x10 + DUP[0] + 4] = DUP[1]
    code[0x10 + SDI[0]:0x10 + SDI[0] + 2] = SDI[1]
    def mkfork(ver, drvr):
        vers = b'\x07\x57\x80\0\0\0' + bytes([len(ver)]) + ver + b'\0'
        data = struct.pack('>I', len(vers)) + vers + struct.pack('>I', len(drvr)) + drvr
        tl = struct.pack('>H', 1) + struct.pack('>4sHH', b'vers', 0, 18) + \
            struct.pack('>4sHH', b'DRVR', 0, 30)
        refs = struct.pack('>hHI4x', 1, 0xffff, 0) + \
            struct.pack('>hHI4x', 49, 0xffff, 4 + len(vers))
        m = bytes(24) + struct.pack('>HH', 28, 28 + len(tl) + len(refs)) + tl + refs
        f = struct.pack('>IIII', 256, 256 + len(data), len(data), len(m))
        f = f.ljust(256, b'\0') + data + m
        h = struct.pack('>II16sH', 0x00051607, 0x00010000, b'Macintosh       ', 1)
        h += struct.pack('>III', 2, 0x200, len(f))
        return bytearray(h.ljust(0x200, b'\0') + f)
    d = mkfork(b'7.5.7', bytes(code))
    assert patch(d) == 'patched'
    assert d.count(DONE) == 1 and SITE not in d
    assert patch(d) == 'already'
    for ver, c in ((b'7.6', bytes(code)), (b'7.5.7', bytes(0xc0))):
        try:
            patch(mkfork(ver, c))
            assert False, 'patched %r' % ver
        except ValueError:
            pass
    code[0x10 + DUP[0]] ^= 1
    try:
        patch(mkfork(b'7.5.7', bytes(code)))
        assert False, 'patched a changed driver'
    except ValueError:
        pass
    print('shutchk.py: self-test ok')

if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['-t']:
        selftest()
        sys.exit(0)
    dry = a[:1] == ['-n']
    if dry:
        a = a[1:]
    if len(a) != 1:
        sys.exit('usage: shutchk.py [-n] %file | -t')
    d = bytearray(open(a[0], 'rb').read())
    try:
        r = patch(d)
    except ValueError as e:
        sys.exit('shutchk.py: %s: %s' % (a[0], e))
    print('shutdown check: %s' % ('would patch' if dry and r == 'patched' else r))
    if not dry and r == 'patched':
        with open(a[0] + '.tmp', 'wb') as f:
            f.write(d)
        shutil.copymode(a[0], a[0] + '.tmp')
        os.rename(a[0] + '.tmp', a[0])
