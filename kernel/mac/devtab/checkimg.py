#!/usr/bin/env python3
"""Static checks of the linked Mac kernel's device tables and of what
Amiga hardware code it can still reach.

    checkimg.py ELF BASE BASEWEAK

ELF: build/unix-mac.elf.  BASE: the relocatable base (port unix-040).
BASEWEAK: build/mac/base.weak (the base after weakening).
Prints the tables and one OK/FAIL line per check; exit 1 on any FAIL.
"""
import bisect, re, struct, subprocess, sys

ELF, BASE, WEAK = sys.argv[1:4]

# Majors whose base driver drives Amiga hardware: emptied on the Mac.
CKILL = {4: 'bb', 5: 'sl', 6: 'amiga', 10: 'screen', 12: 'machid', 13: 'ql',
         17: 'fd', 21: 'par', 22: 'tiga', 31: 'res', 41: 'ben',
         46: 'audio'}
BKILL = {16: 'fd', 17: 'hd'}
# Rows that must name a Mac object rather than the base's.
CMAC = {0: 'coinfo'}
BMAC = {20: 'ramopen'}
# Rows the Mac gives a new driver: exactly these non-nodev entries.
CNEW = {18: ['sninfo']}
CF = 'open close read write ioctl mmap segmap poll xpoll xhalt ttys str flag'.split()
BF = 'open close strat print size xpoll xhalt flag'.split()

bad = 0
def check(ok, msg):
    global bad
    print(('OK   ' if ok else 'FAIL ') + msg)
    if not ok:
        bad = 1

def run(*a):
    return subprocess.run(a, capture_output=True, text=True, check=True).stdout

# ---------------------------------------------------------------- image
img = open(ELF, 'rb').read()
syms, byname = [], {}
for l in run('m68k-elf-nm', '-n', ELF).splitlines():
    p = l.split()
    if len(p) == 3 and p[1] not in 'aAUw':
        syms.append((int(p[0], 16), p[2], p[1]))
        byname.setdefault(p[2], int(p[0], 16))
sec = {}
for l in run('m68k-elf-readelf', '-SW', ELF).splitlines():
    m = re.match(r'\s*\[\s*\d+\]\s+(\.\w+)\s+\w+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', l)
    if m:
        sec[m.group(1)] = tuple(int(m.group(i), 16) for i in (2, 3, 4))
T0, TOFF, TSZ = sec['.text']; D0, DOFF, DSZ = sec['.data']; B0, _, BSZ = sec['.bss']
END = byname['end']

def L(a):
    if D0 <= a < D0 + DSZ:
        o = DOFF + a - D0
    elif T0 <= a < T0 + TSZ:
        o = TOFF + a - T0
    else:
        raise SystemExit('0x%x not loaded' % a)
    return struct.unpack('>I', img[o:o + 4])[0]

def good(n):
    return not ('compiled' in n or n.startswith('.L') or n.startswith('LC%'))
names = {}
for a, n, t in syms:
    names.setdefault(a, []).append((t in 'TD', good(n), n))
def nm(a):
    if a == 0:
        return '0'
    if a in names:
        return sorted(names[a], reverse=True)[0][2]
    return '0x%x' % a

# ----------------------------------------------------------- base table
base_nm = {}
for l in run('m68k-linux-gnu-nm', BASE).splitlines():
    p = l.split()
    if len(p) == 3:
        base_nm.setdefault(p[2], (int(p[0], 16), p[1]))
brel = {}
sect = None
for l in run('m68k-linux-gnu-readelf', '-rW', BASE).splitlines():
    if l.startswith('Relocation section'):
        sect = l.split("'")[1]
        continue
    p = l.split()
    if sect == '.rela.data' and len(p) >= 5 and re.match(r'^[0-9a-f]{8}$', p[0]):
        brel[int(p[0], 16)] = p[4]

def base_rows(tab, n, fields):
    a0 = base_nm[tab][0]
    rows = []
    for i in range(n):
        rows.append([brel.get(a0 + (i * len(fields) + j) * 4, '0')
                     for j in range(len(fields))])
    return rows

def img_rows(tab, n, fields):
    a0 = byname[tab]
    return [[nm(L(a0 + (i * len(fields) + j) * 4)) for j in range(len(fields))]
            for i in range(n)]

bsec = {}
for l in run('m68k-linux-gnu-readelf', '-SW', BASE).splitlines():
    m = re.match(r'\s*\[\s*\d+\]\s+(\.\w+)\s+\w+\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)', l)
    if m:
        bsec[m.group(1)] = int(m.group(2), 16)
bt_end, bd_end = T0 + bsec['.text'], D0 + bsec['.data']

