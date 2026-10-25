#!/bin/sh
# build.sh -- cross-build bash 3.2.57 for /opt/amix: job control, the
# system's malloc, the bundled readline, the compat library and passwd
# parsers linked in.
#
#   sh images/ports/bash/build.sh outdir
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
PORT=bash OUT=$1
. "$AUX/images/ports/port.sh"
CFLAGS="-O -Wall"

# the headers with their full SVR4 namespace (sigaction, sigsetjmp, fileno,
# timeval) at __STDC__ = 1, as the native cc -Xa sees them
cp -R "$SYS/usr/include" "$W/include"
grep -rl '__STDC__ *[-=]' "$W/include" | xargs sed -i \
	-e 's/__STDC__ - 0 == 0/!defined(__STRICT_ANSI__)/g' \
	-e 's/__STDC__ == 0/!defined(__STRICT_ANSI__)/g'
SYSINC=$W/include
export SYSINC

mv "$W/$(src bash)" "$W/src"
cd "$W/src"
# the build-host tools (mksyntax ...) include config.h too
sed -i 's/^#undef \(ssize_t\|socklen_t\)$/#ifndef CROSS_COMPILING\n&\n#endif/' config.h.in
# the bundled termcap reads the package's file; sys/file.h wants sys/types.h
sed -i -e 's,"/etc/termcap","/opt/amix/etc/termcap",' \
	-e 's,^#  include <sys/file.h>,#  include <sys/types.h>\n&,' lib/termcap/termcap.c
# the shipped configure, not one autoconf remakes and reruns without config.site
touch config.h.in configure
# a cross configure cannot run its tests; these are the SVR4 answers
CONFIG_SITE=$D/config.site CC=$CC CPP="$CC -E" CFLAGS=$CFLAGS AR=m68k-linux-gnu-ar \
	RANLIB=m68k-linux-gnu-ranlib $NICE ./configure $HOST \
	--without-bash-malloc --disable-nls --enable-job-control --disable-largefile \
	> "$W/configure.log" 2>&1 || { tail "$W/configure.log"; exit 1; }
for d in JOB_CONTROL HAVE_POSIX_SIGNALS HAVE_POSIX_SIGSETJMP HAVE_GETPWENT; do
	grep -q "^#define $d 1" config.h || { echo "[FAIL] configure: no $d"; exit 1; }
done
! grep -q '^#define USING_BASH_MALLOC' config.h || { echo "[FAIL] bash malloc"; exit 1; }
# getcwd(0, PATH_MAX) allocates on SVR4: keep libc's
sed -i 's/defined (GETCWD_BROKEN) && !defined (SOLARIS)/defined (GETCWD_BROKEN) \&\& 0/' config-bot.h
# the bundled termcap by -l; signames.o where make looks for it
sed -i -e 's|^TERMCAP_LIB = .*|TERMCAP_LIB = -ltermcap|' \
	-e 's|^\t$(CC) $(CCFLAGS) -c $(SUPPORT_SRC)signames.c$|& -o $@|' Makefile
# its host tools link without the target's network libraries
sed -i 's/^LIBS = .*/LIBS =/' builtins/Makefile
nice -n 19 make -j1 > "$W/make.log" 2>&1 || { tail -30 "$W/make.log"; exit 1; }

P=$ROOT/opt/amix
mkdir -p "$P/bin" "$P/etc" "$P/man/man1" "$P/doc/bash"
cp "$D/termcap" "$P/etc/"
cp bash "$P/bin/bash"
cp doc/bash.1 "$P/man/man1/"
cp COPYING "$P/doc/bash/"
# printf's %e %f %g
FPU=bash
portpkg
