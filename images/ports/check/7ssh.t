# the host logs in with its key; dbclient logs in to this host
sleep 5
ps -e | grep prngd > /dev/null && echo prngd=0 || echo prngd=1
mkdir -p /.ssh; chmod 700 /.ssh; cp /mnt/hostk.pub /.ssh/authorized_keys; chmod 600 /.ssh/authorized_keys
rm -f /tmp/sshok; echo sshwait
n=0; while [ ! -f /tmp/sshok ] && [ $n -lt 240 ]; do sleep 2; n=`expr $n + 2`; done
test -f /tmp/sshok && echo sshin=0 || echo sshin=1
dropbearkey -t ed25519 -f k > /dev/null 2>&1 && dropbearkey -y -f k | grep ssh-ed25519 >> /.ssh/authorized_keys
dbclient -y -i k root@127.0.0.1 echo loop-ok > d.out 2>&1; grep '^loop-ok' d.out > /dev/null && echo dbclient=0 ||
	{ echo dbclient=1; cat d.out; }
rm -rf k d.out /.ssh
