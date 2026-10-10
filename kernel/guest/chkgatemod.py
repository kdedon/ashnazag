#!/usr/bin/env python3
"""Verify the packaged module owns the gate bodies and their hook targets."""
import subprocess, sys
from elfrel import Elf

e = Elf(sys.argv[1])
bad = 0


def check(ok, msg):
    global bad
    print(('OK   ' if ok else 'FAIL ') + msg)
    bad |= not ok


names = ('gc_gate_c', 'gc_linea', 'gc_ftrap', 'gc_fpriv')
for name in names:
    s = e.syms[e.sym(name)]
    check(e.sh[s['shndx']]['nm'] == '.text', name + ' defined in module text')
check(not any(s['shndx'] == 0 and s['name'] in names for s in e.syms),
      'gate targets have no unresolved references')
h = e.syms[e.sym('guestcore_hookdata')]
sec = e.sh[h['shndx']]
r = e.byname['.rela' + sec['nm']]
rel = {}
for o in range(r['offset'], r['offset'] + r['size'], 12):
    rel[e.u32(o)] = (e.syms[e.u32(o + 4) >> 8], e.u32(o + 8))
for i, name in enumerate(names):
    target, addend = rel[h['value'] + i * 8 + 4]
    s = e.syms[e.sym(name)]
    check(target['shndx'] == s['shndx'] and target['value'] + addend == s['value'],
          'hook %d relocates to %s' % (i, name))
rel = e.text_relocs()
for i, name in enumerate(names):
    start = e.syms[e.sym(name)]['value']
    end = e.syms[e.sym(names[i + 1])]['value'] if i < 3 else e.byname['.text']['size']
    txt = subprocess.run(['m68k-elf-objdump', '-d', '--start-address=%d' % start,
                          '--stop-address=%d' % end, e.path],
                         capture_output=True, text=True, check=True).stdout
    ins = [l.split('\t')[2] for l in txt.splitlines() if l.count('\t') >= 2]
    refs = [(e.syms[r[1]]['name'], r[3]) for o, r in rel.items() if start <= o < end]
    if i == 0:
        check(sum(x.startswith('jsr') for x in ins) == 1 and
              ('guest_trap', 0) in refs and ('ureturn', 0) in refs and
              ('guest_chain', 0) in refs and 'rts' in ins,
              'common gate calls guest_trap, returns through ureturn or stock chain')
    else:
        check('rte' in ins and '<gc_gate_c>' in txt and ('u', 0x374) in refs,
              name + ' returns by rte, fault/slow path reaches common gate')
check(e.byname['.moddata']['type'] == 13, 'packaged module has type-13 metadata')
sys.exit(bad)
