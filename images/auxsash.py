#!/usr/bin/env python3
# A/UX Startup settings and disk-image checks for mkimage.sh.
#
#   auxsash.py macbin FILE.bin OUT.rsrc      resource fork of a MacBinary file
#   auxsash.py show RSRC                     SIZE -1, SASH 1 (state), SASH 2 (variables)
#   auxsash.py patch IN.rsrc OUT.rsrc [--var NAME=VALUE]... [--recovery N] [--delay N]
#                    [--autoboot 0|1]        autoBoot is set to 1 unless given
#   auxsash.py apm IMAGE                     Apple partition map
#   auxsash.py hashes IMAGE [ZIP MEMBER]     sha256 of every partition (and of the
#                                            source image inside ZIP), HFS excepted
#   auxsash.py small SRC HFSVOL OUT          SRC's DDM, map and driver, then HFSVOL
#                                            as the only other partition
#   auxsash.py apmcheck IMAGE [SRC]          partition map sanity; with SRC, the
#                                            driver matches SRC's byte for byte
#
# SASH 1 (14 bytes, TMPL 128): cluster, autorecovery mode (0 check root fs,
# 2 custom command), gmt_fix.l, eject, autoBoot, Delay.l (seconds of the
# copyright flash before the jump), check passwords, set parity.
# SASH 2 (TMPL 129): count.w, then per variable: flags.b, name (len.w, bytes,
# 0), value (len.w, bytes, 0).  Flag 0x08 = automatic (expands as a command).
import hashlib
import struct
import sys
import zipfile


def rsrc_parse(d):
    doff, moff, dlen, mlen = struct.unpack('>IIII', d[:16])
    m = d[moff:moff + mlen]
    tl, nl = struct.unpack('>HH', m[24:28])
    nt = struct.unpack('>H', m[tl:tl + 2])[0] + 1
    refs = []
    for t in range(nt):
        e = m[tl + 2 + 8 * t:tl + 10 + 8 * t]
        typ = e[:4].decode('mac_roman')
        cnt, ro = struct.unpack('>HH', e[4:8])
        for k in range(cnt + 1):
            at = moff + tl + ro + 12 * k        # file offset of the ref entry
            rid = struct.unpack('>h', d[at:at + 2])[0]
            off = struct.unpack('>I', d[at + 4:at + 8])[0] & 0xffffff
            ln = struct.unpack('>I', d[doff + off:doff + off + 4])[0]
            refs.append((typ, rid, at, off, ln))
    return doff, moff, dlen, mlen, refs


def rsrc_get(d, typ, rid):
    doff, _, _, _, refs = rsrc_parse(d)
    for t, i, _, off, ln in refs:
        if (t, i) == (typ, rid):
            return d[doff + off + 4:doff + off + 4 + ln]
    raise KeyError('%s %d' % (typ, rid))


def rsrc_put(d, typ, rid, new):
    # Replace one resource's data in place; later data and the map shift.
    doff, moff, dlen, mlen, refs = rsrc_parse(d)
    ref = [r for r in refs if (r[0], r[1]) == (typ, rid)]
    if not ref:
        raise KeyError('%s %d' % (typ, rid))
    _, _, _, off, ln = ref[0]
    delta = len(new) - ln
    at = doff + off
    out = bytearray(d[:at] + struct.pack('>I', len(new)) + new + d[at + 4 + ln:])
    if moff > at:
        moff += delta
    for t, i, rat, roff, _ in refs:
        if rat > at:
            rat += delta
        if roff > off:
            v = struct.unpack('>I', out[rat + 4:rat + 8])[0]
            out[rat + 4:rat + 8] = struct.pack('>I', (v & 0xff000000) | (roff + delta))
    hdr = struct.pack('>IIII', doff, moff, dlen + delta, mlen)
    out[0:16] = hdr
    if out[moff:moff + 16] != bytes(16):   # map keeps a copy of the header
        out[moff:moff + 16] = hdr
    return bytes(out)


def vars_parse(b):
    n = struct.unpack('>H', b[:2])[0]
    p, out = 2, []
    for _ in range(n):
        fl = b[p]
        nl = struct.unpack('>H', b[p + 1:p + 3])[0]
        name = b[p + 3:p + 3 + nl].decode('mac_roman')
        q = p + 3 + nl + 1
        vl = struct.unpack('>H', b[q:q + 2])[0]
        val = b[q + 2:q + 2 + vl].decode('mac_roman')
        out.append([fl, name, val])
        p = q + 2 + vl + 1
    if p != len(b):
        raise ValueError('SASH 2: %d trailing bytes' % (len(b) - p))
    return out


