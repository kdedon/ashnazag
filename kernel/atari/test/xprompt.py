#!/usr/bin/env python3
"""Check focused default-terminal readiness before sending commands."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('keys', Path(__file__).parents[1] / 'hatari-keys.py')
keys = importlib.util.module_from_spec(spec)
spec.loader.exec_module(keys)
black, white = (0, 0, 0), (255, 255, 255)
for prompt in keys.PROMPTS:
    rows = [[white] * 640 for _ in range(480)]
    for y, line in enumerate(prompt, 65):
        rows[y][20:26] = [black if c == '#' else white for c in line]
        rows[y][32:38] = [black] * 6
    assert keys.terminal_prompt(rows)
    rows[70][34] = white
    assert not keys.terminal_prompt(rows), 'outlined cursor must not count as focus'
    rows[70][34] = black
    rows[68][21] = white if rows[68][21] == black else black
    assert not keys.terminal_prompt(rows), 'incomplete prompt must not count as shell readiness'
assert not keys.terminal_prompt([[white] * 640 for _ in range(480)])
assert not keys.terminal_prompt([])
print('PASS root/guest prompt, keyboard focus, incomplete prompt and blank screen')
