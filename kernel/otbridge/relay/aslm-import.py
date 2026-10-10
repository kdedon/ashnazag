#!/usr/bin/env python3
"""Emit an ASLM import record and indexed function stubs."""
import argparse
import re
import sys


def emit(record, class_id, version, minimum, functions):
    ident = re.compile(r'[A-Za-z_][A-Za-z0-9_]*\Z')
    if not ident.fullmatch(record) or not class_id or len(class_id) > 255:
        raise ValueError('invalid record or class id')
    if any(ord(c) < 33 or ord(c) > 126 or c in '\\"' for c in class_id):
        raise ValueError('class id must be printable ASCII without quotes or escapes')
    if not 0 <= minimum <= version <= 65535:
        raise ValueError('invalid version range')
    names = set()
    entries = []
    for entry in functions:
        name, index = entry.split('=', 1)
        index = int(index, 0)
        if not ident.fullmatch(name) or name in names or not 0 <= index <= 65535:
            raise ValueError('invalid or duplicate function')
        names.add(name)
        entries.append((name, index))
    if not entries or len(entries) > 512:
        raise ValueError('expected 1..512 functions')
    lines = ['\t.data', '\t.even', f'\t.globl\t{record}', f'{record}:',
             '\t.long\t0,0,0', f'\t.word\t{version},{minimum}',
             f'\t.asciz\t"{class_id}"', '\t.even']
    for name, index in entries:
        lines += [f'{name}_binding:', f'\t.long\t0,{record}', '\t.word\t0', f'\t.long\t{index}']
    lines += ['\t.text', '\t.even', '\t.extern\t__aslm_ctx']
    for name, _ in entries:
        lines += [f'\t.globl\t{name}', f'{name}:',
                  f'\tlea\t{name}_binding,%a0', '\tmove.l\t(%a0),%d0',
                  f'\tbeq.s\t{name}_resolve', '\tmove.l\t%d0,%a0', '\tjmp\t(%a0)',
                  f'{name}_resolve:', '\tmove.l\t0x2b6,%a1',
                  '\tmove.l\t0x10c(%a1),%a1', '\tmove.l\t136(%a1),%a1',
                  '\tmove.l\t__aslm_ctx,%d0', '\tjmp\t(%a1)']
    return '\n'.join(lines) + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('record')
    p.add_argument('class_id')
    p.add_argument('version', type=lambda x: int(x, 0))
    p.add_argument('minimum', type=lambda x: int(x, 0))
    p.add_argument('functions', nargs='+', help='symbol=ordinal')
    a = p.parse_args()
    try:
        sys.stdout.write(emit(a.record, a.class_id, a.version, a.minimum, a.functions))
    except (ValueError, OverflowError) as e:
        p.error(str(e))


if __name__ == '__main__':
    main()
