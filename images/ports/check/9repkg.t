apkg -c $c.h install bzip2 < /dev/null > /tmp/i.out 2>&1 && pkginfo -q bzip2 &&
	echo hello | bzip2 | bzip2 -d | grep hello > /dev/null && echo repkg=0 || { echo repkg=1; tail -5 /tmp/i.out; }
