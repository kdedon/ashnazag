# envmedia.sh -- the user's environment media, set up for a disk image as
# the environments' installers would.  Sourced by the image builders.
#
# Each function appends manifest lines: root files to $I, guest's home to
# $IH under $GH (guest's home as that manifest names it).  Work files go in
# $P; G is the environment packages' guest home (mkenvpkgs.sh's).  AUX is
# the tree.  Proprietary inputs stay in the image.

# the user's Windows for Workgroups 3.11 (WFW) and Windows 3.11 (WIN311)
# disks and Wabi 2.2 (WINMEDIA, default media/) in guest's ~/WIN16 and
# ~/WIN16/win311, as startwin -install puts them; WINS names those installed
envwin16() {
	WINMEDIA=${WINMEDIA:-$AUX/media}
	[ -n "$WFW" ] || for WFW in "$WINMEDIA"/*[Ww]orkgroups*3.11*.7z; do break; done
	[ -n "$WIN311" ] || for WIN311 in "$WINMEDIA"/*Windows*3.11*[Rr]etail*.7z; do break; done
	WABI=$WINMEDIA/Wabi/x86/pkgs/SUNWwabi
	SW=$P/sw/startwin
	WINS=
	if [ ! -d "$G/WIN16" ] || [ ! -f "$WABI/pkgmap" ] || { [ ! -f "$WFW" ] && [ ! -f "$WIN311" ]; }; then
		echo "[skip] Win16: no Windows disks or Wabi in $WINMEDIA"
	elif ! sh "$AUX/tests/win16/host.sh" "$P/sw" > "$P/sw.log" 2>&1; then
		echo "[skip] Win16: no startwin for this host: $(tail -1 "$P/sw.log")"
	else
		if [ -f "$WFW" ]; then
			winst "$WFW" ""
			WINS=wfw
			echo "[ok] Win16: Windows for Workgroups 3.11 and Wabi"
		fi
		if [ -f "$WIN311" ]; then
			winst "$WIN311" /win311
			printf 'id=win311\nname=win311\ncreated=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
				> "$G/WIN16/win311/.env"
			WINS="$WINS win311"
			echo "[ok] Win16: Windows 3.11 and Wabi in ~/WIN16/win311"
		fi
		# no build paths left in the guest's settings
		if grep -rl "$P" "$G/WIN16" > "$P/wfw.log"; then
			echo "[FAIL] Win16: build paths in $(cat "$P/wfw.log")"
			exit 1
		fi
	fi
	rm -rf "$P/sw"
}
# winst archive dir: Windows from archive's disk images into dir, C: as
# the guest sees it
winst() {
	rm -rf "$P/wd"
	7z e -y -o"$P/wd" "$1" > /dev/null &&
	"$SW" -C "$G/WIN16$2" -install "$P"/wd/[Dd][Ii][Ss][Kk]*.[Ii][Mm][Gg] "$WABI" > "$P/wfw.log" ||
		{ cat "$P/wfw.log"; exit 1; }
	rm -rf "$P/wd"
	sed -i "s|^Drives.C=$G/WIN16$2|Drives.C=/home/guest/WIN16$2|" "$G/WIN16$2/windows/wabi.ini"
}

# the user's Voyage 200 Flash dump (V200ZIP, default media/v200.zip;
# V200ROM picks the dump) as guest's ~/Voyage200/media
envv200() {
	V200ZIP=${V200ZIP:-$AUX/media/v200.zip}
	[ -f "$V200ZIP" ] || return 0
	mkdir -p "$P/v200"
	unzip -p "$V200ZIP" "${V200ROM:-voyage200v209.rom}" > "$P/v200/media"
	printf 't\t%s/Voyage200\t100\t1\t%s\n' "$GH" "$P/v200" >> "$IH"
	echo "l $GH/Voyage200/native /usr/lib/voyage200/native" >> "$IH"
}

# the user's Lisa ROM and the files of their ProFile image (LISAMEDIA,
# default media/lisa/imported: H.rom, profile.dc42) as guest's ~/Lisa
envlisa() {
	LISAMEDIA=${LISAMEDIA:-$AUX/media/lisa/imported}
	[ -f "$LISAMEDIA/H.rom" ] && [ -f "$LISAMEDIA/profile.dc42" ] || return 0
	HOME=$P/lisa sh "$AUX/tools/makelisa.sh" office "$LISAMEDIA/profile.dc42"
	cp "$LISAMEDIA/H.rom" "$P/lisa/Lisa/lisa.rom"
	printf 't\t%s/Lisa\t100\t1\t%s\n' "$GH" "$P/lisa/Lisa" >> "$IH"
	echo "l $GH/Lisa/native /usr/lib/lisa/native" >> "$IH"
	echo "l $GH/Lisa/media lisa.rom" >> "$IH"
}

# the user's OPENSTEP 4.2 CD (NEXTCD, default media/openstep's) as
# guest's ~/NeXT/openstep, as makenext.sh installs it
envnext() {
	NEXTCD=${NEXTCD:-$AUX/media/openstep/Openstep-4.2-Intel-User.iso}
	[ -f "$NEXTCD" ] || return 0
	HOME=$P/next sh "$AUX/tools/makenext.sh" openstep "$NEXTCD" > "$P/next.log" 2>&1 ||
		{ tail "$P/next.log"; exit 1; }
	printf 't\t%s/NeXT\t100\t1\t%s\n' "$GH" "$P/next/NeXT" >> "$IH"
	echo "l $GH/NeXT/openstep/native /usr/lib/next/native" >> "$IH"
}

# the Amiga environment's Kickstart, floppies and SYS: template from
# AMIGASTAGE (images/amigaenv/mkamiga.sh's output)
envamiga() {
	[ -n "$AMIGASTAGE" ] || return 0
	S=$AMIGASTAGE/root
	echo "f /etc/amiga/kicka4000.rom 640 0 25 $S/etc/amiga/kicka4000.rom" >> "$I"
	for f in "$S"/amiga/media/*.adf "$S/amiga/media/manifest.json"; do
		echo "f ${f#$S} 640 0 25 $f"
	done >> "$I"
	for f in "$S"/amiga/rtg/Picasso96.*; do
		[ ! -f "$f" ] || echo "f ${f#$S} 644 0 3 $f"
	done >> "$I"
	cp -R "$S/amiga/sys" "$P/amigasys"
	sh "$AUX/images/amigaenv/refresh-sys.sh" "$P/amigasys" || exit 1
	chmod -R a+rX,go-w,u+w "$P/amigasys"
	# guest's ~/Amiga, as startamiga makes it
	printf 't\t%s\t%s\t%s\t%s\n' /amiga/sys 0 3 "$P/amigasys" >> "$I"
	printf 't\t%s/Amiga\t%s\t%s\t%s\n' "$GH" 100 1 "$P/amigasys" >> "$IH"
}

# A/UX's Mac files (AUXROOT) with 7.6.1 (CD761), Mac OS 8.1 from CD81 as
# environment 8.1's template and, with SYS6=1, System 6 from the A/UX
# 2.0.1 CD (CD201) on the IIci's ROM (AUXROM6); A/UX's shared libraries,
# File ID daemon and, if given, the ROM file AUXROM; guest's System Folders
envmac() {
	cat >> "$I" <<EOF2
d /shlib 755 0 3
f /shlib/libc1_s 755 0 3 $AUXROOT/shlib/libc1_s
f /shlib/libmac1_s 755 0 3 $AUXROOT/shlib/libmac1_s
f /etc/aux/fidd 755 0 3 $AUXROOT/etc/fidd
EOF2
	if [ -n "$AUXROM" ]; then
		cp "$AUXROM" "$P/aux.rom"
		echo "f /etc/aux/rom 444 0 3 $P/aux.rom" >> "$I"
	fi
	M=$P/macroot
	sh "$AUX/images/macenv/mkmacroot.sh" "$AUXROOT" "$CD761" "$M"
	if [ -f "$CD81" ]; then
		S81="$M/mac/lib/env/8.1/System Folder"
		FULL=1 sh "$AUX/images/macenv/mksys81.sh" "$CD81" "$S81" "$AUXROOT"
		[ ! -d "$P/network/Extensions" ] || cp -rp "$P/network/Extensions/." "$S81/Extensions/"
		cp "$S81/Finder" "$S81/../RootFinder"
		cp "$S81/%Finder" "$S81/../%RootFinder"
		python3 "$AUX/images/macenv/userfinder.py" "$S81/%Finder"
		(cd "$S81" && find . -type f ! -name .stamp -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) |
			sha256sum | cut -c1-16 > "$S81/.stamp"
	fi
	if [ "$SYS6" != 1 ]; then :
	elif [ -f "$CD201" ] && [ -f "$AUXROM6" ]; then
		sh "$AUX/images/macenv/mksys6.sh" "$CD201" "$P/sys6"
		cp -rp "$P/sys6/." "$M/"
		mkdir -p "$M/usr/aux/aux2/etc/aux"
		cp "$AUXROM6" "$M/usr/aux/aux2/etc/aux/rom"
	else
		echo "[warn] no System 6: needs $CD201 and $AUXROM6"
	fi
	find "$M" -type d -exec chmod 755 {} +
	chmod 700 "$M/.mac"
	find "$M" -type f -perm -u+x -exec chmod 755 {} +
	find "$M" -type f ! -perm -u+x -exec chmod 644 {} +
	# /usr and its directories keep the manifest's owners
	for f in "$M"/* "$M"/.mac; do
		[ "$f" = "$M/usr" ] || printf 't\t/%s\t0\t3\t%s\n' "${f#$M/}" "$f"
	done >> "$I"
	[ ! -d "$M/usr/aux/aux2" ] || printf 't\t/usr/aux/aux2\t0\t3\t%s\n' "$M/usr/aux/aux2" >> "$I"
	printf 't\t%s/System Folder\t100\t1\t%s\n' "$GH" "$M/mac/lib/System Folder" >> "$IH"
	[ ! -d "$M/mac/lib/env/6" ] || envsys6home "$M/mac/lib/env/6/System Folder"
	echo "f /usr/bin/systemfolder 755 0 3 $M/usr/bin/systemfolder" >> "$I"
	echo "f /usr/lib/updtsysfldr 755 0 3 $M/usr/lib/updtsysfldr" >> "$I"
}

# guest's ~/Mac/6 from System 6's template, as the first startmac -e 6
# makes it
envsys6home() {
	mkdir -p "$P/mac6"
	printf 'id=6\nname=6\nprofile=mac\ntemplate=/mac/lib/env/6/System Folder\ncreated=%s\n' \
		"$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$P/mac6/.env"
	echo "d $GH/Mac 755 100 1" >> "$IH"
	echo "d $GH/Mac/6 755 100 1" >> "$IH"
	printf 't\t%s/Mac/6/System Folder\t100\t1\t%s\n' "$GH" "$1" >> "$IH"
	echo "f $GH/Mac/6/.env 644 100 1 $P/mac6/.env" >> "$IH"
}
