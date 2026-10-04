#!/usr/bin/env python3
# mkdesktop.py -- the Finder's desktop database on a Mac disk image: boot
# it in QEMU (q800, 128 MB), run startmac as root until the Finder has
# rebuilt the desktop of /, open the System Folder's folders from the
# keyboard so their bundles go in too, stop it, copy the database to
# guest's System Folder, halt.  The image is changed in place.
#
#   python3 images/macenv/mkdesktop.py IMG [ROM]
#
# ROM: a Quadra 800 ROM image (default "Quadra 800.ROM" beside the
# repository).  Exit 0 when the database was written.
import fcntl, json, os, socket, subprocess, sys, tempfile, threading, time

AUX = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
Q = os.path.join(AUX, 'toolchain', 'qemu-local' if os.path.exists(os.path.join(AUX, 'toolchain', 'qemu-local', 'usr', 'bin', 'qemu-system-m68k')) else 'qemu')
img = sys.argv[1]
rom = sys.argv[2] if len(sys.argv) > 2 else os.path.join(AUX, 'Quadra 800.ROM')
# one QEMU at a time, as tests/run-qemu.sh and images/qemu/run-mac.sh
os.makedirs(os.path.join(AUX, 'images', 'work'), exist_ok=True)
lock = open(os.path.join(AUX, 'images', 'work', '.qemu.lock'), 'a')
fcntl.flock(lock, fcntl.LOCK_EX)
w = tempfile.mkdtemp(prefix='mkdesktop.')
log, sock = os.path.join(w, 'serial.log'), os.path.join(w, 'serial.sock')
qsock = os.path.join(w, 'qmp.sock')

# Desktop DB and DF unchanged for a minute: the rebuild is over.  The
# rebuild leaves out bundles the Finder adds as it shows their folder, so
# the host opens those folders between two such waits, then sends a
# line; a database still small means they did not open.  Short
# lines: the tty takes at most 256 characters a line.
GUEST = """cat > /tmp/mkdt << 'E'
S="/mac/sys/System Folder"
cd /
startmac > /dev/null 2>&1 &
p=$!
w() {
o= n=0
while sleep 20; do
s=`ls -l "$S/Desktop DB" "$S/Desktop DF" 2> /dev/null`
if [ -n "$s" ] && [ "$s" = "$o" ]; then n=`expr $n + 1`; else n=0; fi
o=$s
[ $n -ge 3 ] && break
done
}
w; echo FINDER""-UP; read x; w
kill $p; sleep 5
[ `wc -c < "$S/Desktop DB"` -gt 8192 ] || n=0
G="/home/guest/System Folder"
for f in "Desktop DB" "Desktop DF" "%Desktop DB" "%Desktop DF"; do
for d in "$G" "/mac/lib/System Folder"; do
[ ! -d "$d" ] || [ ! -f "$S/$f" ] || cp "$S/$f" "$d/" || n=0
done
done
[ ! -d "$G" ] || { chown 100 "$G"/*Desktop?D?; chgrp 1 "$G"/*Desktop?D?; }
rm -f /tmp/mkdt; sync; echo DESKTOP-$n-DONE
E
sh /tmp/mkdt
"""

# type-select and Command-O down from /, Command-W back up
FOLDERS = ['/', 'mac', 'sys', 'System Folder', 'Control Panels', '^W', 'Extensions', '^W',
	'Apple Menu Items', '^W']
QCODE = {' ': 'spc', '/': 'slash'}

def finder_keys():
	q = socket.socket(socket.AF_UNIX)
	q.settimeout(60)
	q.connect(qsock)
	f = q.makefile('rw')
	f.readline()
	def cmd(name, **args):
		f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
		f.flush()
		while True:
			r = json.loads(f.readline())
			if 'error' in r:
				raise RuntimeError('%s: %s' % (name, r['error']))
			if 'return' in r:
				return r
	def key(*k):
		cmd('send-key', keys=[{'type': 'qcode', 'data': c} for c in k], **{'hold-time': 80})
		time.sleep(0.3)
	cmd('qmp_capabilities')
	for name in FOLDERS:
		if name == '^W':
			key('meta_l', 'w')
		else:
			for c in name:
				if c.isupper():
					key('shift', c.lower())
				else:
					key(QCODE.get(c, c))
			time.sleep(1)
			key('meta_l', 'o')
		time.sleep(10)
	q.close()

env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(Q, 'lib'))
qemu = subprocess.Popen(['nice', '-n', '19', os.path.join(Q, 'usr', 'bin', 'qemu-system-m68k'),
	'-L', os.path.join(Q, 'usr', 'share', 'qemu'), '-M', 'q800', '-m', '128', '-bios', rom,
	'-drive', 'file=%s,format=raw,if=none,id=hd0' % img, '-device', 'scsi-hd,scsi-id=0,drive=hd0',
	'-display', 'none', '-chardev',
	'socket,id=s0,path=%s,server=on,wait=off,logfile=%s' % (sock, log),
	'-serial', 'chardev:s0', '-serial', 'null', '-monitor', 'none',
	'-qmp', 'unix:%s,server=on,wait=off' % qsock], env=env)

def seen(text, secs):
	end = time.time() + secs
	while time.time() < end:
		if qemu.poll() is not None:
			return False
		try:
			if text in open(log, 'rb').read():
				return True
		except OSError:
			pass
		time.sleep(2)
	return False

def send(s, text):
	for c in text.replace('\n', '\r').encode():
		s.send(bytes([c]))
		time.sleep(0.02)

ok = False
try:
	if seen(b'login:', 600):
		s = socket.socket(socket.AF_UNIX)
		s.connect(sock)
		# unread echo would fill the socket and stall the guest
		threading.Thread(target=lambda: [0 for b in iter(lambda: s.recv(4096), b'')],
			daemon=True).start()
		send(s, 'root\n')
		time.sleep(5)
		send(s, GUEST)
		if seen(b'FINDER-UP', 1800):
			finder_keys()
			send(s, '\n')
		ok = seen(b'-3-DONE', 1800)
		send(s, '/sbin/uadmin 2 0\n')
		seen(b'\0never', 30)
finally:
	qemu.terminate()
	qemu.wait()
	if not ok and os.path.exists(log):
		sys.stdout.write(open(log, 'rb').read()[-2000:].decode('latin1'))
	for f in (log, sock, qsock):
		if os.path.exists(f):
			os.unlink(f)
	os.rmdir(w)
print('[%s] desktop database' % ('ok' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
