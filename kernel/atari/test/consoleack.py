#!/usr/bin/env python3
"""Check that command typing advances only after guest echo."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('keys', Path(__file__).parents[1] / 'hatari-keys.py')
keys = importlib.util.module_from_spec(spec)
spec.loader.exec_module(keys)

class Console:
    def __init__(self, drop=None):
        self.data = '(startx) &\r\n[1]\t99\r\n$ '
        self.pending = None
        self.polls = 0
        self.shift = False
        self.sent = []
        self.drop = drop
        self.clock = 0

    def send(self, command):
        op, code = command.split()[1:]
        code = int(code, 16)
        if op == 'keydown':
            self.shift = True
        elif op == 'keyup':
            self.shift = False
        else:
            assert self.pending is None, 'next key sent before echo'
            c = (keys.HI if self.shift else keys.LO)[code]
            self.sent.append(c)
            if c != self.drop:
                self.pending = '\r\n' if c == '\r' else c
                self.polls = 3

    def read(self):
        if self.pending:
            self.polls -= 1
            if self.polls == 0:
                self.data += self.pending
                self.pending = None
        return self.data

    def sleep(self, duration):
        self.clock += duration

command = '(startx) &\n'
for drop in (None, '&'):
    con = Console(drop)
    keys.time.monotonic = lambda: con.clock
    keys.time.sleep = con.sleep
    try:
        keys.type_console(command, con.send, con.read, 1)
        assert drop is None
        assert con.data.endswith('(startx) &\r\n')
        assert ''.join(con.sent) == command.replace('\n', '\r')
    except TimeoutError as error:
        assert drop == '&'
        assert 'character 10' in str(error)
        assert con.sent[-1] == '&' and '\r' not in con.sent
for name in ('falcon-x-keys.txt', 'falcon-xtos-keys.txt'):
    for line in (Path(__file__).parent / name).read_text().splitlines():
        if line.startswith('typeconsole '):
            text = line[len('typeconsole '):].encode().decode('unicode_escape')
            con = Console()
            keys.time.monotonic = lambda: con.clock
            keys.time.sleep = con.sleep
            keys.type_console(text, con.send, con.read, 10)
            assert ''.join(con.sent) == text.replace('\n', '\r')
            assert con.data.endswith(text.replace('\n', '\r\n'))
con = Console()
keys.time.monotonic = lambda: con.clock
keys.time.sleep = con.sleep
try:
    keys.type_console('(startx) &\n', con.send, con.read, .05)
    assert False
except TimeoutError:
    assert con.clock < .1 and '\r' not in con.sent
for command in ('one\ntwo\n', 'one\n\n', 'one\rtwo\n'):
    try:
        keys.type_console(command, con.send, con.read, 1)
        assert False
    except ValueError:
        pass
print('PASS console echo backpressure, stale echo, dropped key, whole-command deadline and multiline refusal')
