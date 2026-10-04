#!/usr/bin/env python3
# chkguest.py -- static checks of guest-process support in a fully
# linked kernel image.
#
#   chkguest.py image.elf gates.lst
#
# - every gated M68Kvec slot holds its guest_gate_N, vector 42 (the
#   ungated reference trap) still holds nullvect;
# - guest_chain[N] is the handler the slot had before (gates.lst);
# - each gate sends supervisor faults and native processes to that
#   handler and reaches guest_gate_c only after testing p_evpdp;
# - guest_gate_c calls guest_trap and leaves through ureturn or the
#   chain;
# - the A-line gate sends a guest with GPF_ALINE to guest_linea, which
#   leaves by rte or through guest_gate_c;
# - hooksw names the seven hook pointers, all 0 at link time;
# - the events stubs, sendsig, valid_usr_range, fsig and execsw are ours,
#   with the stock bodies at __amix_*; execsw has 11 rows and the
#   stock table's three rows are coffexec, elfexec, intpexec.
# One OK/FAIL line per check; exit 1 on any FAIL.
import re, struct, subprocess, sys

elf, lst = sys.argv[1], sys.argv[2]
d = open(elf, 'rb').read()
u16 = lambda o: struct.unpack('>H', d[o:o+2])[0]
u32 = lambda o: struct.unpack('>I', d[o:o+4])[0]
shoff, shnum, shstr = u32(32), u16(48), u16(50)
secs = []
for i in range(shnum):
    b = shoff + 40 * i
    secs.append(dict(name=u32(b), type=u32(b+4), addr=u32(b+12), off=u32(b+16),
                     size=u32(b+20), link=u32(b+24)))
so = secs[shstr]['off']
for s in secs:
    s['nm'] = d[so + s['name']:d.index(b'\0', so + s['name'])].decode()