ncd, nbd = L(byname['cdevcnt']), L(byname['bdevcnt'])
check(ncd == 70 and nbd == 32, 'cdevcnt %d bdevcnt %d (base counts)' % (ncd, nbd))
check(byname['cdevsw'] >= bd_end and byname['bdevsw'] >= bd_end,
      'cdevsw/bdevsw bound to the Mac object (0x%x, 0x%x)' % (byname['cdevsw'], byname['bdevsw']))

def compare(label, new, old, kill, mac, fields):
    print('--- %s: major | fate | base row -> Mac row (non-nodev entries)' % label)
    allowed = set()
    for i, (r, o) in enumerate(zip(new, old)):
        live = [(f, v) for f, v in zip(fields, r)
                if v not in ('nodev', '0') and f != 'flag']
        olive = [(f, v) for f, v in zip(fields, o)
                 if v not in ('nodev', '0') and f != 'flag']
        if not olive and not live:
            continue
        if label == 'cdevsw' and i in CNEW:
            fate = 'new'
            ok = [v for f, v in live] == CNEW[i]
        elif i in kill:
            fate = 'empty (%s)' % kill[i]
            ok = not live
        elif i in mac:
            fate = 'Mac'
            ok = [v for f, v in live] == [v for f, v in olive]
        else:
            fate = 'kept'
            ok = [v for f, v in live] == [v for f, v in olive]
        allowed.update(v for f, v in live)
        print('%s[%2d] %-12s %s -> %s%s' % (label, i, fate,
              ' '.join(v for f, v in olive) or '-',
              ' '.join(v for f, v in live) or '-', '' if ok else '   <-- MISMATCH'))
        check(ok, '%s[%d] %s' % (label, i, fate))
    return allowed

cnew, cold = img_rows('cdevsw', ncd, CF), base_rows('cdevsw', ncd, CF)
bnew, bold = img_rows('bdevsw', nbd, BF), base_rows('bdevsw', nbd, BF)
roots = compare('cdevsw', cnew, cold, CKILL, CMAC, CF)
roots |= compare('bdevsw', bnew, bold, BKILL, BMAC, BF)
check(byname['coinfo'] >= D0 and nm(L(byname['cdevsw'] + 44)) == 'coinfo', 'cdevsw[0] -> coinfo (SCC)')
check(byname['ramopen'] >= bt_end, 'bdevsw[20] ram* are the Mac RAM disk')

# io_* and the other hook tables
def tbl(name):
    a, out = byname[name], []
    while L(a):
        out.append(L(a)); a += 4
    return out
for t in ('io_init', 'io_start', 'io_halt', 'io_poll', 'init_tbl'):
    v = tbl(t)
    print('%-8s %s' % (t, ' '.join(nm(x) for x in v) or '(empty)'))
    if t != 'init_tbl':
        check(all(x >= bt_end for x in v), '%s entries are Mac code' % t)
check([nm(x) for x in tbl('io_start')] == ['mac_diskprobe'], 'io_start = { mac_diskprobe }')
fm = []
for i in range(L(byname['fmodcnt'])):
    a = byname['fmodsw'] + 20 * i
    o = DOFF + a - D0
    fm.append((img[o:o + 9].split(b'\0')[0].decode(), nm(L(a + 12))))
print('fmodsw  ' + ' '.join('%s=%s' % f for f in fm))

# config chain: stext -> config_cachefix -> config_orig -> Mac config
dis = run('m68k-elf-objdump', '-d', '--no-show-raw-insn', ELF)
def body(name):
    i = dis.find('<%s>:\n' % name)
    return dis[i:dis.find('\n\n', i)]
cfg = byname['config']
check(cfg >= bt_end, 'config is the Mac config (0x%x)' % cfg)
check('config_cachefix' in body('_start') and 'config_orig' in body('config_cachefix'),
      'stext calls config_cachefix, which jumps to config_orig')
check(byname['config_orig'] >= bt_end and ('%x <config>' % cfg) in body('config_orig'),
      'config_orig is the Mac stub jumping to config')

# ------------------------------------------------ reachability of Amiga code
# Nodes: every text/data symbol, plus the weakened base bodies (dead)
# except those kept under __amix_<name> for the wrappers that call them.
nodes = dict((a, nm(a)) for a in names)
wd = {}
wk = [l.split() for l in run('m68k-linux-gnu-nm', '-n', WEAK).splitlines()]
for p in wk:
    if len(p) == 3 and p[1] in 'TD' and p[2] in byname and p[1] not in wd:
        wd[p[1]] = byname[p[2]] - int(p[0], 16)
for p in wk:
    if len(p) == 3 and p[1] in 'WV' and '__amix_' + p[2] not in byname:
        a = int(p[0], 16) + wd['T' if p[1] == 'W' else 'D']
        nodes[a] = 'DEAD:' + p[2]
addrs = sorted(nodes)
def node(v):
    i = bisect.bisect_right(addrs, v) - 1
    return addrs[i] if i >= 0 else None
