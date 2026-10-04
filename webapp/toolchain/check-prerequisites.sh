#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
missing=0
for tool in emcc emconfigure emmake make python3 node; do
    if command -v "$tool" >/dev/null 2>&1; then
        printf 'FOUND %s\n' "$tool"
    else
        printf 'MISSING %s\n' "$tool"
        missing=1
    fi
done
for source in ld/ldmain.c bfd/elf32-m68k.c; do
    if [ ! -f "$root/toolchain/src/gcc-cross-amix/src/binutils-2.8.1/$source" ]; then
        printf 'MISSING binutils source: %s\n' "$source"
        missing=1
    fi
done
if [ "$missing" -ne 0 ]; then
    printf 'WASM port prerequisites incomplete. Native probe remains available.\n'
    exit 1
fi
printf 'Prerequisites present; legacy host-port patches and WASM linking still need validation.\n'
