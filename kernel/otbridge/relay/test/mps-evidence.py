#!/usr/bin/env python3
"""Verify indexed MPS imports against local shipping resources."""
import importlib.util
import struct
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location('install', Path(__file__).with_name('install-evidence.py'))
install = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install)


def target(jumps, pointer):
    above, below, length, offset = struct.unpack_from('>4I', jumps)
    assert offset == 32 and offset + 2 <= pointer < offset + length
    raw = 16 + pointer - offset - 2
    segment, trap, entry = struct.unpack_from('>HHI', jumps, raw)
    assert segment == 2 and trap == 0xa9f0
    return entry


def debug_name(code, entry, expected):
    stop = min(entry + 8192, len(code) - 3)
    for offset in range(entry, stop):
        if code[offset:offset + 2] != b'\x4e\x75' or not code[offset + 2] & 0x80:
            continue
        count = code[offset + 2] & 0x7f
        name = code[offset + 3:offset + 3 + count]
        if not count or len(name) != count or not all(32 <= c < 127 for c in name):
            continue
        assert name.decode() == expected, (entry, expected, name)
        return
    raise AssertionError((entry, expected, 'missing first function name'))


def main():
    init, jumps, code, echo_init, echo_code = (Path(a).read_bytes() for a in sys.argv[1:])
    data = install.decode(init, 4056)
    start = data.index(b'OTKrnl$mps\0') + 12
    assert struct.unpack_from('>I', data, start)[0] == 0
    names = {'OTAllocMem': 4, 'OTFreeMem': 6, 'allocb': 7, 'bcanput': 10,
             'bcanputnext': 11, 'bufcall': 12, 'canput': 13, 'canputnext': 14,
             'flushq': 22, 'freeb': 23, 'freemsg': 24, 'getq': 28, 'putbq': 34,
             'putnextctl': 39, 'putnextctl1': 41, 'putq': 42, 'qenable': 43,
             'qprocson': 44, 'qprocsoff': 45, 'noenable': 58, 'OTHERQ': 59,
             'RD': 60, 'WR': 61, 'mi_timer_cancel': 62, 'mi_timer': 63,
             'mi_timer_alloc': 64, 'mi_timer_free': 65, 'mi_timer_valid': 66,
             'unbufcall': 53}
    for name, ordinal in names.items():
        pointer = struct.unpack_from('>I', data, start + ordinal * 4)[0]
        entry = target(jumps, pointer)
        debug_name(code, entry, name)
        print(f'PASS MPS {name} ordinal={ordinal} target={entry:#x}')
    nextptr = struct.unpack_from('>I', data, start + 40 * 4)[0]
    nextentry = target(jumps, nextptr)
    assert nextentry == 0x25b2
    assert struct.unpack_from('>4H', code, nextentry) == (0x206f, 4, 0x2068, 12)
    print('PASS MPS putnext ordinal40 selects q_next12 and enters synchronized dispatcher')
    replyptr = struct.unpack_from('>I', data, start + 46 * 4)[0]
    assert target(jumps, replyptr) == 0x25c0
    echo = install.decode(echo_init, 756)
    record = echo.index(b'OTKrnl$mps\0') - 16
    for call, ordinal in [(0xa7a, 7), (0xb1a, 28), (0xb42, 34), (0xba6, 46)]:
        stub = call + 40
        assert struct.unpack_from('>H', echo_code, stub)[0] == 0x41f9
        displacement = struct.unpack_from('>i', echo_code, stub + 2)[0]
        fn, rec, zero, found = struct.unpack_from('>IIHI', echo, len(echo) + displacement)
        assert fn == 0 and rec == (record - len(echo)) & 0xffffffff and zero == 0 and found == ordinal
    print('PASS shipping echo corroborates one-based allocb7/getq28/putbq34/qreply46')


if __name__ == '__main__':
    if len(sys.argv) != 6:
        sys.exit('usage: mps-evidence.py cd32-1.bin cd32-0.bin cd32-2.bin cd62-1.bin cd62-2.bin')
    main()
