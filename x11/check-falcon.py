#!/usr/bin/env python3
"""Verify that a packaged X server includes the Falcon bitplane backend."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent.parent / "toolchain/bin"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def executable(path):
    with path.open("rb") as binary:
        header = binary.read(20)
    return (header[:7] == b"\x7fELF\x01\x02\x01" and
            header[16:20] == b"\x00\x02\x00\x04")


def planes(path):
    result = subprocess.run([str(TOOLS / "m68k-elf-nm"), str(path)],
                            capture_output=True, text=True)
    return any(line.split()[1:] == ["T", "ipl2p8ScreenInit"]
               for line in result.stdout.splitlines())


def matches(source, server):
    if not executable(source) or not executable(server) or not planes(source):
        return False
    with tempfile.TemporaryDirectory(prefix="xfalcon-") as tmp:
        stripped = Path(tmp) / "Xamix"
        subprocess.run([str(TOOLS / "m68k-elf-strip"), "-o", str(stripped),
                        str(source)], check=True)
        return digest(stripped) == digest(server)


def verify(package):
    server = package / "bin/Xamix"
    receipt = package / "falcon.json"
    if not executable(server):
        return False
    if receipt.exists():
        info = json.loads(receipt.read_text())
        return (isinstance(info, dict) and info.get("iplan2p8") is True and
                info.get("sha256") == digest(server))
    if planes(server):
        return True
    source = package.parent / "src/xc/programs/Xserver/Xamix"
    return source.is_file() and matches(source, server)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "seal":
        source, package = map(Path, sys.argv[2:])
        server = package / "bin/Xamix"
        if not matches(source, server):
            raise ValueError("Xamix lacks iplan2p8 or differs from its build")
        (package / "falcon.json").write_text(json.dumps(
            {"iplan2p8": True, "sha256": digest(server)}) + "\n")
    elif len(sys.argv) == 2:
        if not verify(Path(sys.argv[1])):
            raise ValueError("Xamix has no verified Falcon bitplane backend; "
                             "rebuild or repackage X11")
    else:
        raise ValueError("usage: check-falcon.py [seal SOURCE] PACKAGE")
    print("[OK] Falcon X server: iplan2p8")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit("[FAIL] " + str(error))
