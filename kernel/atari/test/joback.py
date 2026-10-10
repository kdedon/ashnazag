#!/usr/bin/env python3
"""Check console acknowledgement before queuing X readiness commands."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('keys', Path(__file__).parents[1] / 'hatari-keys.py')
keys = importlib.util.module_from_spec(spec)
spec.loader.exec_module(keys)
command = '(startx; echo done) &'
for job in ('[1]\t337', '337'):
    for prompt in ('$ ', '# '):
        data = '$ ' + command + '\r\n' + job + '\r\n' + prompt
        assert keys.job_end(data, command, 0) == len(data)
        assert keys.job_end(data, command, len(data)) is None
for old in (command + '\r\n[1]\t337\r\n$ ', command + '\r\n337\r\n# '):
    assert keys.job_end(old + command + '\r\n', command, len(old)) is None
for tail in ('\r\n', '\r\n[1]\t337\r\n', '\r\n[1]\t337\r\n$'):
    assert keys.job_end(command + tail, command, 0) is None
assert keys.job_end(command[:-1] + '\r\n[1]\t337\r\n$ ', command, 0) is None
assert keys.job_end(command + '\r\nnoise\r\n[1]\t337\r\n$ ', command, 0) is None
print('PASS background command echo/job/prompt acknowledgement (15 cases)')
