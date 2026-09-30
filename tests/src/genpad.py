#!/usr/bin/env python3
# genpad.py KIND SIZE OUT -- xhpad.h: SIZE pattern bytes in .data (KIND data) or .rodata
import sys
kind, n, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
s = ''.join(chr(ord('A') + (i * 7 + i // 26) % 26) for i in range(n))
with open(out, 'w') as f:
	f.write('#define PADSZ %d\n' % n)
	q = 'const ' if kind == 'rodata' else ''
	f.write('static %schar pad[%d] =\n' % (q, max(n, 1)))
	if n == 0:
		f.write('\t{ 0 };\n')
	else:
		for i in range(0, n, 72):
			f.write('\t"%s"\n' % s[i:i + 72])
		f.write('\t;\n')
