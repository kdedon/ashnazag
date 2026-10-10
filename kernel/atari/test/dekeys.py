#!/usr/bin/env python3
"""Translate the scripted German marker through the driver key tables."""
import importlib.util
from pathlib import Path
import re

root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('keys', root / 'kernel/atari/hatari-keys.py')
keys = importlib.util.module_from_spec(spec)
spec.loader.exec_module(keys)

def body(path, name):
    text = re.sub(r'/\*.*?\*/', '', path.read_text(), flags=re.S)
    return text.split(name, 1)[1].split('};', 1)[0].split('= {', 1)[1]

scan = [255 if n == 'NONE' else int(n, 16) for n in re.findall(r'NONE|0x[0-9A-Fa-f]+', body(root / 'kernel/atari/atads.c', 'ad_toadb['))]
source = root / 'x11/src/amixMacKeyMap.c'
base = re.findall(r'XK_\w+|NoSymbol', body(source, 'AdbMap['))
de = {}
for code, names in re.findall(r'\{\s*(0x\w+),\s*\{([^}]+)\}', body(source, 'deKeys[')):
    de[int(code, 16)] = re.findall(r'XK_\w+|NoSymbol', names)
chars = {'XK_space': ' ', 'XK_Return': '\n', 'XK_greater': '>', 'XK_slash': '/'}
shift, output = False, ''

def press(code, shifted):
    adb = scan[code]
    assert adb != 255, 'unmapped scan code %#x' % code
    names = de.get(adb, base[adb * 2:adb * 2 + 2])
    name = names[int(shifted)] if len(names) > int(shifted) else 'NoSymbol'
    if name == 'NoSymbol':
        name = names[0]
    return chars.get(name, name[3:] if name.startswith('XK_') and len(name) == 4 else None)

lines = (root / 'kernel/atari/test/falcon-x-keys.txt').read_text().splitlines()
start = next(i for i, line in enumerate(lines) if line.startswith('type echo kezy'))
for line in lines[start:]:
    op, _, arg = line.partition(' ')
    if op == 'wait':
        break
    if op == 'type':
        for c in arg.encode().decode('unicode_escape'):
            code, shifted = keys.scancode(c)
            value = press(code, shifted)
            assert value is not None
            output += value
    elif op == 'event':
        event, code = arg.split()
        if event == 'keypress':
            value = press(int(code, 16), shift)
            assert value is not None
            output += value
        else:
            shift = event == 'keydown'
assert output == 'echo keyz > /dev/console\n', repr(output)
print('PASS German marker uses actual Atari ISO/slash and X key tables')
