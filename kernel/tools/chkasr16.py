#!/usr/bin/env python3
# chkasr16.py -- find gcc's broken signed 64-bit shift right by 16:
#
#   move.w %dH,%dL; clr.w %dH; swap %dL; ext.l %dH
#
# which leaves the high word 0 instead of the old high word >> 16.
#
#   chkasr16.py path...      list the sites in ELF files and archives
#                            (directories are searched), fail on one
#   chkasr16.py -c gcc...    fail if a compiler still emits it
import os, re, subprocess, sys, tempfile

SEQ = [r'movew %d(\d),%d(\d)', r'clrw %d\1', r'swap %d\2', r'extl %d\1']


def elf(f):
    with open(f, 'rb') as h:
        e = h.read(20)
    # big-endian 68k ELF, or an archive
    return e[:4] == b'\x7fELF' and e[18:20] == b'\0\4' or e[:8] == b'!<arch>\n'


def elves(paths):
    for p in paths:
        if not os.path.isdir(p):
            if elf(p):
                yield p, True
            continue
        for d, _, fs in os.walk(p):
            for f in sorted(fs):
                f = os.path.join(d, f)
                if os.path.islink(f) or not os.path.isfile(f):
                    continue
                try:
                    if elf(f):
                        yield f, False
                except OSError:
                    pass


def sites(path, strict=True, od='m68k-linux-gnu-objdump'):
    r = subprocess.run([od, '-d', path],
                       capture_output=True, text=True)
    if r.returncode and not r.stdout:
        if strict:
            raise SystemExit('%s: %s' % (path, r.stderr.strip()))
        return []
    out = r.stdout
    fn, win, res, obj = '?', [], [], ''
    for l in out.splitlines():
        m = re.match(r'^(\S+):\s+file format', l)
        if m and m.group(1) != path:
            obj = m.group(1) + ': '
        m = re.match(r'^[0-9a-f]+ <(.*)>:', l)
        if m:
            fn, win = m.group(1), []
            continue
        p = l.split('\t')
        if len(p) < 3:
            continue
        win = (win + [(p[0].strip().rstrip(':'), p[2].strip())])[-4:]
        if len(win) == 4 and re.fullmatch(r'\n'.join(SEQ),
                                          '\n'.join(i for _, i in win)):
            res.append((obj + fn, win[0][0]))
    return res


def compiler(gcc):
    with tempfile.TemporaryDirectory() as t:
        c, o = os.path.join(t, 'p.c'), os.path.join(t, 'p.o')
        with open(c, 'w') as f:
            f.write('long long f(x) long long x; { return x >> 16; }\n')
        subprocess.run([gcc, '-O', '-c', c, '-o', o], check=True)
        return sites(o, od=re.sub(r'gcc$', 'objdump', gcc))


bad = 0
if sys.argv[1] == '-c':
    for g in sys.argv[2:]:
        if compiler(g):
            print('%s: miscompiles long long >> 16' % g)
            bad = 1
else:
    for e, strict in elves(sys.argv[1:]):
        for f, a in sites(e, strict):
            print('%s: %s +%s' % (e, f, a))
            bad = 1
if bad:
    sys.exit('[FAIL] signed 64-bit shift right by 16 loses the high word')
