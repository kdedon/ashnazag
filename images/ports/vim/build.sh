#!/bin/sh
# build.sh -- cross-build Vim 9.2 for /opt/amix: normal features, no
# GUI, terminfo through the system's termlib.
#
#   sh images/ports/vim/build.sh outdir
#
# The runtime keeps the common file types (LANGS); help is in vimdoc.
# Files whose names exceed 14 bytes are left out.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
PORT=vim OUT=$1
. "$AUX/images/ports/port.sh"
CC="sh $PD/c99.sh"
# file types with syntax, filetype plugin and indent files, and the ones
# they include
LANGS="asm asm68k awk c changelog cmake conf config cpp crontab csh css ctrlh
	diff dircolors dosini dtd fortran fstab git gitcommit gitconfig gitrebase go
	group help html initex java javaid javascript json lex lisp lua lynx m4 mail
	make man markdown messages nroff pascal passwd perl plaintex pod python
	python2 qf readline ruby rust scheme screen sed sh sql sqloracle sudoers tags
	tcl terminfo tex texinfo text vb vim wget xdefaults xf86conf xml xmodmap yacc
	yaml zsh"

# the full SVR4 namespace at __STDC__ = 1
cp -R "$SYS/usr/include" "$W/include"
grep -rl '__STDC__ *[-=]' "$W/include" | xargs sed -i \
	-e 's/__STDC__ - 0 == 0/!defined(__STRICT_ANSI__)/g' \
	-e 's/__STDC__ == 0/!defined(__STRICT_ANSI__)/g'
SYSINC=$W/include
export SYSINC

S=$W/$(src vim)
cd "$S/src"
# a cross configure cannot run its tests; these are the SVR4 answers
log conf env CC="$CC" CFLAGS="-O2 -fomit-frame-pointer -std=gnu99 -fno-builtin-floor -fno-builtin-ceil" PKG_CONFIG=false \
	vim_cv_toupper_broken=no vim_cv_terminfo=yes vim_cv_tgetent=zero \
	vim_cv_getcwd_broken=no vim_cv_stat_ignores_slash=no \
	vim_cv_memmove_handles_overlap=yes vim_cv_timer_create=no \
	vim_cv_uname_output=UNIX_System_V vim_cv_uname_r_output=4.0 vim_cv_uname_m_output=m68k \
	./configure $HOST --with-features=normal --with-tlib=termlib \
	--with-compiledby=ashnazag --enable-gui=no --without-x --without-wayland \
	--without-local-dir --disable-netbeans --disable-channel --disable-terminal \
	--disable-gpm --disable-sysmouse --disable-acl --disable-selinux --disable-smack \
	--disable-xattr --disable-xsmp --disable-canberra --disable-libsodium --disable-nls \
	--disable-largefile
sed -i '/PROC_EXE_LINK/d' auto/config.h
# the declarations after configure, whose probes declare "char f();"
sed -i "s|^CC\([[:space:]]*\)= .*|& -include $PD/compat/include/port.h -include $D/c99fix.h|" auto/config.mk
$CC -O2 -c "$D/ll.c" -o "$W/ll.o"
# not in osdef.sh's view, which would declare sprintf again
sed -i -e "s|^LDFLAGS\([[:space:]]*\)= |&$W/ll.o |" \
	-e "s|^CFLAGS\([[:space:]]*\)= |&-Dsprintf=llsprintf |" auto/config.mk
log make $NICE make -j1
P=$ROOT/opt/amix
R=$P/share/vim
mkdir -p "$P/man/man1" "$P/doc/vim" "$R/vim92"
cp vim xxd/xxd "$P/bin/"
cp "$D/vimrc" "$R/"
cp ../runtime/doc/vim.1 ../runtime/doc/xxd.1 "$P/man/man1/"
cp ../LICENSE ../README.md "$P/doc/vim/"
cd ../runtime
tar cf - --exclude=testdir ./*.vim autoload colors compiler import macros plugin \
	syntax/synload.vim syntax/syntax.vim syntax/nosyntax.vim syntax/manual.vim \
	syntax/syncolor.vim $(for d in syntax ftplugin indent; do
		for l in $LANGS; do [ ! -f $d/$l.vim ] || echo $d/$l.vim; done; done) |
	(cd "$R/vim92" && tar xf -)
cd "$R/vim92"
rm -f ./*menu.vim evim.vim gvimrc_example.vim mswin.vim macmap.vim
# it calls into an autoload file whose name exceeds 14 bytes
rm ftplugin/html.vim
find . -name '???????????????*' -exec rm -rf {} +
# float formats, the float functions
FPU=vim
portpkg
