#!/bin/sh
# relnotes.sh list -- the unstable prerelease's text from mkmodpkgs.sh's
# package list (target|file|name|modules|needs lines).
L=$1
cat <<'EOF'
Rolling build of main: the kernel's hardware modules as SVR4 packages. Install one with `pkgadd -d FILE all` (as root), remove it with `pkgrm NAME`. Each file is for one kernel: `mac` Mac (68040), `ami` Amiga, `at30` Atari 68030, `at60` Atari 68060; `am*` files are Amiga cards.

Most users want their host's `disk` and `cdfs` packages. The rest are optional drivers: install those for hardware you have. The environments (Mac, TOS, Amiga, ...) and their modules come as environment packages with the disk images, not here.
EOF
for t in mac amiga atari030 atari060; do
	grep -q "^$t|" "$L" || continue
	case $t in
	mac) h="Mac (mac)" ;; amiga) h="Amiga (ami, am*)" ;;
	atari030) h="Atari 68030 (at30)" ;; atari060) h="Atari 68060 (at60)" ;;
	esac
	printf '\n### %s\n\n| File | What | Needs |\n|---|---|---|\n' "$h"
	grep "^$t|" "$L" | while IFS='|' read t f n m d; do
		printf '| `%s` | %s (`%s`) | %s |\n' "$f" "$n" "$m" "${d:--}"
	done
done
