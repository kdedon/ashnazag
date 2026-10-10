#!/usr/bin/env python3
"""Verify the shipping initializer's separate data and code bases."""
from pathlib import Path
import sys
b=Path(sys.argv[1]).read_bytes()
checks={
    0x156:'266e000897d4',
    0x178:'2f2e000c487408002f0b2f2e0008610000ac',
    0x238:'222e0008226e000c206e0010',
    0x27c:'20116f1c4aae00146716246e0014d5c00c524ef9',
    0x296:'202a00026002d0812280',
}
for address,hexbytes in checks.items():
    expected=bytes.fromhex(hexbytes)
    assert b[address:address+len(expected)]==expected,hex(address)
print('PASS 5 shipping initializer signatures: A5 data base, separate code base, JMP target and zero fallback')
