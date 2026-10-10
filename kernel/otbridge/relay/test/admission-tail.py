#!/usr/bin/env python3
"""Check the assembled tail adapter's preserved state and argument location."""
import struct, subprocess, sys
from pathlib import Path
obj=Path(sys.argv[1]);objcopy=sys.argv[2]
raw=obj.with_suffix(".text")
try:
    subprocess.run([objcopy,"-O","binary","-j",".text",str(obj),str(raw)],check=True)
    code=raw.read_bytes()
finally:
    raw.unlink(missing_ok=True)
assert code[:38]==bytes.fromhex("48e7fffe42e72f2f00464eb900000000588f44df4cdf7fff4e56000048e700384ef900000000")
registers=list(range(15))
original_sp=0x1000
saved_sp=original_sp-15*4-2
assert saved_sp+struct.unpack_from(">H",code,8)[0]==original_sp+8
assert code[0x18:0x20]==bytes.fromhex("4e56000048e70038")
assert code[32:34]==bytes.fromhex("4ef9")
assert code[38:40]==bytes.fromhex("4ef9")
assert code[44:]==bytes.fromhex("4f544144000100000000")
print("PASS tail register/CCR preservation, second argument offset, native prologue and absolute tail transfer")
