#!/bin/sh
set -eu
T=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
AUX=$(CDPATH= cd -- "$T/.." && pwd)
TC=${AMIX_TC:-$AUX/toolchain/amix}
B=${1:-$T/build/nativehost-platform}
NH=$AUX/kernel/guest/nativehost
mkdir -p "$B"
B=$(CDPATH= cd -- "$B" && pwd)
SYS=$TC/m68k-cbm-sysv4/sysroot
LIB=$SYS/usr/ccs/lib
CC=$TC/bin/m68k-cbm-sysv4-gcc
LD=$TC/bin/m68k-cbm-sysv4-ld
mkdir -p "$B/obj" "$B/extra"
"${HOSTCC:-cc}" -std=c89 -Wall -Wextra -Werror -I"$NH" \
    -Duname=nh_test_uname -Dsysconf=nh_test_sysconf \
    -c "$NH/nativehost_platform.c" -o "$B/obj/host-platform.o"
"${HOSTCC:-cc}" -std=c89 -Wall -Wextra -Werror -I"$NH" \
    "$T/nativehost/platform-fixture.c" "$B/obj/host-platform.o" \
    -o "$B/platform-fixture"
"$B/platform-fixture"
for f in "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a; do LIBGCC=$f; done
(cd "$B/extra" && ar x "$LIB/libc.so" && rm -f libc.so.1 &&
    ar rc "$B/libextra.a" ./*.o)
"$CC" -O -Wall -Wno-comment -D__STDC__=0 -I"$NH" \
    -c "$NH/nativehost_platform.c" -o "$B/obj/nativehost_platform.o"
"$CC" -O -Wall -Wno-comment -D__STDC__=0 -I"$NH" -I"$T/src" \
    -c "$T/src/t_nativehost_platform.c" -o "$B/obj/t_nativehost_platform.o"
"$CC" -O -Wall -Wno-comment -I"$T/src" \
    -c "$T/src/t.c" -o "$B/obj/t.o"
"$LD" -o "$B/t_nativehost_platform" "$LIB/crt1.o" "$LIB/crti.o" \
    "$B/obj/t_nativehost_platform.o" "$B/obj/nativehost_platform.o" \
    "$B/obj/t.o" "$SYS/usr/lib/libc.so.1" "$B/libextra.a" \
    "$LIBGCC" "$LIB/crtn.o"
echo "READY $B/t_nativehost_platform"
