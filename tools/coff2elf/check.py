#!/usr/bin/env python3
# Check a coff2elf output against its m68k COFF input, parsing both
# independently of the C tool: section bytes, symbols, relocations, and
# (for executables) entry point and PT_LOAD coverage.
# usage: check.py coff elf
import struct, sys

C_EXT, C_STAT, C_EXTDEF, C_LABEL = 2, 3, 5, 6
PCREL = {0x12: 1, 0x13: 2, 0x14: 4}
ABSREL = {0x06: 4, 0x0f: 1, 0x10: 2, 0x11: 4}
ELFTYPE = {0x06: 1, 0x11: 1, 0x10: 2, 0x0f: 3, 0x14: 4, 0x13: 5, 0x12: 6}


def coff(path):
    d = open(path, 'rb').read()
    magic, nscns, _, symptr, nsyms, opthdr, flags = struct.unpack('>HHiiiHH', d[:20])
    assert magic == 0x150, 'not m68k COFF'
    entry = struct.unpack('>I', d[36:40])[0] if opthdr >= 28 else 0
    secs = []
    for k in range(nscns):
        o = 20 + opthdr + 40 * k
        name = d[o:o+8].rstrip(b'\0').decode('latin1')
        paddr, vaddr, size, scnptr, relptr, _ = struct.unpack('>6I', d[o+8:o+32])
        nrel, _, sf = struct.unpack('>HHI', d[o+32:o+40])
        rels = [struct.unpack('>IIH', d[relptr+10*i:relptr+10*i+10])
                for i in range(nrel)]
        secs.append(dict(name=name, vaddr=vaddr, paddr=paddr, size=size,
                         scnptr=scnptr, flags=sf, rels=rels,
                         data=d[scnptr:scnptr+size] if scnptr else None))
    strtab = symptr + 18 * nsyms
    syms, i = {}, 0
    while i < nsyms:
        e = d[symptr+18*i:symptr+18*i+18]
        if e[:4] == b'\0\0\0\0':
            off, = struct.unpack('>I', e[4:8])
            name = d[strtab+off:d.index(b'\0', strtab+off)].decode('latin1')
        else:
            name = e[:8].rstrip(b'\0').decode('latin1')
        val, scn, typ, cls, naux = struct.unpack('>IhHBB', e[8:18])
        syms[i] = (name, val, scn, cls)
        i += 1 + naux
    return dict(flags=flags, entry=entry, secs=secs, syms=syms)


