#!/usr/bin/env python3
"""Exercise staging rejection against a private package copy."""
import pathlib
import subprocess
import sys
checker = pathlib.Path(__file__).with_name('stage.py')
kernel, package = map(pathlib.Path, sys.argv[1:3])
module = package / 'root/etc/conf/mod.d/otbridge'
manifest = package / 'root.manifest.add'
original_module = module.read_bytes()
original_manifest = manifest.read_bytes()
checks = 0

def run(success):
    global checks
    result = subprocess.run([sys.executable, str(checker), str(kernel), str(package)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert (result.returncode == 0) == success, result.stdout.decode() + result.stderr.decode()
    checks += 1

try:
    run(True)
    module.unlink()
    run(False)
    module.write_bytes(original_module)
    bad = bytearray(original_module)
    bad[19] = 3
    module.write_bytes(bad)
    run(False)
    module.write_bytes((module.parent / 'guestcore').read_bytes())
    run(False)
    module.write_bytes(original_module)
    manifest.write_bytes(original_manifest.replace(b'/dev/amigaguest 660 0 25 57 0',
                                                   b'/dev/amigaguest 660 0 25 6 0'))
    run(False)
    manifest.write_bytes(original_manifest + b'c /dev/amiga 600 0 3 57 0\n')
    run(False)
finally:
    module.write_bytes(original_module)
    manifest.write_bytes(original_manifest)
run(True)
print('PASS %d staging acceptance/rejection checks; original bytes restored' % checks)
