#!/bin/sh
# cached.sh -- the ports' datastreams for the image builders: prints their
# paths, building them with mkports.sh into PORTS (default
# images/work/ports) when their sources changed.
set -e
P=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$P/../.." && pwd)
C=${PORTS:-$AUX/images/work/ports}
s=$(cd "$AUX" && { find images/ports -type f ! -path '*/check/*' | sort | xargs md5sum;
	md5sum images/vendor.manifest images/envpkg/pkgmk.py; } | md5sum | cut -d' ' -f1)
if [ "$(cat "$C/stamp" 2> /dev/null)" != "$s" ]; then
	rm -rf "$C.new.$$"
	sh "$P/mkports.sh" "$C.new.$$" > "$C.log" 2>&1 || { tail "$C.log" >&2; exit 1; }
	rm -rf "$C.new.$$/w" "$C.new.$$"/*.cat
	echo "$s" > "$C.new.$$/stamp"
	rm -rf "$C"
	mv "$C.new.$$" "$C"
fi
ls "$C"/pkgs/*.pkg
