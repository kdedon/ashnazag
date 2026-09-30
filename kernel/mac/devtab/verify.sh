#!/bin/sh
# verify.sh -- static checks of the Mac device tables and disk root policy.
#
#   sh kernel/mac/devtab/verify.sh [kernel-dir]
#
# Run after build.sh.  Dumps cdevsw/bdevsw/io_*/fmodsw of build/unix-mac.elf
# against the base, checks that no Amiga hardware code is reachable, and
# runs the host test of the root policy.  Writes only a temporary directory.
set -e

K=$(cd "${1:-$(dirname "$0")/../..}" && pwd)
AUX=$(cd "$K/.." && pwd)
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

rc=0
nice -n 19 python3 "$K/mac/devtab/checkimg.py" "$K/build/unix-mac.elf" \
	"$K/amix-040-060-port/build/unix-040" "$K/build/mac/base.weak" || rc=1

sed -n '/^\/\* -* root and swap \*\/$/,/^bi_parse()$/p' "$K/mac/macconf.c" |
	sed '$d' | sed '$d' > "$T/root.c"
nice -n 19 cc -std=gnu89 -w -I"$K/mac/devtab" -I"$T" -o "$T/hosttest" \
	"$K/mac/devtab/test/hosttest.c" "$K/mac/devtab/macroot.c"
"$T/hosttest" "$K/mac/ramdisk/build/root.img" || rc=1
exit $rc