def vars_build(vs):
    b = struct.pack('>H', len(vs))
    for fl, name, val in vs:
        n, v = name.encode('mac_roman'), val.encode('mac_roman')
        b += bytes([fl]) + struct.pack('>H', len(n)) + n + b'\0'
        b += struct.pack('>H', len(v)) + v + b'\0'
    return b


STATE = ('>BBlBBlBB', ('cluster', 'autorecovery_mode', 'gmt_fix', 'eject',
                       'autoBoot', 'Delay', 'check_passwords', 'set_parity'))


def show(d):
    s = rsrc_get(d, 'SIZE', -1)
    fl, pref, mn = struct.unpack('>HII', s)
    print('SIZE -1: flags 0x%04x preferred %d KB minimum %d KB' % (fl, pref >> 10, mn >> 10))
    st = struct.unpack(STATE[0], rsrc_get(d, 'SASH', 1))
    print('SASH 1 (state): ' + ', '.join('%s=%d' % kv for kv in zip(STATE[1], st)))
    print('SASH 2 (variables):')
    for fl, name, val in vars_parse(rsrc_get(d, 'SASH', 2)):
        print('  0x%02x %-13s = "%s"' % (fl, name, val))
    cfg = rsrc_get(d, 'STRL', 134)
    n, p, items = struct.unpack('>H', cfg[:2])[0], 2, []
    for _ in range(n):
        i, l = cfg[p], cfg[p + 1]
        items.append('%d=%s' % (i, cfg[p + 2:p + 2 + l].decode('mac_roman')))
        p += 2 + l
    print('STRL 134 (config; item 5 = program space KB): ' + ' '.join(items))


def patch(d, sets, recovery, delay, autoboot=1):
    vs = vars_parse(rsrc_get(d, 'SASH', 2))
    for name, val in sets:
        hit = [v for v in vs if v[1] == name]
        if not hit:
            raise KeyError('no variable ' + name)
        hit[0][2] = val
    d = rsrc_put(d, 'SASH', 2, vars_build(vs))
    st = list(struct.unpack(STATE[0], rsrc_get(d, 'SASH', 1)))
    if recovery is not None:
        st[1] = recovery
    if delay is not None:
        st[5] = delay
    st[4] = autoboot
    return rsrc_put(d, 'SASH', 1, struct.pack(STATE[0], *st))


def apm(f):
    f.seek(0)
    b0 = f.read(512)
    sig, bsz, nblk = struct.unpack('>2sHI', b0[:8])
    parts, i = [], 1
    while True:
        f.seek(512 * i)
        e = f.read(512)
        if e[:2] != b'PM':
            break
        n, st, cnt = struct.unpack('>III', e[4:16])
        parts.append((i, st, cnt, e[16:48].split(b'\0')[0].decode('mac_roman'),
                      e[48:80].split(b'\0')[0].decode('mac_roman')))
        if i >= n:
            break
        i += 1
    return sig, bsz, nblk, parts


def region_hashes(f, regions, size):
    # regions: list of (name, start, end) in bytes, sorted, non-overlapping
    hs = {r[0]: hashlib.sha256() for r in regions}
    pos, chunk = 0, 1 << 20
    while pos < size:
        b = f.read(min(chunk, size - pos))
        if not b:
            break
        for name, s, e in regions:
            lo, hi = max(s, pos), min(e, pos + len(b))
            if lo < hi:
                hs[name].update(b[lo - pos:hi - pos])
        pos += len(b)
    return {k: v.hexdigest() for k, v in hs.items()}


