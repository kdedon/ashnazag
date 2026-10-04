#!/usr/bin/env python3
"""Inventory an AmigaOS CD or extract its ROM and ADF files locally."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import subprocess
import sys

A4000_SHA256 = '151f1984fa567a183d761378af8050e35cd8694b169cff2bcb49dd539eb0f86d'


def rom_info(data):
    if len(data) != 0x80000:
        raise ValueError('expected a 512 KiB Kickstart ROM')
    checksum = 0
    for (word,) in struct.iter_unpack('>I', data):
        checksum += word
        checksum = (checksum & 0xffffffff) + (checksum >> 32)
    pc = struct.unpack_from('>I', data, 4)[0]
    if checksum != 0xffffffff:
        raise ValueError('Kickstart checksum mismatch')
    if data[:4] != bytes.fromhex('11144ef9') or not 0xf80000 <= pc < 0x1000000 or pc & 1:
        raise ValueError('invalid Kickstart reset header')
    digest = hashlib.sha256(data).hexdigest()
    return dict(size=len(data), sha256=digest, reset_pc=hex(pc),
                version=struct.unpack_from('>H', data, 12)[0],
                revision=struct.unpack_from('>H', data, 14)[0],
                a4000_32=digest == A4000_SHA256)


def iso(iso_path, *args):
    return subprocess.check_output(['isoinfo', '-i', str(iso_path), '-R', *args])


def entries(iso_path):
    result = []
    for source in iso(iso_path, '-f').decode('latin-1').splitlines():
        name = source.removesuffix(';1').lstrip('/')
        path = PurePosixPath(name)
        if len(path.parts) == 2 and path.parts[0] in ('ROM', 'ADF') and path.suffix.lower() in ('.rom', '.bin', '.adf'):
            result.append((source, name))
    if not any(name == 'ROM/kicka4000.rom' for _, name in result):
        raise ValueError('CD lacks ROM/kicka4000.rom')
    if len({name for _, name in result}) != len(result):
        raise ValueError('duplicate CD filenames')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for cmd in ('inventory', 'extract'):
        p = sub.add_parser(cmd)
        p.add_argument('iso', type=Path)
        if cmd == 'extract':
            p.add_argument('destination', type=Path, help='private local media directory; must not exist')
    p = sub.add_parser('check-rom')
    p.add_argument('rom', type=Path)
    args = parser.parse_args()
    if args.command == 'check-rom':
        info = rom_info(args.rom.read_bytes())
        print(json.dumps(info, indent=2))
        if not info['a4000_32']:
            raise ValueError('ROM is valid but is not the supported A4000 3.2 image')
        return
    paths = entries(args.iso)
    manifest = {'source': args.iso.name, 'files': []}
    if args.command == 'extract':
        args.destination.mkdir(mode=0o700, parents=True, exist_ok=False)
    for source, name in paths:
        record = {'path': name}
        if args.command == 'extract' or name == 'ROM/kicka4000.rom':
            data = iso(args.iso, '-x', source)
            if not data:
                raise ValueError('empty ISO entry: ' + name)
            record.update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
            if name == 'ROM/kicka4000.rom':
                record.update(rom_info(data))
                if not record['a4000_32']:
                    raise ValueError('A4000 ROM differs from the supported 3.2 CD image')
            if args.command == 'extract':
                target = args.destination / name
                target.parent.mkdir(mode=0o700, exist_ok=True)
                with target.open('xb') as output:
                    output.write(data)
                target.chmod(0o600)
        manifest['files'].append(record)
    rendered = json.dumps(manifest, indent=2) + '\n'
    if args.command == 'extract':
        (args.destination / 'manifest.json').write_text(rendered)
    print(rendered, end='')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print('media: ' + str(exc), file=sys.stderr)
        sys.exit(1)
