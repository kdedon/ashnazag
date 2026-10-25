#!/bin/sh
# mkgcc.sh -- the toolchain's gcc driver, with asmfix.pl run on its output.
#
#   sh gl/mkgcc.sh OUT
set -e
D=$(cd "$(dirname "$0")" && pwd)
TC=$(cd "$D/.." && pwd)/toolchain/amix
sed -e "s,^bindir=.*,bindir=$TC/bin," -e "/^fix_asm()/{n;a\\
	perl -0777 -pi '$D/asmfix.pl' \"\$1\"
}" "$TC/bin/m68k-cbm-sysv4-gcc" > "$1.fix"
chmod 755 "$1.fix"
# asmfix.pl exits 3 for a function it can fix only with a frame pointer
cat > "$1" <<!
#!/bin/sh
"$1.fix" "\$@" && exit 0 || s=\$?
[ \$s -eq 3 ] || exit \$s
exec "$1.fix" "\$@" -fno-omit-frame-pointer
!
chmod 755 "$1"
