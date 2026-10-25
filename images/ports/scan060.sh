#!/bin/sh
# scan060.sh -- count 68060-unimplemented integer instructions (64-bit
# mul/div, cas2, chk2, cmp2) and FPU instructions in m68k ELF files.
#
#   sh images/ports/scan060.sh file...
#
# Prints "file int060 fpu" per file.  Encodings are read, not mnemonics:
# MUL.L/DIV.L (opword 4c00-4c7f) with extension bit 10 set is the 64-bit form.
OD=${OBJDUMP:-$(cd "$(dirname "$0")/../.." && pwd)/toolchain/linux/bin/m68k-linux-gnu-objdump}
for f; do
	"$OD" -d "$f" 2>/dev/null | awk -F'\t' -v f="$f" '
	function hex(s,   i, v) {
		v = 0; s = tolower(s)
		for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
		return v
	}
	NF >= 3 {
		n = split($2, w, " ")
		if (n < 1 || length(w[1]) != 4) next
		op = hex(w[1])
		if (op >= 19456 && op <= 19583 && n >= 2 && int(hex(w[2]) / 1024) % 2 == 1) { bad++; if (V) print "  " $0 }
		else if ($3 ~ /^(cmp2|chk2|cas2)/) { bad++; if (V) print "  " $0 }
		if (op >= 61952 && op <= 62463) { fp++; if (V > 1) print "  " $0 }
	}
	END { print f, bad + 0, fp + 0 }' V="${V:-0}"
done
