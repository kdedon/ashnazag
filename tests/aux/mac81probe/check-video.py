#!/usr/bin/env python3
"""Execute linked video traps with clobbered callee registers."""
from pathlib import Path
import subprocess
import sys

build = Path(sys.argv[1])
symbols = {}
for line in subprocess.check_output([sys.argv[2], str(build / 'probe.elf')], text=True).splitlines():
    words = line.split()
    if len(words) == 3:
        symbols[words[2]] = int(words[0], 16)
code = (build / 'probe.bin').read_bytes()
saved = [*range(2, 8), *range(10, 15)]
checks = 0
for name, trap in [('videolist', 0xaa29), ('videomain', 0xaa2a), ('videonext', 0xaa2b)]:
    for result in [0, 0x23456, 0xf1234567]:
        mem = bytearray(8192)
        regs = [0x10000 + i for i in range(16)]
        regs[15] = 4096
        original = regs[:]
        def put(a, v): mem[a:a+4] = (v & 0xffffffff).to_bytes(4, 'big')
        def get(a): return int.from_bytes(mem[a:a+4], 'big')
        def push(v): regs[15] -= 4; put(regs[15], v)
        def pop():
            v = get(regs[15]); regs[15] += 4; return v
        put(4096, 0xdeadbeef)
        put(4100, 0x123456)
        pc = symbols[name]
        for _ in range(16):
            op = int.from_bytes(code[pc:pc+2], 'big'); pc += 2
            if op == 0x206f:
                off = int.from_bytes(code[pc:pc+2], 'big'); pc += 2
                regs[8] = get(regs[15] + off)
            elif op == 0x48e7:
                assert code[pc:pc+2] == bytes.fromhex('3f3e'); pc += 2
                for r in reversed(saved): push(regs[r])
            elif op == 0x42a7: push(0)
            elif op == 0x2f08: push(regs[8])
            elif op == trap:
                if name == 'videonext': assert pop() == 0x123456
                put(regs[15], result)
                for r in saved: regs[r] = 0xbad000 + r
            elif op == 0x201f: regs[0] = pop()
            elif op == 0x4cdf:
                assert code[pc:pc+2] == bytes.fromhex('7cfc'); pc += 2
                for r in saved: regs[r] = pop()
            elif op == 0x4e75:
                assert pop() == 0xdeadbeef
                break
            else: raise AssertionError(hex(op))
        else: raise AssertionError('unbounded glue')
        assert regs[0] == result and regs[15] == 4100
        assert all(regs[r] == original[r] for r in saved)
        checks += 1
print('video glue: %d checks passed' % checks)
