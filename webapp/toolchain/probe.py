#!/usr/bin/env python3
"""Exercise the two m68k link stages with redistributable synthetic objects."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(args, cwd, ok=True):
    result = subprocess.run([str(x) for x in args], cwd=cwd, capture_output=True, text=True)
    if ok and result.returncode:
        raise RuntimeError(f"{' '.join(str(x) for x in args)}\n{result.stderr}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wasm-ld', type=Path, help='modularized Emscripten legacy ld CommonJS module')
    parser.add_argument('--node', default=os.environ.get('NODE', 'node'))
    parser.add_argument('--output', type=Path, help='write JSON result')
    args = parser.parse_args()
    legacy = ROOT / 'toolchain/amix/bin/m68k-cbm-sysv4-ld'
    assembler = ROOT / 'toolchain/amix/bin/m68k-cbm-sysv4-as'
    modern = ROOT / 'toolchain/bin/m68k-elf-ld'
    objcopy = ROOT / 'toolchain/bin/m68k-elf-objcopy'
    nm = ROOT / 'toolchain/bin/m68k-elf-nm'
    for tool in (legacy, assembler, modern, objcopy, nm):
        if not os.access(tool, os.X_OK):
            raise RuntimeError(f'Missing native prerequisite: {tool}')
    if args.wasm_ld and (not args.wasm_ld.is_file() or not shutil.which(args.node)):
        raise RuntimeError('--wasm-ld requires an existing module and Node executable')
    with tempfile.TemporaryDirectory(prefix='ash-linker-') as temp:
        work = Path(temp)
        (work / 'base.s').write_text(''' .text
 .globl _start
_start:
 .long target
 .long target-.
 .long optional
 .weak optional
 .data
 .globl pointer
pointer:
 .long target
 .bss
 .globl scratch
scratch:
 .space 32
''')
        (work / 'weak.s').write_text(' .text\n .weak optional\noptional:\n .long 0x11111111\n')
        (work / 'override.s').write_text(''' .text
 .globl target
 .globl optional
target:
 .long 0x12345678
optional:
 .long 0x22222222
''')
        (work / 'layout.ld').write_text('''ENTRY(_start)
SECTIONS {
 . = 0x10000;
 .text : { *(.text) }
 . = ALIGN(16);
 .data : { *(.data) }
 .bss : { *(.bss) *(COMMON) }
}
''')
        for name in ('base', 'weak', 'override'):
            run([assembler, '-m68040', '-o', name + '.o', name + '.s'], work)
        link_args = ['-r', '-o', 'merged.o', 'base.o', 'weak.o', 'override.o']
        run([legacy, *link_args], work)
        merged = (work / 'merged.o').read_bytes()
        run([legacy, *link_args], work)
        assert (work / 'merged.o').read_bytes() == merged, 'Non-deterministic native merge'
        run([modern, '-T', 'layout.ld', '-o', 'kernel.elf', 'merged.o'], work)
        run([objcopy, '-O', 'binary', 'kernel.elf', 'kernel.bin'], work)
        expected = bytes.fromhex('000100100000000c00010014111111111234567822222222')
        expected += bytes(8) + bytes.fromhex('00010010')
        assert (work / 'kernel.bin').read_bytes() == expected, f"Relocation/section bytes differ: {(work / 'kernel.bin').read_bytes().hex()}"
        symbols = run([nm, '-n', 'kernel.elf'], work).stdout
        for symbol in ('00010000 T _start', '00010010 T target', '00010014 T optional',
                       '00010020 D pointer', '00010024 B scratch'):
            assert symbol in symbols, f'Missing symbol: {symbol}'
        unresolved = run([modern, '-T', 'layout.ld', '-o', 'bad.elf', 'base.o'], work, ok=False)
        assert unresolved.returncode != 0, 'Undefined target unexpectedly linked'
        result = {'native': 'passed',
                  'legacy_linker': run([legacy, '--version'], work).stdout.splitlines()[0],
                  'final_linker': run([modern, '--version'], work).stdout.splitlines()[0],
                  'checks': ['absolute relocation', 'PC-relative relocation',
                  'strong overrides weak', 'section addresses', 'BSS symbol', 'undefined symbol rejection',
                  'repeatable relocatable link'], 'merged_sha256': hashlib.sha256(merged).hexdigest(),
                  'wasm': 'not tested (no module supplied)'}
        if args.wasm_ld:
            (work / 'merged.o').unlink()
            run([args.node, Path(__file__).with_name('run-linker.cjs'),
                 args.wasm_ld.resolve(), work, *link_args], work)
            assert (work / 'merged.o').read_bytes() == merged, 'WASM/native merged objects differ'
            result['wasm'] = 'passed: byte-identical legacy relocatable link'
        encoded = json.dumps(result, indent=2) + '\n'
        if args.output:
            args.output.write_text(encoded)
        print(encoded, end='')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, AssertionError, OSError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
