#!/usr/bin/env python3
import os
from pathlib import Path
import struct
import subprocess
import tempfile

base = Path(__file__).resolve().parents[2]
source = base / "kernel/guest/nativeenv/startnative.c"
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    launcher = work / "startnative"
    common = ["nice", "-n", "19", "cc", "-std=gnu89", "-Wall", "-Wextra",
              "-Wno-unused-function", "-I" + str(base / "kernel/guest/include")]
    subprocess.run(common + [str(source), "-o", str(launcher)], check=True)
    probe = work / "probe"
    subprocess.run(common + ["-Dexecv=nativeenv_execprobe", str(source),
                             str(base / "tests/nativeenv/execprobe.c"), "-o", str(probe)], check=True)
    env = dict(os.environ, HOME=str(work))
    families = ["smsqe", "lisa", "voyage200"]
    roots = [work / "SMSQE", work / "Lisa", work / "Voyage200"]
    for root in roots:
        root.mkdir()
    def run(binary, *args, status):
        result = subprocess.run([str(binary), *args], env=env, capture_output=True, text=True)
        assert result.returncode == status, (args, result.returncode, result.stderr)
        return result
    run(launcher, "unknown", status=2)
    run(launcher, "smsqe", "-x", status=2)
    run(launcher, "smsqe", "-e", "../outside", status=2)
    run(launcher, "smsqe", "-q", status=1)
    header = bytearray(52)
    header[:7] = b"\x7fELF\x01\x02\x01"
    struct.pack_into(">HHI", header, 16, 2, 4, 1)
    for family, root in zip(families, roots):
        binary = root / "native"
        binary.write_bytes(header)
        binary.chmod(0o755)
        run(launcher, family, "-q", status=1)
        (root / "media").write_bytes(b"caller media")
        run(launcher, family, "-q", status=0)
        run(probe, family, "--", "argument", status=37)
        run(launcher, family, status=127)
        alias = work / ("start" + family)
        alias.symlink_to(probe)
        run(alias, "--", "argument", status=37)
    run(launcher, "smsqe", "-x", "relative", status=2)
    bad = bytearray(header)
    bad[19] = 3
    (roots[0] / "native").write_bytes(bad)
    run(launcher, "smsqe", "-q", status=1)
    bad = bytearray(header)
    bad[5] = 1
    (roots[0] / "native").write_bytes(bad)
    run(launcher, "smsqe", "-q", status=1)
    (roots[0] / "native").write_text("#!/bin/sh\nexit 0\n")
    run(launcher, "smsqe", "-q", status=1)
    import fcntl
    for family, root in zip(families[1:], roots[1:]):
        named = root / "separate"
        named.mkdir()
        run(probe, family, "-e", "separate", "-x", str(root / "native"),
            "-m", str(root / "media"), "--", "argument", status=37)
        with (named / ".env").open("r+") as lock:
            fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            run(probe, family, "-e", "separate", "-x", str(root / "native"),
                "-m", str(root / "media"), "--", "argument", status=1)
    with (roots[1] / ".env").open("r+") as lock:
        fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        run(probe, "voyage200", "--", "argument", status=37)
    subprocess.run(["sh", "-n", str(base / "x11/session/xsession")], check=True)
print("native environment checks passed")