def elf(path):
    d = open(path, 'rb').read()
    assert d[:6] == b'\x7fELF\x01\x02'
    (etype, mach, _, entry, phoff, shoff, _, _, phes, phn, shes, shn,
     shstrndx) = struct.unpack('>HHIIIIIHHHHHH', d[16:52])
    shs = [struct.unpack('>10I', d[shoff+40*k:shoff+40*k+40]) for k in range(shn)]
    sst = shs[shstrndx]
    def nm(tab, off):
        b = d[tab[4]+off:]
        return b[:b.index(b'\0')].decode('latin1')
    secs = []
    for s in shs:
        secs.append(dict(name=nm(sst, s[0]), type=s[1], flags=s[2], addr=s[3],
                         off=s[4], size=s[5], link=s[6], info=s[7],
                         data=d[s[4]:s[4]+s[5]] if s[1] != 8 else None))
    phs = [struct.unpack('>8I', d[phoff+32*k:phoff+32*k+32]) for k in range(phn)]
    syms = []
    for s in secs:
        if s['type'] == 2:
            st = shs[s['link']]
            for k in range(s['size'] // 16):
                n, v, sz, info, oth, ndx = struct.unpack('>IIIBBH', s['data'][16*k:16*k+16])
                syms.append((nm(st, n), v, sz, info >> 4, info & 15, ndx))
    return dict(type=etype, mach=mach, entry=entry, secs=secs, phs=phs,
                syms=syms, raw=d)


def check(cpath, epath):
    c, e = coff(cpath), elf(epath)
    errs = []
    isexec = e['type'] == 2
    assert e['mach'] == 4
    # sections 1..n correspond
    for k, cs in enumerate(c['secs'], 1):
        es = e['secs'][k]
        if es['name'] != cs['name'] or es['size'] != cs['size']:
            errs.append('section %d: %s/%x vs %s/%x' % (k, cs['name'], cs['size'],
                                                       es['name'], es['size']))
    # symbols: (name, COFF absolute value, section name)
    def base(k):
        return c['secs'][k-1]['vaddr'] if not isexec else 0
    want = []
    for i, (name, val, scn, cls) in c['syms'].items():
        if cls not in (C_EXT, C_STAT, C_EXTDEF, C_LABEL) or scn == -2 or not name:
            continue
        if scn > 0 and cls != C_EXT and name == c['secs'][scn-1]['name']:
            continue
        if scn > 0:
            where = c['secs'][scn-1]['name']
            if c['secs'][scn-1]['flags'] & 1:
                where = 'ABS'
        else:
            where = {0: 'UND', -1: 'ABS'}[scn]
        if scn == 0 and val and not isexec:
            where = 'COM'
        want.append((name, val if where != 'COM' else 0, where))
    got = []
    nc = len(c['secs'])
    for name, v, sz, bind, typ, ndx in e['syms']:
        if typ == 3 or not name:
            continue
        if ndx == 0:
            where = 'UND'
        elif ndx == 0xfff1:
            where = 'ABS'
        elif ndx == 0xfff2:
            where, v = 'COM', 0
        elif ndx <= nc:
            where = e['secs'][ndx]['name']
            v += base(ndx)
        else:
            continue            # merged library symbol
        got.append((name, v, where))
    if sorted(want) != sorted(got):
        a, b = set(want), set(got)
        errs.append('symbols differ: %d vs %d; e.g. missing %s extra %s' % (
            len(want), len(got), sorted(a - b)[:3], sorted(b - a)[:3]))
    nrel = 0
    if isexec:
        if e['entry'] != c['entry']:
            errs.append('entry %x vs %x' % (e['entry'], c['entry']))
        for k, cs in enumerate(c['secs'], 1):
            es = e['secs'][k]
            load = (cs['flags'] & 0xe0 and not cs['flags'] & 3 and cs['size'])
            if not load:
                continue
            if es['addr'] != cs['vaddr']:
                errs.append('%s addr %x' % (cs['name'], es['addr']))
            ph = [p for p in e['phs'] if p[0] == 1 and p[2] <= cs['vaddr']
                  and cs['vaddr'] + cs['size'] <= p[2] + p[5]]
            if not ph:
                errs.append('%s not covered by a PT_LOAD' % cs['name'])
            elif cs['data'] is not None:
                p = ph[0]
                o = p[1] + cs['vaddr'] - p[2]
                if e['raw'][o:o+cs['size']] != cs['data']:
                    errs.append('%s bytes differ in segment' % cs['name'])
    else:
        esec = e['secs']
        # map ELF symbol index -> (name, S in COFF address space or None)
        esyms = []
        for name, v, sz, bind, typ, ndx in e['syms']:
            if typ == 3:
                name, v = esec[ndx]['name'], base(ndx)
            elif 0 < ndx <= nc:
                v += base(ndx)
            elif ndx in (0, 0xfff2):
                v = None
            esyms.append((name, v))
        for k, cs in enumerate(c['secs'], 1):
            if not cs['rels']:
                continue
            rs = [s for s in esec if s['type'] == 4 and s['info'] == k]
            if len(rs) != 1:
                errs.append('%s: no .rela section' % cs['name'])
                continue
            er = rs[0]['data']
            if len(er) != 12 * len(cs['rels']):
                errs.append('%s: reloc count' % cs['name'])
                continue
            data = bytearray(cs['data'])
            for j, (rva, si, t) in enumerate(cs['rels']):
                roff, info, add = struct.unpack('>IIi', er[12*j:12*j+12])
                w = PCREL.get(t) or ABSREL[t]
                o = rva - cs['vaddr']
                fld = int.from_bytes(cs['data'][o:o+w], 'big', signed=True)
                target = fld + (rva if t in PCREL else 0)   # COFF meaning
                cname, cval, cscn, ccls = c['syms'][si]
                if cscn > 0 and cname == c['secs'][cscn-1]['name'] and ccls == C_STAT:
                    cval = c['secs'][cscn-1]['vaddr']
                ename, ev = esyms[info >> 8]
                bad = []
                if roff != o or (info & 255) != ELFTYPE[t]:
                    bad.append('offset/type')
                if ename != cname:
                    bad.append('symbol %s vs %s' % (ename, cname))
                if ev is not None and ev != cval:
                    bad.append('S %x vs %x' % (ev, cval))
                if (add - (target - cval)) & 0xffffffff:
                    bad.append('addend %x vs %x' % (add, target - cval))
                if bad:
                    errs.append('%s reloc %d @%x: %s' % (cs['name'], j, rva, ', '.join(bad)))
                data[o:o+w] = bytes(w)
                nrel += 1
            if bytes(data) != esec[k]['data']:
                errs.append('%s: bytes differ outside relocated fields' % cs['name'])
        for k, cs in enumerate(c['secs'], 1):
            if not cs['rels'] and cs['data'] is not None and cs['data'] != esec[k]['data']:
                errs.append('%s: bytes differ' % cs['name'])
    for m in errs[:10]:
        print('  FAIL ' + m)
    print('%s %s: %d sections, %d symbols, %d relocations, %d PT_LOAD' % (
        'ok  ' if not errs else 'FAIL', cpath, len(c['secs']), len(got), nrel,
        len(e['phs'])))
    return not errs


if __name__ == '__main__':
    sys.exit(0 if check(sys.argv[1], sys.argv[2]) else 1)
