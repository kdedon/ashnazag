# the host serves the release directory over HTTP
wget -q -O c http://$H/catalog && cmp c /mnt/rel/catalog && echo wget=0 || echo wget=1
TERM=vt100 lynx -dump http://$H/ > l.out; grep catalog.md5 l.out > /dev/null && echo lynx=0 || echo lynx=1
rm -f c l.out
