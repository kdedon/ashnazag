#!/usr/bin/env python3
"""Exercise Falcon package validation with m68k ELF servers."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CHECK = ROOT / "x11/check-falcon.py"
TOOLS = ROOT / "toolchain/bin"


def run(*args):
    subprocess.run(list(map(str, args)), check=True, capture_output=True)


def check(package, expected):
    result = subprocess.run(["python3", str(CHECK), str(package)],
                            capture_output=True, text=True)
    assert (result.returncode == 0) == expected, result.stdout + result.stderr


with tempfile.TemporaryDirectory(prefix="xpackage-") as tmp:
    work = Path(tmp)
    package = work / "pkg"
    (package / "bin").mkdir(parents=True)
    source = work / "src/xc/programs/Xserver/Xamix"
    source.parent.mkdir(parents=True)
    assembly = work / "server.s"
    obj = work / "server.o"
    server = package / "bin/Xamix"
    for symbol, expected in (("mfbScreenInit", False),
                             ("ipl2p8ScreenInit", True)):
        assembly.write_text(".text\n.globl " + symbol + "\n" + symbol + ": rts\n")
        run(TOOLS / "m68k-elf-as", "-m68030", "-o", obj, assembly)
        run(TOOLS / "m68k-elf-ld", "-e", symbol, "-o", source, obj)
        run(TOOLS / "m68k-elf-strip", "-o", server, source)
        check(package, expected)
    print("[OK] legacy mono rejected; legacy bitplane server accepted")
    run("python3", CHECK, "seal", source, package)
    unstripped = work / "unstripped"
    shutil.copyfile(source, unstripped)
    check(package, True)
    source.unlink()
    check(package, True)
    print("[OK] sealed bitplane package works without its build tree")
    with server.open("ab") as output:
        output.write(b"changed")
    check(package, False)
    print("[OK] changed server rejected")
    (package / "falcon.json").write_text("{}\n")
    check(package, False)
    (package / "falcon.json").write_text("invalid\n")
    check(package, False)
    (package / "falcon.json").write_text("null\n")
    check(package, False)
    (package / "falcon.json").unlink()
    check(package, False)
    print("[OK] missing build, capability, and malformed receipt rejected")
    shutil.copyfile(unstripped, server)
    check(package, True)
    print("[OK] unstripped bitplane server accepted")
    alternate = work / "alternate/x11/check-falcon.py"
    alternate.parent.mkdir(parents=True)
    shutil.copyfile(CHECK, alternate)
    result = subprocess.run(["python3", str(alternate), str(package)],
                            capture_output=True, text=True)
    assert result.returncode != 0 and result.stderr.startswith("[FAIL]")
    assert "Traceback" not in result.stderr
    print("[OK] unavailable cross tools fail cleanly")
    shutil.copyfile(obj, server)
    check(package, False)
    print("[OK] relocatable object rejected")
    result = subprocess.run(["sh", str(ROOT / "kernel/atari/mkdisk.sh"),
                             str(work / "missing-kernel"), str(work / "disk.img")],
                            env=dict(os.environ, X11=str(package)),
                            capture_output=True, text=True)
    assert result.returncode != 0
    assert "no verified Falcon bitplane backend" in result.stderr
    assert not (work / "disk.img").exists()
    print("[OK] image builder rejects unsuitable X before building")
    server.unlink()
    result = subprocess.run(["python3", str(CHECK), str(package)],
                            capture_output=True, text=True)
    assert result.returncode != 0 and result.stderr.startswith("[FAIL]")
    assert "Traceback" not in result.stderr
    print("[OK] missing server fails cleanly")
