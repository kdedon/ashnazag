#!/usr/bin/env python3
"""Verify display code ownership in the linked kernel and packaged module."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'guest'))
from elfrel import Elf

kernel, module = map(Elf, sys.argv[1:3])
bad = 0


def check(ok, msg):
    global bad
    print(('OK   ' if ok else 'FAIL ') + msg)
    bad |= not ok


def defined(e, name):
    return any(s['name'] == name and s['shndx'] != 0 for s in e.syms)


def exported(e, name):
    return any(s['name'] == name and s['shndx'] != 0 and s['info'] >> 4 in (1, 2)
               for s in e.syms)


for name in ('ds_fbopen', 'ds_evopen', 'ds_segmap', 'ds_segheld',
             'dsuser_evpost', 'dsuser_unloadsess', 'dsuser_segcount'):
    check(not defined(kernel, name) and defined(module, name), name + ' belongs to module')
for name in ('ds_init', 'ds_release', 'ds_modinit', 'ds_fbh', 'ds_evh',
             'ds_evpost', 'ds_unloadsess', 'ds_segcount'):
    check(defined(kernel, name) and not defined(module, name), name + ' stays in kernel')
missing = [s['name'] for s in module.syms if s['shndx'] == 0 and
           s['info'] >> 4 == 1 and s['name'] and not exported(kernel, s['name'])]
check(not missing, 'module imports resolve in kernel: ' + ', '.join(missing))
check(module.byname['.moddata']['type'] == 13, 'packaged module metadata')
refs = [module.syms[r[1]]['name'] for r in module.text_relocs().values()]
check('mod_hold' in refs and 'mod_rele' in refs, 'segment callbacks retain module')
check(all(defined(module, x) for x in ('dsuser_fb_td', 'dsuser_ev_td', 'dsuser_hook_td')),
      'separate frame buffer, input and hook linkage records')
sys.exit(bad)
