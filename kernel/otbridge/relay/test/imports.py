#!/usr/bin/env python3
"""Check generated binding layout and import-resource validation."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys

base = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('aslm_import', base / 'aslm-import.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
checks = 0


def check(ok):
    global checks
    assert ok
    checks += 1


def rejected(*args):
    try:
        mod.emit(*args)
    except ValueError:
        check(True)
        return
    raise AssertionError('accepted malformed specification')


for args in [('', 'X$y', 0x100, 0x100, ['f=0']),
             ('record', '', 0x100, 0x100, ['f=0']),
             ('record', 'X$y"', 0x100, 0x100, ['f=0']),
             ('record', 'X$y', 0x100, 0x101, ['f=0']),
             ('record', 'X$y', 0x100, 0x100, ['f=-1']),
             ('record', 'X$y', 0x100, 0x100, ['f=65536']),
             ('record', 'X$y', 0x100, 0x100, ['f=0', 'f=1']),
             ('record', 'X$y', 0x100, 0x100, [])]:
    rejected(*args)

w = Path(sys.argv[1]).resolve()
w.mkdir(parents=True, exist_ok=True)
root = base.parents[2]
asm = root / 'toolchain/bin/m68k-elf-as'
link = root / 'toolchain/bin/m68k-elf-ld'
copy = root / 'toolchain/bin/m68k-elf-objcopy'
mkaslm = w / 'mkaslm'
source = base.parent / 'n0n2/mkaslm'


def run(args, expected=0):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    check(result.returncode == expected)
    if result.returncode != expected:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout


run(['cc', '-std=gnu89', '-O', '-Wall', '-o', mkaslm, source / 'mkaslm.c'])
(w / 'bindings.s').write_text(mod.emit('backend_import', 'AUXTest$backend',
                                     0x110, 0x100, ['ImportedAdd=0', 'ImportedCount=7']))
run([asm, '-m68020', '-o', w / 'bindings.o', w / 'bindings.s'])
run([copy, '-O', 'binary', '-j', '.data', w / 'bindings.o', w / 'bindings.data'])
b = (w / 'bindings.data').read_bytes()
start = (16 + len('AUXTest$backend') + 1 + 1) & ~1
check(len(b) == start + 28)
check(b[start + 8:start + 14] == b'\0' * 6)
check(struct.unpack_from('>I', b, start + 14 + 10)[0] == 7)
run([asm, '-m68020', '-o', w / 'runtime.o', source / 'aslmrt.s'])
(w / 'probe.s').write_text('''\t.text
\t.globl ImportProbe
ImportProbe:
\tjmp ImportedAdd
\t.data
\t.globl invalid_state,invalid_version,invalid_id
invalid_state: .long 1,0,0
 .word 256,256
 .asciz "X$y"
 .even
invalid_version: .long 0,0,0
 .word 256,257
 .asciz "X$y"
 .even
invalid_id: .long 0,0,0
 .word 256,256
 .asciz "X y"
 .even
''')
run([asm, '-m68020', '-o', w / 'probe.o', w / 'probe.s'])
run([link, '-r', '-d', '-o', w / 'library.o', w / 'runtime.o',
     w / 'bindings.o', w / 'probe.o'])
args = [mkaslm, '-e', 'AUXTest$import,0x110,0x100,ImportProbe',
        '-o', w / 'import.bin', w / 'library.o']
run(args + ['-I', 'backend_import'])
check('1 imports' in run([mkaslm, '-d', w / 'import.bin']))
check('0 errors' in run([mkaslm, '-l', w / 'import.bin']))
run([mkaslm, '-t', w / 'import.bin'])
for record in ['invalid_state', 'invalid_version', 'invalid_id', 'ImportProbe', 'absent']:
    run(args + ['-I', record], 2)
run(args + ['-I', 'backend_import', '-I', 'backend_import'], 2)
print(f'PASS {checks} ASLM import build checks')
