dd if=/usr/lib/libc.so.1 of=d bs=1k count=700 2>/dev/null
bzip2 -k -v d && bzip2 -t d.bz2 && bunzip2 -c d.bz2 | cmp - d && bzcat d.bz2 > d1 && cmp d d1 &&
	echo bzip2=0 || echo bzip2=1
rm -f d1 d.bz2
