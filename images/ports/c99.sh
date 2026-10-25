#!/bin/sh
# c99.sh -- cc.sh for C99 sources: the m68k Linux gcc compiles against
# the SVR4 headers, cc.sh links.
#
# Its ABI is SVR4's (doubles in fp0, pointers in a0 and d0); -malign-int
# gives SVR4's structure layout, -m68020-60 code the 68060 runs.  The
# system's Linux macros go, the compat headers come before the system ones.
P=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$P/../.." && pwd)
SYS=$AUX/toolchain/amix/m68k-cbm-sysv4/sysroot
GCC=$AUX/toolchain/linux/bin/m68k-linux-gnu-gcc
set -f
F="-nostdinc -isystem $($GCC -print-file-name=include) -isystem $P/compat/include
	-isystem ${SYSINC:-$SYS/usr/include} -m68020-60 -malign-int
	-fno-asynchronous-unwind-tables -fno-stack-protector -fno-pic -fcommon
	-U__linux__ -U__linux -Ulinux -U__gnu_linux__ -U__unix__ -U__ELF__
	-Dm68k -Dunix -D__unix -D__amix -DSVR4 -DSYSV"
mode=link
for a; do
	case $a in
	-c) mode=c ;;
	-E|-S|-M|-MM) mode=pass ;;
	-v|--version|-V*|-dumpversion|-print-*) exec "$GCC" "$@" ;;
	esac
done
[ $mode = link ] || exec "$GCC" $F "$@"

# link: compile the sources here, the rest to cc.sh
n=$# cf= tmp= prev=
for a; do
	case $a in -[IDU]*|-O*|-f*|-m*|-g*|-w|-std=*|-W[!l]*) cf="$cf $a" ;; esac
done
for a; do
	case $prev in -o|-L|-l) set -- "$@" "$a"; prev=; continue ;; esac
	prev=$a
	case $a in
	*.c)	t=${TMPDIR:-/tmp}/c99$$.$(basename "$a" .c).o
		"$GCC" $F $cf -c "$a" -o "$t" || { rm -f $tmp; exit 1; }
		tmp="$tmp $t"; a=$t ;;
	-[IDU]*|-O*|-f*|-m*|-g*|-w|-std=*|-W[!l]*) continue ;;
	esac
	set -- "$@" "$a"
done
shift $n
sh "$P/cc.sh" "$@"
r=$?
[ -z "$tmp" ] || rm -f $tmp
exit $r
