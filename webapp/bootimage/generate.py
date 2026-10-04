#!/usr/bin/env python3
"""Embed freshly assembled first-party boot code."""
import base64
from pathlib import Path
import subprocess

here = Path(__file__).resolve().parent
root = here.parent.parent
subprocess.run(["sh", str(root / "kernel/mac/bootblk/build.sh")], check=True)
code = (root / "kernel/mac/bootblk/build/bootblk.bin").read_bytes()
if len(code) != 1024 or code[0x8c:0x90] != b"UxBB":
    raise SystemExit("invalid boot-block template")
(here / "bootblocks.go").write_text(
    'package bootimage\n\nconst bootBlocksBase64 = "'
    + base64.b64encode(code).decode("ascii") + '"\n'
)
