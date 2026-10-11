#!/usr/bin/env python3
"""xtype.py -- type text into the window under the pointer on an X display.

    python3 xtype.py display text...

Each argument is typed followed by Return; \\n inside it also gives Return.
"""
import ctypes
import sys
import time

X = ctypes.cdll.LoadLibrary('libX11.so.6')
T = ctypes.cdll.LoadLibrary('libXtst.so.6')
X.XOpenDisplay.restype = ctypes.c_void_p
X.XOpenDisplay.argtypes = [ctypes.c_char_p]
X.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
X.XStringToKeysym.restype = ctypes.c_ulong
X.XStringToKeysym.argtypes = [ctypes.c_char_p]
X.XFlush.argtypes = [ctypes.c_void_p]
T.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int,
                                ctypes.c_ulong]

NAMES = {' ': 'space', '\n': 'Return', '-': 'minus', '/': 'slash',
         '.': 'period', ',': 'comma', '=': 'equal', ';': 'semicolon',
         "'": 'apostrophe', '[': 'bracketleft', ']': 'bracketright',
         '\\': 'backslash', '`': 'grave'}
# shifted characters on a US layout
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6',
           '&': '7', '*': '8', '(': '9', ')': '0', '_': 'minus',
           '+': 'equal', ':': 'semicolon', '"': 'apostrophe', '<': 'comma',
           '>': 'period', '?': 'slash', '|': 'backslash', '~': 'grave',
           '{': 'bracketleft', '}': 'bracketright'}

d = X.XOpenDisplay(sys.argv[1].encode())
if not d:
    sys.exit('xtype: cannot open display %s' % sys.argv[1])


def code(name):
    return X.XKeysymToKeycode(d, X.XStringToKeysym(name.encode()))


SHIFT = code('Shift_L')


def key(c):
    shift = c in SHIFTED or c.isupper()
    name = SHIFTED.get(c) or NAMES.get(c) or c.lower()
    k = code(name)
    # the emulator polls the keyboard, so each event must stay visible
    if shift:
        T.XTestFakeKeyEvent(d, SHIFT, 1, 0)
    T.XTestFakeKeyEvent(d, k, 1, 0)
    X.XFlush(d)
    time.sleep(0.06)
    T.XTestFakeKeyEvent(d, k, 0, 0)
    if shift:
        T.XTestFakeKeyEvent(d, SHIFT, 0, 0)
    X.XFlush(d)
    time.sleep(0.06)


for arg in sys.argv[2:]:
    for c in arg.replace('\\n', '\n') + '\n':
        key(c)
