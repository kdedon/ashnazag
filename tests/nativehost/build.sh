#!/bin/sh
set -eu
T=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
AUX=$(CDPATH= cd -- "$T/.." && pwd)
TC=${AMIX_TC:-$AUX/toolchain/amix}
CORE=${AMIX_CORE:-$AUX/kernel/mac/ramdisk/build/core}
KERNEL=${KERNEL:-$AUX/kernel/build/unix-mac.elf}
B=${1:-$T/build/nativehost}
mkdir -p "$B"
B=$(CDPATH= cd -- "$B" && pwd)
SYS=$TC/m68k-cbm-sysv4/sysroot
LIB=$SYS/usr/ccs/lib
CC=$TC/bin/m68k-cbm-sysv4-gcc
LD=$TC/bin/m68k-cbm-sysv4-ld
AS=$TC/bin/m68k-cbm-sysv4-as
D=$AUX/kernel/mac/display
DLM=$AUX/kernel/dlm
RD=$AUX/kernel/mac/ramdisk
NH=$AUX/kernel/guest/nativehost
for f in "$CORE/sbin/init" "$CORE/sbin/sh" "$KERNEL" "$CC"; do
    test -f "$f" || { echo "missing $f"; exit 1; }
done
mkdir -p "$B/obj" "$B/extra" "$B/tools"
for f in "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a; do LIBGCC=$f; done
(cd "$B/extra" && ar x "$LIB/libc.so" && rm -f libc.so.1 &&
    ar rc "$B/libextra.a" ./*.o)
cc() {
    nice -n 19 "$CC" -O -Wall -Wno-comment "$@"
}
cc -D__STDC__=0 -I"$NH" -I"$D" -c "$NH/nativehost.c" -o "$B/obj/nativehost.o"
cc -D__STDC__=0 -I"$T/src" -I"$NH" -I"$D" -I"$DLM/include" \
    -c "$T/src/t_nativehost.c" -o "$B/obj/t_nativehost.o"
cc -I"$T/src" -c "$T/src/t.c" -o "$B/obj/t.o"
cc -D__STDC__=0 -I"$T/src" -c "$T/src/runall.c" -o "$B/obj/runall.o"
for f in modload moduload; do
    nice -n 19 "$AS" -o "$B/obj/$f.o" "$DLM/libmod/$f.s"
done
link() {
    output=$1; shift
    nice -n 19 "$LD" -o "$output" "$LIB/crt1.o" "$LIB/crti.o" "$@" \
        "$SYS/usr/lib/libc.so.1" "$B/libextra.a" "$LIBGCC" "$LIB/crtn.o"
}
link "$B/t_nativehost" "$B/obj/t_nativehost.o" "$B/obj/nativehost.o" \
    "$B/obj/t.o" "$B/obj/modload.o" "$B/obj/moduload.o"
link "$B/runall" "$B/obj/runall.o" "$B/obj/t.o"
for f in dsdev dsseg dsuser; do
    cc -traditional -D_KERNEL -DSYSV -Dinline=__inline__ -Dvolatile=__volatile__ \
        -m68040 -fno-common -fno-builtin -I"$D" -I"$AUX/kernel/mac/video" \
        -I"$AUX/kernel/mac/adb" -I"$DLM/include" -c "$D/$f.c" -o "$B/obj/$f.o"
done
nice -n 19 "${HOSTCC:-cc}" -std=gnu89 -O -w -DDLM_TOOL -I"$DLM" \
    -I"$DLM/include" -o "$B/tools/mkksym" "$DLM/tools/mkksym.c" "$DLM/dlm_sym.c"
nice -n 19 "${HOSTCC:-cc}" -std=gnu89 -O -w -o "$B/tools/modfix" "$DLM/tools/modfix.c"
"$B/tools/mkksym" -x "$KERNEL" > "$B/exports"
cat > "$B/obj/moddep.s" <<'ASM'
    .section .moddata,"aw",@progbits
    .balign 4
    .long dsuser_wrapper
    .byte 0
ASM
nice -n 19 "$AS" -o "$B/obj/moddep.o" "$B/obj/moddep.s"
nice -n 19 "$LD" -r -o "$B/dsuser" "$B/obj/dsdev.o" "$B/obj/dsseg.o" \
    "$B/obj/dsuser.o" "$B/obj/moddep.o"
"$B/tools/modfix" -e "$B/exports" -p dsuser -m dsuser "$B/dsuser"
python3 "$D/checkmod.py" "$KERNEL" "$B/dsuser"
cat > "$B/inittab" <<'INIT'
is:s:initdefault:
ap::sysinit:/sbin/autopush -f /etc/ap/chan.ap
tt::sysinit:/tests/runall t_nativehost:35 </dev/console >/dev/console 2>&1
INIT
{
    echo 'display::25:'
    echo 'root::0:'
    echo 'sys::3:'
} > "$B/group"
python3 - "$RD" "$CORE" "$B" <<'PY'
import pathlib, sys
rd, core, build = map(pathlib.Path, sys.argv[1:])
keep = {'/etc/TIMEZONE', '/etc/profile', '/etc/ap/chan.ap', '/etc/ioctl.syscon',
        '/etc/passwd', '/etc/vfstab', '/usr/lib/libc.so.1', '/sbin/init',
        '/sbin/autopush', '/sbin/sh', '/sbin/mount', '/sbin/umount',
        '/usr/lib/fs/proc/mount'}
lines = []
for line in (rd / 'root.manifest').read_text().splitlines():
    fields = line.split()
    if not fields or fields[0].startswith('#'):
        continue
    if fields[0] == 'f':
        if fields[1] not in keep:
            continue
        source = fields[-1]
        fields[-1] = str(core / source[len('build/core/'):]) if source.startswith('build/core/') else str(rd / source)
    elif fields[0] in ('h', 'l') and fields[1] not in {'/dev/syscon', '/dev/systty', '/dev/fb', '/dev/swap', '/etc/sulogin', '/bin', '/usr/bin/sh'}:
        continue
    lines.append(' '.join(fields))
lines += ['d /dev/term 755 0 3', 'c /dev/term/b 620 0 7 0 1',
          'd /tests 755 0 3', 'f /etc/inittab 644 0 3 ' + str(build / 'inittab'),
          'f /etc/group 444 0 3 ' + str(build / 'group')]
for name in ('t_nativehost', 'runall', 'dsuser'):
    lines.append('f /tests/' + name + ' 755 0 3 ' + str(build / name))
(build / 'root.manifest').write_text('\n'.join(lines) + '\n')
PY
nice -n 19 python3 "$RD/mks5fs.py" -s 4096 -i 256 -b 1024 -t 723000000 \
    "$B/root.manifest" "$B/nativehost.img"
python3 "$RD/s5check.py" "$B/nativehost.img" -l > "$B/root.lst"
test "$(wc -c < "$B/nativehost.img")" -le 33554432
echo "READY $B/t_nativehost $B/nativehost.img"