sym, byaddr = {}, {}
for s in secs:
    if s['type'] == 2:
        st = secs[s['link']]['off']
        for k in range(s['size'] // 16):
            b = s['off'] + 16 * k
            n = u32(b)
            nm = d[st+n:d.index(b'\0', st+n)].decode()
            if u16(b + 14) and d[b + 12] >> 4:
                sym.setdefault(nm, (u32(b + 4), u32(b + 8)))
                byaddr.setdefault(u32(b + 4), nm)


def rd(a, n):
    for s in secs:
        if s['type'] in (1, 8) and s['addr'] <= a and a + n <= s['addr'] + s['size']:
            if s['type'] == 8:
                return bytes(n)
            return d[s['off'] + a - s['addr']:s['off'] + a - s['addr'] + n]
    raise SystemExit('FAIL address 0x%x not in the image' % a)


L = lambda a: struct.unpack('>I', rd(a, 4))[0]
bad = 0


def check(ok, msg):
    global bad
    print(('OK   ' if ok else 'FAIL ') + msg)
    bad |= not ok


def dis(a, n):
    return subprocess.run(['m68k-elf-objdump', '-d', '--start-address=0x%x' % a,
                           '--stop-address=0x%x' % (a + n), elf],
                          capture_output=True, text=True).stdout


vec = sym['M68Kvec'][0]
chain = sym['guest_chain'][0]
gated = [(int(v), t) for v, t in (l.split() for l in open(lst))]
wrong = [v for v, t in gated if L(vec + 4 * v) != sym['guest_gate_%d' % v][0]]
check(not wrong, '%d gated slots hold their gates' % len(gated) + (' (not: %s)' % wrong if wrong else ''))
check(L(vec + 4 * 42) == sym['nullvect'][0], 'vector 42 (trap #10) is still nullvect')
wrong = [v for v, t in gated if L(chain + 4 * v) != sym[t][0]]
check(not wrong, 'guest_chain holds the previous handlers' + (' (not: %s)' % wrong if wrong else ''))

gc = sym['guest_gate_c'][0]
bad_g = []
for v, t in gated:
    a = sym['guest_gate_%d' % v][0]
    txt = dis(a, 0x60)
    body = txt[txt.index('<guest_gate_%d>:' % v):]
    body = body.split('<guest_gate_', 2)
    body = body[0] + body[1]
    ins = [l.split('\t')[2] if l.count('\t') >= 2 else '' for l in body.splitlines()[1:]]
    ins = [i for i in ins if i]
    jmps = [i for i in ins if i.startswith('jmp')]
    ok = (len(jmps) >= 2 and ('%x' % sym[t][0]) in jmps[-2] and ('%x' % gc) in jmps[-1] and
          any(i.startswith('tstl') and '200' in i for i in ins))
    if v < 32 or v >= 48:
        ok = ok and ins[0].startswith('btst') and ('%x' % sym[t][0]) in jmps[0]
    if not ok:
        bad_g.append(v)
check(not bad_g, 'gates: native and supervisor paths reach the stock handler, '
      'guest_gate_c only after the p_evpdp test' + (' (not: %s)' % bad_g if bad_g else ''))
txt = dis(gc, 0x60)
check(('<guest_trap>' in txt) and ('<ureturn>' in txt) and txt.count('jsr') == 1,
      'guest_gate_c: one call, guest_trap; exit through ureturn')
txt = dis(sym['guest_gate_10'][0], 0x60)
txt = txt[txt.index('<guest_gate_10>:'):].split('<guest_gate_', 2)[1]
check('btst #0,%a0@(15)' in txt and '<guest_linea>' in txt,
      'A-line gate: GPF_ALINE (gp_flags bit 0) selects guest_linea')
for v, bit, off, f in ((8, 1, 15, 'guest_fpriv'), (33, 0, 14, 'guest_ftrap'),
                       (45, 1, 14, 'guest_ftrap')):
    txt = dis(sym['guest_gate_%d' % v][0], 0x60)
    txt = txt[txt.index('<guest_gate_%d>:' % v):].split('<guest_gate_', 2)[1]
    check('btst #%d,%%a0@(%d)' % (bit, off) in txt and '<%s>' % f in txt,
          'gate %d: gp_flags bit %d at %d selects %s' % (v, bit, off, f))
for f in ('guest_fpriv', 'guest_ftrap'):
    txt = dis(sym[f][0], 0x200)
    ins = [l.split('\t')[2] for l in txt.splitlines() if l.count('\t') >= 2]
    check('rte' in ins and '<guest_gate_c>' in txt and '<u+0x374>' in txt,
          '%s: returns by rte, falls back to guest_gate_c' % f)
la = sym['guest_linea'][0]
txt = dis(la, 0xc0)
ins = [l.split('\t')[2] for l in txt.splitlines() if l.count('\t') >= 2]
check('rte' in ins and '<guest_gate_c>' in txt and '<u+0x374>' in txt,
      'guest_linea: returns by rte, falls back to guest_gate_c')

hs = sym['hooksw'][0]
names, ok = [], True
for i in range(16):
    n = L(hs + 16 * i)
    if n == 0:
        break
    s = rd(n, 32)
    names.append(s[:s.index(b'\0')].decode())
    p = L(hs + 16 * i + 4)
    ok = ok and byaddr.get(p) == names[-1] + '_hook' and L(p) == 0 and L(hs + 16 * i + 12) == 0
check(ok and names == ['guest_trap', 'guest_exec', 'guest_fork', 'guest_exit',
                       'guest_sendsig', 'guest_vur', 'guest_fsig', 'guest_unote'],
      'hooksw: %s, pointers 0, no owner' % ' '.join(names))

own = [s for s in ('ev_config', 'ev_fork', 'ev_exec', 'ev_exit', 'sendsig', 'valid_usr_range',
                   'fsig')
       if abs(sym[s][0] - sym['guest_trap'][0]) > 0x1000]
check(not own, 'events stubs and wrappers bound to the guest shims' + (' (not: %s)' % own if own else ''))
check(all('__amix_' + s in sym and sym['__amix_' + s][0] != sym[s][0]
          for s in ('sendsig', 'valid_usr_range', 'fsig', 'execsw')),
      '__amix_sendsig, __amix_valid_usr_range, __amix_fsig, __amix_execsw present')
check(sym['execsw'][1] == 11 * 12 and L(sym['nexectype'][0]) == 3,
      'execsw: 11 rows, nexectype 3 at link time')
ae = sym['__amix_execsw'][0]
check([byaddr.get(L(ae + 12 * i + 4)) for i in range(3)] == ['coffexec', 'elfexec', 'intpexec'],
      'stock rows: coffexec, elfexec, intpexec')
sys.exit(bad)
