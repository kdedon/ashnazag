#!/usr/bin/env python3
# summarize.py SERIAL.LOG STATE -- per-area table and FAIL list from a test console log.
# STATE: done, panic, timeout or exited.
# Exit: 0 all PASS, 1 any FAIL, 2 run incomplete.
import re, sys

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
