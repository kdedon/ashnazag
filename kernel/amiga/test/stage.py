#!/usr/bin/env python3
"""Verify the staged Amiga modules and additive device manifest."""
import pathlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'guest'))
from elfrel import Elf
kernel = Elf(sys.argv[1])
package = pathlib.Path(sys.argv[2])
exports = {s['name'] for s in kernel.syms if s['shndx'] and s['info'] >> 4 in (1, 2)}
for name in ('am_dsinit', 'plat_dmainit', 'ds_modinit', 'dlm_creg', 'dlm_xreg'):
    assert name in exports, 'missing host export ' + name
modules = ('guestcore', 'auxcore', 'auxexec', 'uinter', 'tosguest', 'amigaguest', 'cpmguest', 'dsuser', 'otbridge')
staged = {name: Elf(package / 'root/etc/conf/mod.d' / name) for name in modules}
resolved = exports | {s['name'] for module in staged.values() for s in module.syms
                      if s['shndx'] and s['info'] >> 4 in (1, 2)}
for name, module in staged.items():
    assert module.u16(16) == 1 and module.u16(18) == 4, name + ' ELF type/machine'
    assert any(s['name'] == name + '_wrapper' and s['shndx'] for s in module.syms), name + ' wrapper'
    assert sum(s['type'] == 13 and s['size'] >= 4 for s in module.sh) == 1, name + ' metadata'
    missing = [s['name'] for s in module.syms if not s['shndx'] and
               s['info'] >> 4 == 1 and s['name'] and s['name'] not in resolved]
    assert not missing, name + ' imports: ' + ','.join(missing)
rows = [line.split() for line in (package / 'root.manifest.add').read_text().splitlines()]
assert not any(row[1] == '/dev/amiga' for row in rows), 'native device must remain intact'
for name, major in zip(('fb0', 'kbd', 'mouse', 'uinter', 'otbridge', 'tos', 'amigaguest'), range(51, 58)):
    assert ['c', '/dev/' + name, '600' if name == 'otbridge' else '660', '0',
            '3' if name == 'otbridge' else '25', str(major), '0'] in rows, name + ' device node'
assert ['c', '/dev/cpm', '660', '0', '25', '59', '0'] in rows, 'CP/M device node'
for name in modules:
    assert ['f', '/etc/conf/mod.d/' + name, '644', '0', '3', 'etc/conf/mod.d/' + name] in rows, name + ' manifest'
print('PASS nine staged modules: exports/imports, metadata, wrappers and device manifest')
