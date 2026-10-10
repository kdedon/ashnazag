#!/bin/sh
# links.sh dir -- X.68K, which the CCP runs, as a hard link to each X.REL
for f in "$1"/*.REL; do
	[ -f "$f" ] || continue
	ln -f "$f" "${f%.REL}.68K"
done
