#!/bin/sh
set -eu
T=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
A=$(CDPATH= cd -- "$T/.." && pwd)
TC=${AMIX_TC:-$A/toolchain/amix}
B=${1:?usage: build-ui.sh OUTDIR}
NH=$A/kernel/guest/nativehost
D=$A/kernel/mac/display
mkdir -p "$B/obj"
B=$(CDPATH= cd -- "$B" && pwd)
nice -n 19 "${HOSTCC:-cc}" -std=c89 -Wall -Wextra -Werror -I"$NH" -I"$D" \
    "$NH/nativehost_ui.c" "$T/nativehost/ui-fixture.c" -o "$B/ui-fixture"
"$B/ui-fixture"
nice -n 19 "${HOSTCC:-cc}" -std=c89 -Wall -Wextra -Werror -I"$NH" -I"$D" \
    -Dopen=nh_test_open -Dclose=nh_test_close -Dmmap=nh_test_mmap \
    -Dmunmap=nh_test_munmap -Dioctl=nh_test_ioctl \
    -c "$NH/nativehost.c" -o "$B/obj/host-display.o"
nice -n 19 "${HOSTCC:-cc}" -std=c89 -Wall -Wextra -Werror -I"$NH" -I"$D" \
    "$T/nativehost/display-fixture.c" "$B/obj/host-display.o" -o "$B/display-fixture"
"$B/display-fixture"
SYS=$TC/m68k-cbm-sysv4/sysroot
LIB=$SYS/usr/ccs/lib
for LIBGCC in "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a; do
    [ -f "$LIBGCC" ] && break
done
[ -f "$LIBGCC" ] || { echo "libgcc.a not found" >&2; exit 1; }
for source in "$NH/nativehost.c" "$NH/nativehost_ui.c" "$T/nativehost/ui-fixture.c"; do
    name=$(basename "$source" .c)
    nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -D__STDC__=0 \
        -I"$NH" -I"$D" -c "$source" -o "$B/obj/$name.o"
done
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$B/t_nativehost_ui" \
    "$LIB/crt1.o" "$LIB/crti.o" "$B/obj/ui-fixture.o" \
    "$B/obj/nativehost_ui.o" "$SYS/usr/lib/libc.so.1" "$LIBGCC" "$LIB/crtn.o"
echo "READY $B/t_nativehost_ui"
