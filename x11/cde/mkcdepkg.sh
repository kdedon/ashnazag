#!/bin/sh
# mkcdepkg.sh -- the CDE package datastream from package-cde.sh's tree.
#
#   sh x11/cde/mkcdepkg.sh cdew
#
# In: cdew/work/dtroot, cdew/x11/xc.  Out: cdew/CDE.pkg (PKG cde), /usr/dt
# with fonts and Xlib's databases, and S92dt, which does at each boot what
# AMIX-INSTALL.sh does once.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
W=$(cd "${1:?usage: mkcdepkg.sh cdew}" && pwd)
R=$W/pkgroot
P=$W/pkgproto
XC=$W/x11/xc
PATH=${GCC14:-$AUX/toolchain/gcc14}/bin:$PATH
rm -rf "$R" "$P"
mkdir -p "$P"
cp -R "$W/work/dtroot" "$R"
chmod -R u+w "$R"
rm -f "$R/MISSING" "$R/usr/dt/AMIX-INSTALL.sh"
# Xlib's databases, found through XKEYSYMDB (0005.fonts)
mv "$R/usr/x11r6/lib/X11/XKeysymDB" "$R/usr/x11r6/lib/X11/XErrorDB" "$R/usr/dt/config/"
rm -rf "$R/usr/x11r6"
find "$R/usr/dt" -type f | while read -r f; do
	case $(head -c 4 "$f" | od -An -c | tr -d ' ') in
	177ELF) m68k-elf-strip "$f" ;;
	esac
done
# ptypes preprocessed here: the target has no cpp
for f in "$R"/usr/dt/appconfig/tttypes/src/*.ptype; do
	/usr/bin/cpp -P -undef -traditional -I"$W/work/cde-2.2.4a/programs/tttypes" "$f" > "$f.i"
	mv "$f.i" "$f"
done
# Courier, Helvetica, Times, Lucida and Symbol at 75 and 100 dpi, which
# CDE's font aliases name
for dpi in 75 100; do
	o=$R/usr/dt/config/xfonts/${dpi}dpi
	mkdir -p "$o"
	for b in "$XC"/fonts/bdf/${dpi}dpi/cour*.bdf "$XC"/fonts/bdf/${dpi}dpi/helv*.bdf \
		"$XC"/fonts/bdf/${dpi}dpi/tim*.bdf "$XC"/fonts/bdf/${dpi}dpi/lu*.bdf \
		"$XC"/fonts/bdf/${dpi}dpi/symb*.bdf; do
		bdftopcf -o "$o/$(basename "$b" .bdf).pcf" "$b"
	done
	mkfontdir "$o"
done
# Xlib's C and ISO 8859-1 locale databases, preprocessed as X installs them
L=$R/usr/dt/config/xlocale
mkdir -p "$L/C" "$L/iso8859-1"
nls() {
	/usr/bin/cpp -P -undef -traditional -DWCHAR32=1 < "$XC/nls/$1" |
		sed -e '/^XCOMM$/s//#/' -e '/^XCOMM[^a-zA-Z0-9_]/s/^XCOMM/#/' > "$L/$2"
}
nls locale.alias locale.alias
nls locale.dir locale.dir
nls compose.dir compose.dir
nls XLC_LOCALE/C C/XLC_LOCALE
nls XLC_LOCALE/iso8859-1 iso8859-1/XLC_LOCALE
cp "$XC/nls/Compose/iso8859-1" "$L/iso8859-1/Compose"
# one connection held for the session: the server resets, font path
# included, whenever its last client leaves
m68k-elf-gcc -O2 -I"$XC/exports/include" -o "$R/usr/dt/bin/xhold" "$W/cde-amix/target/extra/xhold.c" \
	"$XC/exports/lib/libX11.a" -lsocket -lnsl
m68k-elf-strip "$R/usr/dt/bin/xhold"
mkdir -p "$R/usr/dt/config/Xsession.d"
cp "$D/0005.xenv" "$R/usr/dt/config/Xsession.d/0005.xenv"
mkdir -p "$R/etc/rc2.d"
cp "$D/S92dt" "$R/etc/rc2.d/S92dt"
cp "$D/S92dt" "$P/postinstall"

# prototype: owner bin as AMIX-INSTALL.sh sets it, set-id bits as CDE's
# install databases have them
{
	echo 'i pkginfo=pkginfo'
	echo 'i postinstall=postinstall'
	echo 'f none /etc/rc2.d/S92dt 0744 root sys'
	(cd "$R" && find usr -mindepth 1 | sort) | while read -r f; do
		if [ -L "$R/$f" ]; then
			echo "s none /$f=$(readlink "$R/$f")"
		elif [ -d "$R/$f" ]; then
			echo "d none /$f 0755 bin bin"
		else
			case /$f in
			/usr/dt/bin/dtaction) m=6555 o=root ;;
			/usr/dt/bin/dtappgather|/usr/dt/bin/dtprintinfo|/usr/dt/bin/dtsession|/usr/dt/bin/dtterm)
				m=4555 o=root ;;
			*) o=bin; if [ -x "$R/$f" ]; then m=0555; else m=0444; fi ;;
			esac
			echo "f none /$f $m $o bin"
		fi
	done
} > "$P/prototype"
v=$(sed -n 's/^VERSION=//p' "$D/pkginfo")
cp "$D/pkginfo" "$P/pkginfo"
python3 "$AUX/images/envpkg/pkgmk.py" -r "$R" "$P/prototype" "$W/CDE.pkg" PSTAMP=ashnazag-cde-$v
du -sk "$R/usr/dt"
ls -l "$W/CDE.pkg"
