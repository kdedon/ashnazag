#!/usr/bin/env python3
"""Prepare a private AmigaOS directory template from supplied 3.2 floppies."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
from adf import ADF
from p96prefs import FILES as P96FILES

STARTUP = '''; Container startup; original system sequence is Startup-Sequence.amigaos.
; Output opens the boot shell window and with it the Workbench screen, so
; everything stays silent until IPrefs has set the screen mode.
FailAt 21
MakeDir >NIL: RAM:ENV RAM:T RAM:Clipboards
Assign >NIL: ENV: RAM:ENV
Assign >NIL: T: RAM:T
Assign >NIL: CLIPS: RAM:Clipboards
Assign >NIL: ENVARC: SYS:Prefs/Env-Archive
Copy >NIL: ENVARC: ENV: ALL QUIET
Assign >NIL: REXX: S:
Assign >NIL: PRINTERS: DEVS:Printers
Assign >NIL: KEYMAPS: DEVS:Keymaps
Assign >NIL: LOCALE: SYS:Locale
Assign >NIL: LIBS: SYS:Classes ADD
Assign >NIL: HELP: LOCALE:Help DEFER
Run >NIL: C:container-input
LoadMonDrvs >NIL: EXCEPT Container
SetEnv >NIL: Language "english"
AddDataTypes >NIL: REFRESH QUIET
IPrefs >NIL:
ConClip
Path C: SYS:Utilities SYS:Rexxc SYS:System S: SYS:Prefs SYS:Tools SYS:Tools/Commodities
If EXISTS S:User-Startup
  Execute S:User-Startup
EndIf
LoadWB
EndCLI >NIL:
'''


def donotwait(info):
    """A plain tool icon with the DONOTWAIT tool type: Workbench does not
    wait for its WBStartup program to return."""
    import struct
    if len(info) < 98 or info[:4] != b'\xe3\x10\x00\x01' or info[48] != 3 \
            or any(info[o:o + 4] != bytes(4) for o in (26, 50, 54, 66, 70)):
        raise ValueError('not a plain tool icon')
    width, height, depth = struct.unpack('>HHH', info[82:88])
    if 98 + (width + 15) // 16 * 2 * height * depth != len(info):
        raise ValueError('tool icon has data after its image')
    tool = b'DONOTWAIT\0'
    return (info[:54] + b'\0\0\0\1' + info[58:] + struct.pack('>LL', 8, len(tool)) + tool)


def prepare(adfs, output, input_binary, card, session):
    sources = {}
    disks = {}
    for name in ('Install3.2', 'Workbench3.2', 'Extras3.2', 'Classes3.2', 'Fonts', 'Storage3.2', 'Locale', 'ModulesA4000D_3.2'):
        data = (adfs / (name + '.adf')).read_bytes()
        disks[name] = {entry.path.lower(): entry for entry in ADF(data).entries()}
        sources[name] = hashlib.sha256(data).hexdigest()
    payloads = {}
    provenance = {}

    def put(path, data, source):
        key = path.lower()
        payloads[key] = (path, data)
        provenance[key] = source

    def one(disk, source, target=None):
        entry = disks[disk][source.lower()]
        put(target or entry.path, entry.data, disk + ':' + entry.path)

    def tree(disk, prefix='', target='', omit=()):
        prefix = prefix.lower().rstrip('/')
        for key, entry in disks[disk].items():
            if prefix:
                if not key.startswith(prefix + '/'):
                    continue
                relative = entry.path[len(prefix) + 1:]
            else:
                relative = entry.path
            if relative.split('/')[0].lower() in omit:
                continue
            put(str(PurePosixPath(target) / relative), entry.data, disk + ':' + entry.path)

    one('Install3.2', 'Installer', 'System/Installer')
    one('Install3.2', 'Libs/workbench.library')
    one('Install3.2', 'Libs/icon.library')
    tree('Workbench3.2', omit=('disk.info', 'locale'))
    tree('Extras3.2', omit=('disk.info',))
    tree('Classes3.2', omit=('disk.info',))
    tree('Fonts', target='Fonts', omit=('disk.info',))
    for source, target in (('Classes/DataTypes', 'Classes/DataTypes'), ('C', 'C'), ('LIBS', 'Libs'),
                           ('DefIcons', 'Prefs/Env-Archive/Sys'), ('Presets/Pointers', 'Prefs/Presets/Pointers'),
                           ('Monitors', 'Storage/Monitors'), ('DOSDrivers', 'Storage/DOSDrivers'),
                           ('Keymaps', 'Devs/Keymaps'), ('Printers', 'Storage/Printers')):
        tree('Storage3.2', source, target)
    one('Storage3.2', 'Env-Archive/deficons.prefs', 'Prefs/Env-Archive/deficons.prefs')
    one('Storage3.2', 'Env-Archive/Pointer.prefs', 'Prefs/Env-Archive/Sys/Pointer.prefs')
    tree('Locale', 'Countries', 'Locale/Countries')
    for directory in ('DEVS', 'L', 'LIBS'):
        tree('ModulesA4000D_3.2', directory, directory)
    one('Install3.2', 'Update/Disk.info', 'Disk.info')
    one('Install3.2', 'Update/Release', 'Prefs/Env-Archive/Versions/Release')
    one('Install3.2', 'Update/Startup-HardDrive', 'S/Startup-Sequence.amigaos')
    put('S/Startup-Sequence', STARTUP.encode('ascii'), 'container')
    put('C/container-input', input_binary.read_bytes(), 'container')
    put('Libs/Picasso96/container.card', card.read_bytes(), 'container')
    # Workbench's Tools menu: Log Out, and Shut Down for root
    put('WBStartup/Session', session.read_bytes(), 'container')
    put('WBStartup/Session.info', donotwait(payloads['prefs/env-archive/sys/def_tool.info'][1]), 'container')
    for path, make in P96FILES.items():
        put(path, make(), 'container')
    required = ('c/assign', 'c/loadwb', 'c/loadmondrvs', 'l/con-handler', 'l/ram-handler',
                'libs/workbench.library', 'libs/icon.library', 's/startup-sequence', 'c/container-input')
    if any(name not in payloads for name in required):
        raise ValueError('template lacks a required startup file')
    output.mkdir(parents=True, exist_ok=False)
    paths = {}

    def destination(relative):
        parent = output
        key = ''
        for component in PurePosixPath(relative).parts:
            key += '/' + component.lower()
            parent = paths.setdefault(key, parent / component)
        return parent

    manifest = {'source_adfs': sources, 'files': [], 'profile': 'container-english',
                'limitations': ['Prepared from media; vendor installer not executed',
                                'CPU/MMU/ROM patching and physical disk mounts omitted',
                                'Optional locale help/catalogs and commodities omitted',
                                'P96 runtime and monitor configuration remain separate',
                                'Guest boot not verified']}
    for key, (relative, data) in sorted(payloads.items()):
        path = destination(relative)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o644)
        manifest['files'].append({'path': str(path.relative_to(output)), 'source': provenance[key],
                                  'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    for relative in ('Devs/Monitors', 'Devs/DOSDrivers', 'WBStartup', 'Prefs/Env-Archive', 'Locale/Help'):
        destination(relative).mkdir(parents=True, exist_ok=True)
    (output / '.container-template.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (output / '.container-template.json').chmod(0o644)
    output.chmod(0o755)
    for directory in output.rglob('*'):
        if directory.is_dir():
            directory.chmod(0o755)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('adfs', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--input', type=Path, required=True, dest='input_binary')
    parser.add_argument('--card', type=Path, required=True)
    parser.add_argument('--session', type=Path, required=True)
    args = parser.parse_args()
    manifest = prepare(args.adfs, args.output, args.input_binary, args.card, args.session)
    print('Prepared', len(manifest['files']), 'files in', args.output)


if __name__ == '__main__':
    main()
