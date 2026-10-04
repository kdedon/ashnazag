#!/bin/sh
# test.sh -- check coff2elf.
#
# i386: COFF fixtures -> coff2elf -> gnu ld + libcoh crt0 -> run on the host.
#   (1) main returns 42, no relocations; (2) a R_DIR32 reloc to .data.
#   Skipped when libcoh could not be built (no gcc -m32).
# m68k: convert A/UX executables, kernels, driver modules and shared
#   libraries from $AUXROOT; readelf must accept each output, and check.py
#   compares sections, symbols, relocations and segments with the COFF.
#   Skipped when AUXROOT is unset.
#
# Run it as `make test'.  Work happens in a scratch directory.
HERE=$(cd "$(dirname "$0")" && pwd)
B=$(cd "${B:-$HERE/build}" && pwd)
W=$B/t.$$
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT INT TERM
cd "$W" || exit 2

fail=0
chk() {	# chk <label> <got> <want>
	if [ "$2" = "$3" ]; then echo "PASS  $1 (=$2)"
	else echo "FAIL  $1: got '$2' want '$3'"; fail=1; fi
}

if [ -f "$B/crt0.o" ] && [ -f "$B/libcoh.a" ]; then
	echo "== i386 case 1: main returns 42, no relocations =="
	"$B/mkfix" t1.coff
	"$B/coff2elf" -u t1.coff t1.elf.o
	readelf -s t1.elf.o | grep -q ' main$' || { echo "FAIL  no 'main'"; fail=1; }
	ld -m elf_i386 -e _start -o t1.prog "$B/crt0.o" t1.elf.o "$B/libcoh.a" 2>/dev/null
	./t1.prog; rc=$?
	chk "case1 exit code" "$rc" "42"

	echo "== i386 case 2: R_DIR32 reloc to a .data symbol =="
	"$B/mkfix" -r t2.coff
	"$B/coff2elf" -u t2.coff t2.elf.o
	readelf -r t2.elf.o | grep -q 'R_386_32' || { echo "FAIL  no R_386_32"; fail=1; }
	ld -m elf_i386 -e _start -o t2.prog "$B/crt0.o" t2.elf.o "$B/libcoh.a" 2>/dev/null
	# main loads the first 4 bytes of "Hi\0" = 0x6948; exit keeps the low byte
	./t2.prog; rc=$?
	chk "case2 exit code" "$rc" "$((0x6948 & 0xff))"
else
	echo "SKIP  i386 (no libcoh: needs gcc -m32)"
fi

if [ -n "$AUXROOT" ]; then
	echo "== m68k: A/UX files under $AUXROOT =="
	for f in unix etc/config.d/newunix \
	    etc/boot.d/ddp etc/boot.d/elap etc/boot.d/at_atp etc/boot.d/at_sig \
	    etc/boot.d/adsp etc/boot.d/toolbox lib/crt0.o \
	    mac/bin/startmac bin/sh shlib/libc1_s shlib/libmac1_s; do
		o=$(basename "$f").elf
		if ! "$B/coff2elf" "$AUXROOT/$f" "$o"; then
			echo "FAIL  $f: coff2elf"; fail=1; continue
		fi
		if readelf -a "$o" 2>&1 >/dev/null | grep -q .; then
			echo "FAIL  $f: readelf complains"; fail=1
		fi
		python3 "$HERE/check.py" "$AUXROOT/$f" "$o" | sed "s|$AUXROOT/||" \
			| grep -q '^ok' && echo "PASS  $f" \
			|| { echo "FAIL  $f"; python3 "$HERE/check.py" \
			    "$AUXROOT/$f" "$o"; fail=1; }
		if command -v m68k-linux-gnu-objdump >/dev/null &&
		    ! m68k-linux-gnu-objdump -dr "$o" >/dev/null; then
			echo "FAIL  $f: objdump"; fail=1
		fi
	done

	echo "== m68k: startmac with its shared libraries (-l) =="
	"$B/coff2elf" -l -R "$AUXROOT" "$AUXROOT/mac/bin/startmac" sm.elf \
	    2>/dev/null
	n=$(readelf -lW sm.elf | grep -c '^  LOAD')
	chk "startmac -l PT_LOADs (own text, data+bss, 2 libs x 2)" "$n" "6"
	n=$(readelf -SW sm.elf | grep -c '\.text\.lib')
	chk "startmac -l library text sections" "$n" "2"
else
	echo "SKIP  m68k (set AUXROOT to an extracted A/UX root)"
fi

exit $fail
