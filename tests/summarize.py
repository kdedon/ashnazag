#!/usr/bin/env python3
# summarize.py SERIAL.LOG STATE [QEMU.ERR] -- per-area table and FAIL list from a test console log.
# STATE: done, panic, timeout or exited.
# Exit: 0 all PASS, 1 any FAIL, 2 run incomplete.
import re, sys, zlib

log = open(sys.argv[1], 'rb').read().decode('latin-1').replace('\r', '')
state = sys.argv[2] if len(sys.argv) > 2 else 'done'
areas, fails, skips = {}, [], []
for line in log.split('\n'):
	m = re.match(r'(PASS|FAIL|SKIP) ([\w-]+)\.(\S+?)(?::\s*(.*))?$', line)
	if not m:
		continue
	kind, area, name, why = m.groups()
	a = areas.setdefault(area, {'PASS': 0, 'FAIL': 0, 'SKIP': 0})
	a[kind] += 1
	if kind == 'FAIL':
		fails.append('%s.%s: %s' % (area, name, why or ''))
	elif kind == 'SKIP':
		skips.append('%s.%s: %s' % (area, name, why or ''))

# HALT area.name: passes when the kernel's halt message follows, with no panic
for m in re.finditer(r'^HALT ([\w-]+)\.(\S+)$', log, re.M):
	area, name = m.groups()
	rest = log[m.end():]
	ok = 'system is halted' in rest and not re.search('panic', rest, re.I)
	a = areas.setdefault(area, {'PASS': 0, 'FAIL': 0, 'SKIP': 0})
	a['PASS' if ok else 'FAIL'] += 1
	if not ok:
		fails.append('%s.%s: no halt' % (area, name))

# BURST area.name bytes crc: the lines up to "BURST END" arrived whole
for m in re.finditer(r'^BURST ([\w-]+)\.(\S+) (\d+) ([0-9a-f]{8})\n', log, re.M):
	area, name, n, crc = m.group(1), m.group(2), int(m.group(3)), int(m.group(4), 16)
	end = log.find('\nBURST END\n', m.end() - 1)
	got = log[m.end():end + 1].encode('latin-1') if end >= 0 else b''
	ok = len(got) == n and zlib.crc32(got) == crc
	a = areas.setdefault(area, {'PASS': 0, 'FAIL': 0, 'SKIP': 0})
	a['PASS' if ok else 'FAIL'] += 1
	if not ok:
		fails.append('%s.%s: %s' % (area, name, '%d of %d bytes, crc %s' % (len(got), n,
		    'ok' if zlib.crc32(got) == crc else 'wrong') if end >= 0 else 'no BURST END'))

# a run cut short fails the test it was in, with the emulator's last words
cur = re.findall(r'^(?:=== (\S+)|--- (\S+?):)', log, re.M)
if state != 'done' and cur and cur[-1][0]:
	name = cur[-1][0]
	err = ''
	if len(sys.argv) > 3:
		try:
			err = [l for l in open(sys.argv[3], 'rb').read().decode('latin-1').split('\n')
			    if l.strip()]
			err = ([l for l in err if 'ERROR' in l] or err or [''])[-1]
		except OSError:
			pass
	areas.setdefault(name, {'PASS': 0, 'FAIL': 0, 'SKIP': 0})['FAIL'] += 1
	fails.append('%s.run: run %s here%s' % (name, state, ': ' + err if err else ''))

done = re.search(r'^TESTS DONE (.*)$', log, re.M)
print('%-12s %5s %5s %5s' % ('area', 'pass', 'fail', 'skip'))
tot = {'PASS': 0, 'FAIL': 0, 'SKIP': 0}
for area in sorted(areas):
	a = areas[area]
	for k in tot:
		tot[k] += a[k]
	print('%-12s %5d %5d %5d' % (area, a['PASS'], a['FAIL'], a['SKIP']))
print('%-12s %5d %5d %5d' % ('total', tot['PASS'], tot['FAIL'], tot['SKIP']))
print('runner: %s' % (done.group(1) if done else 'no TESTS DONE line'))
if state != 'done':
	print('RUN %s' % state.upper())
	for line in log.split('\n'):
		if re.search('panic', line, re.I):
			print('  ' + line)
for f in fails:
	print('FAIL ' + f)
for s in skips:
	print('SKIP ' + s)
sys.exit(2 if state != 'done' or not done else 1 if tot['FAIL'] else 0)
