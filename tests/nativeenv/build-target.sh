#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
A=$(CDPATH= cd -- "$D/../.." && pwd)
TC=${AMIX_TC:-$A/toolchain/amix}
O=${1:?usage: build-target.sh OUTDIR}
mkdir -p "$O"
O=$(CDPATH= cd -- "$O" && pwd)
SYS=$TC/m68k-cbm-sysv4/sysroot
LIB=$SYS/usr/ccs/lib
for LIBGCC in "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a; do
    [ -f "$LIBGCC" ] && break
done
[ -f "$LIBGCC" ]
TC="$TC" sh "$A/kernel/guest/nativeenv/build.sh" "$O"
for name in target payload; do
    nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -Wno-comment -D__STDC__=0 \
        -c "$D/$name.c" -o "$O/$name.o"
    nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$O/nativeenv-$name" \
        "$LIB/crt1.o" "$LIB/crti.o" "$O/$name.o" \
        "$SYS/usr/lib/libc.so.1" "$LIBGCC" "$LIB/crtn.o"
done
{
    echo 'd /tests 755 0 3'
    echo 'd /etc/default 755 0 3'
    echo "f /tests/startnative 755 0 3 $O/startnative"
    echo "f /tests/t_natlaunch 755 0 3 $O/nativeenv-target"
    echo "f /tests/natpayload 755 0 3 $O/nativeenv-payload"
    for family in smsqe lisa voyage200; do
        case $family in
            smsqe) directory=SMSQE ;;
            lisa) directory=Lisa ;;
            voyage200) directory=Voyage200 ;;
        esac
        {
            echo "LEGACY_ROOT=/tmp/natlaunch/$directory"
            echo "ENVIRONMENT_ROOT=/tmp/natlaunch/$directory"
            echo 'ENVIRONMENT_METADATA=.env'
        } > "$O/$family.policy"
        echo "f /etc/default/$family 644 0 3 $O/$family.policy"
    done
} > "$O/fixture.manifest"
python3 - "$O" <<'PYCODE'
from pathlib import Path
import sys
p = Path(sys.argv[1])
size = sum((p / n).stat().st_size for n in ('startnative', 'nativeenv-target', 'nativeenv-payload'))
assert size < 262144, size
print('READY', p / 'nativeenv-target', p / 'fixture.manifest', 'bytes=' + str(size))
PYCODE
