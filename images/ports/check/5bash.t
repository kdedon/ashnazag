bash -c 'n=0; for i in 1 2 3; do n=$((n + i)); done; sleep 1 & wait $!; [ $n = 6 ] && [[ abc == a* ]]' &&
	echo bash=0 || echo bash=1
# bash 4 and 5: associative arrays, case changes, mapfile, 64-bit arithmetic
bash -c 'declare -A a=([x]=1 [y]=2); s=abc; mapfile -t l <<< $'\''p\nq'\''
	[ "${a[y]}${s^^}${#l[@]}" = 2ABC2 ] && [ $((2**40 / 3)) = 366503875925 ] && ((2**33 > 2**32))' &&
	echo bash5=0 || echo bash5=1
# a login's PATH finds the ports ahead of the tape's bash
o=`su - guest -c 'bash --version; vim --version' < /dev/null 2>&1`
case $o in *'version 5.2'*'Vi IMproved 9'*) echo loginpath=0 ;; *) echo loginpath=1 ;; esac
