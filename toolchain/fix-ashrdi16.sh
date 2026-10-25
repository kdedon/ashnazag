#!/bin/sh
# fix-ashrdi16.sh m68k.md -- gcc 2.7 and 2.95 clear the high word of a
# signed 64-bit shift right by 16 before sign-extending it, giving 0.
# Swap it down instead, as later gcc does.
set -e
perl -pi -e 's/(move%\.w %0,%1\x5c;)clr%\.w %0\x5c;(swap %1)\x5c;(ext%\.l %0)/$1swap %0\x5c;$3\x5c;$2/' "$1"
grep -q 'move%\.w %0,%1.;swap %0.;ext%\.l %0.;swap %1' "$1" ||
	{ echo "$1: the ashrdi3 by 16 fix did not apply" >&2; exit 1; }
