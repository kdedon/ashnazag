#!/bin/sh
set -e
A=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$A/.." && pwd)
KERNEL=${1:-$K/build/unix-amiga.elf}
O=${2:-$K/build/amiga/package}
mkdir -p "$O/root/etc/conf/mod.d"
sh "$K/guest/build.sh" -m "$KERNEL" "$O/guest"
sh "$K/otbridge/build.sh" -k "$KERNEL" "$O/otbridge"
for n in guestcore auxcore auxexec uinter tosguest amigaguest cpmguest dsuser otbridge; do
 if [ "$n" = otbridge ]; then m="$O/otbridge/mod.d/$n"; else m="$O/guest/mod.d/$n"; fi
 [ -s "$m" ] || { echo "[FAIL] missing required module $n"; exit 1; }
 cp "$m" "$O/root/etc/conf/mod.d/$n"
done
cat > "$O/root.manifest.add" <<'MANIFEST'
d /etc/conf 755 0 3
d /etc/conf/mod.d 755 0 3
c /dev/fb0 660 0 25 51 0
c /dev/kbd 660 0 25 52 0
c /dev/mouse 660 0 25 53 0
c /dev/uinter 660 0 25 54 0
c /dev/otbridge 600 0 3 55 0
c /dev/tos 660 0 25 56 0
c /dev/amigaguest 660 0 25 57 0
c /dev/cpm 660 0 25 59 0
MANIFEST
for m in "$O/root/etc/conf/mod.d/"*; do
 n=$(basename "$m")
 printf 'f /etc/conf/mod.d/%s 644 0 3 etc/conf/mod.d/%s\n' "$n" "$n" >> "$O/root.manifest.add"
done
python3 "$A/test/stage.py" "$KERNEL" "$O"
echo "[OK] staged modules and additive manifest; preserve /dev/amiga major 6"
