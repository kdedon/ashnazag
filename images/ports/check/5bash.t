bash -c 'n=0; for i in 1 2 3; do n=$((n + i)); done; sleep 1 & wait $!; [ $n = 6 ] && [[ abc == a* ]]' &&
	echo bash=0 || echo bash=1
