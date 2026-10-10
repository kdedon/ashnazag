#!/usr/bin/env python3
"""Verify a private UDP helper on loopback, including shutdown cleanup."""
from pathlib import Path
import socket
import subprocess
import sys
import time

work = Path(sys.argv[1]).resolve()
ready = work / 'ready'
ready.unlink(missing_ok=True)
subprocess.run(['cc', '-O', '-Wall', '-Wextra', '-Werror',
                f'-DREADY_PATH="{ready}"', '-DECHO_PORT=0',
                str(Path(__file__).with_name('udp-echo.c')), '-o', str(work / 'host-otudp')], check=True)
p = subprocess.Popen([str(work / 'host-otudp')])
try:
    deadline = time.monotonic() + 3
    while not ready.exists() and time.monotonic() < deadline:
        assert p.poll() is None, 'helper exited before bind'
        time.sleep(.02)
    assert ready.exists(), 'ready timeout'
    port = int.from_bytes(ready.read_bytes(), 'big')
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(2)
        for payload in (b'OT81!\n', bytes(range(256)), b''):
            s.sendto(payload, ('127.0.0.1', port))
            data, address = s.recvfrom(1024)
            assert data == payload and address == ('127.0.0.1', port)
    p.terminate()
    assert p.wait(timeout=3) == 0
    assert not ready.exists()
    print('PASS UDP helper payload/source/zero datagram and SIGTERM cleanup')
finally:
    if p.poll() is None:
        p.terminate()
        p.wait(timeout=3)
    ready.unlink(missing_ok=True)