edges = dict((a, set()) for a in addrs)
# string literals (LC%n) disassemble as noise: no outgoing references
strings = set(a for a in addrs if a in names and all(n.startswith('LC%') for _, _, n in names[a]))
# the exception vector table lives in .text: scanned as words below
vtab = byname['M68Kvec']
strings.add(vtab)
hw = {}
ins = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
prev = last = None
for l in dis.splitlines():
    m = ins.match(l)
    if not m:
        continue
    a, op, args = int(m.group(1), 16), m.group(2), m.group(3)
    n = node(a)
    if n in strings:
        prev = None
        continue
    if n != prev:
        if prev is not None and not re.match(r'(rts|rte|rtr|jmp|bra[wlsb]?)$', last or ''):
            edges[prev].add(n)
        prev = n
    if op != 'nop':
        last = op
    # absolute memory operands are annotated <sym+off>
    for x in re.findall(r'\b([0-9a-f]+) <', args):
        v = int(x, 16)
        if 0xA00000 <= v < 0x1000000 and v > END:
            hw.setdefault(n, set()).add('%s %s' % (op, args))
        elif T0 <= v < B0 + BSZ:
            edges[n].add(node(v))
    # immediates: addresses when loaded into an address register or pushed
    for x, dst in re.findall(r'#(-?\d+),(%a\d(?![@\w])|%sp@-)', args):
        v = int(x) & 0xffffffff
        if 0xA00000 <= v < 0x1000000 and dst != '%sp@-':
            hw.setdefault(n, set()).add('%s %s' % (op, args))
        elif T0 <= v < B0 + BSZ and v in nodes:
            edges[n].add(v)
    # a compared address is not a reference (oncons tests d_open == scropen)
    for x in ([] if op.startswith('cmp') else re.findall(r'#(-?\d+)', args)):
        v = int(x) & 0xffffffff
        if v in nodes and T0 <= v < B0 + BSZ:
            edges[n].add(v)
# the RAM-disk image and the kernel symbol table hold values, not references
blob = (byname.get('rd_image', 0), byname.get('rd_image_end', 0))
ksym = byname.get('dlm_ksym')
for i, a in enumerate(addrs):
    if a != vtab and (not (D0 <= a < D0 + DSZ) or blob[0] <= a < blob[1] or a in strings
                      or a == ksym):
        continue
    e = min(addrs[i + 1] if i + 1 < len(addrs) else D0 + DSZ, D0 + DSZ)
    for o in range(a, e - 3, 2):
        v = L(o)
        if v in nodes and v != a:
            edges[a].add(v)

seen, par, stack = set(), {}, [byname['mac_entry']]
while stack:
    a = stack.pop()
    if a in seen:
        continue
    seen.add(a)
    for b in edges[a]:
        if b is not None and b not in seen:
            par.setdefault(b, a)
            stack.append(b)
def path(a):
    p = []
    while a in par and len(p) < 12:
        p.append(nodes[a]); a = par[a]
    return ' <- '.join(p + [nodes[a]])

print('--- reference graph from mac_entry: %d nodes, %d reached' % (len(addrs), len(seen)))
dead = [a for a in seen if nodes[a].startswith('DEAD:')]
for a in dead:
    print('  reached ' + path(a))
check(not dead, 'no weakened base body is referenced')
# Port debug trace: writes Amiga colour registers only while btrace_on != 0.
GATED = {'btrace_mark', 'btrace_hex', 'Lbt_go'}
check(L(byname['btrace_on']) == 0, 'btrace_on = 0 (port trace, Amiga colour registers, off)')
live = []
for a in sorted(hw):
    if a in seen:
        tag = 'gated' if nodes[a] in GATED else 'LIVE'
        print('  %s Amiga-address code: %s [%s]' % (tag, nodes[a], sorted(hw[a])[0]))
        print('      ' + path(a))
        if tag == 'LIVE':
            live.append(nodes[a])
unreached = sorted(nodes[a] for a in hw if a not in seen)
print('  unreachable Amiga-address code: %d nodes (%s ...)' % (len(unreached), ' '.join(unreached[:12])))
check(not live, 'no reachable code touches Amiga addresses 0xA00000-0xFFFFFF (%s)' % ' '.join(live))
amiga_entries = set()
for i in CKILL:
    amiga_entries |= set(v for v in cold[i][:12] if v not in ('nodev', 'nulldev', '0'))
for i in BKILL:
    amiga_entries |= set(v for v in bold[i][:7] if v not in ('nodev', 'nulldev', '0'))
ent = sorted(e for e in amiga_entries if e in byname)
r = [e for e in ent if byname[e] in seen]
check(not r, 'emptied drivers unreachable: %s' % (' '.join(r) or ' '.join(ent)))
sys.exit(bad)
