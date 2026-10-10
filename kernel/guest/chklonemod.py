#!/usr/bin/env python3
"""Verify CP/M imports and shared execution ownership in packaged modules."""
import sys
from elfrel import Elf
kernel, core, cpm, tos = map(Elf, sys.argv[1:5])
bad = 0

def check(ok, message):
    global bad
    print(('OK   ' if ok else 'FAIL ') + message)
    bad |= not ok

def defined(e, name):
    return any(s['name'] == name and s['shndx'] != 0 and s['info'] >> 4 in (1, 2)
               for s in e.syms)

for name in ('guest_lone_init', 'guest_lone_enter', 'guest_lone_trap', 'guest_lone_stats'):
    check(defined(core, name) and not defined(kernel, name) and not defined(cpm, name),
          name + ' belongs to guestcore')
check(defined(cpm, 'cpmguest_wrapper'), 'CP/M driver wrapper defined')
s = cpm.byname['.moddata']
check(s['type'] == 13 and cpm.cstr(s['offset'] + 4) == 'guestcore',
      'CP/M dependency is guestcore alone')
missing = [s['name'] for s in cpm.syms if s['shndx'] == 0 and s['info'] >> 4 == 1
           and s['name'] and not (defined(kernel, s['name']) or defined(core, s['name']))]
check(not missing, 'CP/M imports resolve without TOS: ' + ', '.join(missing))
check(not any(s['name'].startswith(('tos', 'TOS')) for s in cpm.syms), 'CP/M has no TOS symbol imports')
refs = [tos.syms[r[1]]['name'] for r in tos.text_relocs().values()]
check('guest_lone_enter' in refs and 'guest_lone_trap' in refs,
      'TOS NOMACH delegates initialization and exceptions')
sys.exit(bad)
