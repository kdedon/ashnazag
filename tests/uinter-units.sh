#!/bin/sh
# uinter-units.sh -- host-side unit checks of the uinter module's sources.
#
#   sh tests/uinter-units.sh
#
# Exit: 0 all PASS, 1 any FAIL.
T=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$T/.." && pwd)
U=$AUX/kernel/guest/mod/uinter
G=$AUX/kernel/guest/test
M=$T/aux/mac81probe
rc=0
run() {
	if nice -n 19 python3 "$@" > /dev/null 2>&1; then echo "[PASS] ${1##*/}"
	else echo "[FAIL] ${1##*/}"; rc=1; fi
}
run "$G/check-pm-idle.py" "$U"
run "$G/t_coffstackspace.py"
run "$G/t_forceworld.py"
run "$G/t_pmclose.py"
run "$G/t_patchshared.py"
run "$M/check-watch-filter.py" "$U/uipatch.c" "$G/t_uipatch.py"
run "$M/check-watch-owner.py" "$U"
exit $rc