def regions_of(f, size):
    _, _, _, parts = apm(f)
    regs, cur = [], 0
    for i, st, cnt, name, typ in sorted(parts, key=lambda p: p[1]):
        s, e = st * 512, (st + cnt) * 512
        if s > cur:
            regs.append(('gap@%d' % (cur // 512), cur, s))
        tag = 'p%d %s (%s)' % (i, name, typ)
        if typ != 'Apple_HFS':
            regs.append((tag, s, e))
        cur = e
    if cur < size:
        regs.append(('gap@%d' % (cur // 512), cur, size))
    return regs


def ddm_driver(b0, drvent):
    # an A/UX CD's map lists the driver but its DDM has no descriptor
    # (CDs boot through the ROM); describe it from the partition entry
    if struct.unpack('>H', b0[16:18])[0]:
        return b0
    b0 = bytearray(b0)
    st, size = struct.unpack('>I', drvent[8:12])[0], struct.unpack('>I', drvent[96:100])[0]
    struct.pack_into('>HIHH', b0, 16, 1, st, (size + 511) // 512, 1)
    return bytes(b0)


def small(src, vol, out):
    # DDM, map entries for the map, the driver and the HFS volume, driver copy
    with open(src, 'rb') as f:
        _, _, _, parts = apm(f)
        f.seek(0)
        head = f.read(64 * 512)
        drv = [p for p in parts if p[4] == 'Apple_Driver']
        hfs = [p for p in parts if p[4] == 'Apple_HFS']
        if len(drv) != 1 or len(hfs) != 1 or drv[0][1] != 64:
            sys.exit('small: expected one driver at block 64 and one HFS partition')
        f.seek(64 * 512)
        driver = f.read(drv[0][2] * 512)
    hdat = open(vol, 'rb').read()
    n = len(hdat) // 512
    hstart = 64 + len(driver) // 512
    total = hstart + n
    img = bytearray(ddm_driver(head[:512], head[512 * drv[0][0]:512 * (drv[0][0] + 1)])) + bytes(63 * 512)
    struct.pack_into('>I', img, 4, total)                       # sbBlkCount
    keep = [parts[0][0], drv[0][0], hfs[0][0]]
    for k, i in enumerate(keep, 1):
        e = bytearray(head[512 * i:512 * (i + 1)])
        struct.pack_into('>I', e, 4, len(keep))                 # pmMapBlkCnt
        if i == hfs[0][0]:
            struct.pack_into('>II', e, 8, hstart, n)            # start, count
            struct.pack_into('>II', e, 80, 0, n)                # data start, count
        img[512 * k:512 * (k + 1)] = e
    with open(out, 'wb') as f:
        f.write(img + driver + hdat)
    print('wrote %s: %d blocks, HFS at %d count %d' % (out, total, hstart, n))


def apmcheck(path, src):
    bad = []
    with open(path, 'rb') as f:
        sig, bsz, nblk, parts = apm(f)
        f.seek(0, 2)
        size = f.tell() // 512
        f.seek(0)
        b0 = f.read(512)
        ents = []
        for i in range(1, 64):
            f.seek(512 * i)
            ents.append(f.read(512))
    if sig != b'ER' or bsz != 512:
        bad.append('DDM signature/block size')
    if nblk != size:
        bad.append('DDM block count %d, image %d blocks' % (nblk, size))
    ndrv = struct.unpack('>H', b0[16:18])[0]
    drvs = [struct.unpack('>IHH', b0[18 + 8 * k:26 + 8 * k]) for k in range(ndrv)]
    n = len(parts)
    for k, e in enumerate(ents):
        if k < n:
            if struct.unpack('>I', e[4:8])[0] != n:
                bad.append('entry %d: pmMapBlkCnt != %d' % (k + 1, n))
        elif e != bytes(512):
            bad.append('block %d after the map is not zero' % (k + 1))
    spans = sorted((st, st + cnt, name) for _, st, cnt, name, _ in parts)
    for (s1, e1, n1), (s2, e2, n2) in zip(spans, spans[1:]):
        if e1 > s2:
            bad.append('%s overlaps %s' % (n1, n2))
    for i, st, cnt, name, typ in parts:
        e = ents[i - 1]
        ds, dc = struct.unpack('>II', e[80:88])
        if st + cnt > size or cnt == 0:
            bad.append('%s: outside the image' % name)
        if ds + dc > cnt:
            bad.append('%s: data area outside the partition' % name)
        if typ == 'Apple_partition_map' and (st != 1 or cnt < n):
            bad.append('map partition does not hold the map')
    dp = [p for p in parts if p[4] == 'Apple_Driver']
    for blk, cnt, typ in drvs:
        if not any(p[1] <= blk and blk + cnt <= p[1] + p[2] for p in dp):
            bad.append('DDM driver at %d (%d blocks) not inside an Apple_Driver partition' % (blk, cnt))
    if src:
        with open(src, 'rb') as f:
            _, _, _, sp = apm(f)
            f.seek(0)
            s0 = f.read(512)
            sd = [p for p in sp if p[4] == 'Apple_Driver']
            f.seek(sd[0][1] * 512)
            sdrv = f.read(sd[0][2] * 512)
            sent = {}
            for p in sp:
                f.seek(p[0] * 512)
                sent[p[4]] = f.read(512)
        with open(path, 'rb') as f:
            f.seek(dp[0][1] * 512)
            mdrv = f.read(dp[0][2] * 512)
        if len(dp) != 1 or dp[0][1:3] != sd[0][1:3] or mdrv != sdrv:
            bad.append('driver partition differs from the source')
        s0 = ddm_driver(s0, sent.get('Apple_Driver', bytes(512)))
        if b0[:4] + b0[8:] != s0[:4] + s0[8:]:
            bad.append('DDM differs from the source outside sbBlkCount')
        for i, st, cnt, name, typ in parts:
            m, o = bytearray(ents[i - 1]), bytearray(sent.get(typ, bytes(512)))
            for x in (m, o):
                x[4:8] = bytes(4)
                if typ == 'Apple_HFS':
                    x[8:16] = x[80:88] = bytes(8)
            if m != o:
                bad.append('%s entry differs from the source outside count/start/size' % name)
        print('driver: %d blocks at %d, identical to the source' % (dp[0][2], dp[0][1])
              if not any('driver' in b for b in bad) else 'driver: DIFFERS')
    print('DDM %d blocks = image size; %d drivers %s; %d map entries, no overlap: %s' % (
        nblk, ndrv, drvs, n, 'ok' if not bad else '%d problems' % len(bad)))
    for m in bad:
        print('  ' + m)
    return not bad


def main(a):
    if a[0] == 'macbin':
        d = open(a[1], 'rb').read()
        dl, rl = struct.unpack('>II', d[83:91])
        s = 128 + (dl + 127) // 128 * 128
        open(a[2], 'wb').write(d[s:s + rl])
    elif a[0] == 'show':
        show(open(a[1], 'rb').read())
    elif a[0] == 'patch':
        sets, rec, dly, ab, i = [], None, None, 1, 3
        while i < len(a):
            if a[i] == '--var':
                k, v = a[i + 1].split('=', 1)
                sets.append((k, v))
            elif a[i] == '--recovery':
                rec = int(a[i + 1])
            elif a[i] == '--delay':
                dly = int(a[i + 1])
            elif a[i] == '--autoboot':
                ab = int(a[i + 1])
            else:
                sys.exit('unknown option ' + a[i])
            i += 2
        d = patch(open(a[1], 'rb').read(), sets, rec, dly, ab)
        rsrc_parse(d)
        open(a[2], 'wb').write(d)
    elif a[0] == 'apm':
        with open(a[1], 'rb') as f:
            sig, bsz, nblk, parts = apm(f)
        print('DDM %s block %d blocks %d (%d bytes)' % (sig.decode(), bsz, nblk, bsz * nblk))
        for p in parts:
            print('  %d start %8d count %8d  %-24s %s' % p)
    elif a[0] == 'hashes':
        with open(a[1], 'rb') as f:
            f.seek(0, 2)
            size = f.tell()
            regs = regions_of(f, size)
            f.seek(0)
            mine = region_hashes(f, regs, size)
        src = None
        if len(a) > 3:
            with zipfile.ZipFile(a[2]) as z, z.open(a[3]) as f:
                src = region_hashes(f, regs, size)
        elif len(a) > 2:
            with open(a[2], 'rb') as f:
                src = region_hashes(f, regs, size)
        bad = 0
        for name, s, e in regs:
            line = '%s  %-44s %10d bytes' % (mine[name], name, e - s)
            if src is not None:
                ok = src[name] == mine[name]
                bad += not ok
                line += '  %s' % ('same as source' if ok else 'DIFFERS from source')
            print(line)
        if bad:
            sys.exit(1)
    elif a[0] == 'ddm':
        with open(a[1], 'r+b') as f:
            _, _, _, parts = apm(f)
            drv = [p for p in parts if p[4] == 'Apple_Driver']
            f.seek(0)
            b0 = f.read(512)
            f.seek(512 * drv[0][0])
            b0 = ddm_driver(b0, f.read(512))
            f.seek(0)
            f.write(b0)
    elif a[0] == 'bootable':
        # the CD's HFS boot blocks have their header (ID, entry, version)
        # zeroed; the boot code after it is intact
        with open(a[1], 'r+b') as f:
            _, _, _, parts = apm(f)
            at = [p for p in parts if p[4] == 'Apple_HFS'][0][1] * 512
            f.seek(at)
            bb = f.read(18)
            if bb[:10] == bytes(10) and bb[10:17] == b'\x06System':
                f.seek(at)
                f.write(b'LK' + struct.pack('>IH', 0x60000086, 0x4418))
    elif a[0] == 'small':
        small(a[1], a[2], a[3])
    elif a[0] == 'apmcheck':
        if not apmcheck(a[1], a[2] if len(a) > 2 else None):
            sys.exit(1)
    else:
        sys.exit('unknown command ' + a[0])


if __name__ == '__main__':
    main(sys.argv[1:])
