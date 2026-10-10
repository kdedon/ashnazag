#!/usr/bin/env python3
"""mkbench.py dir -- BENCH80.COM and BENCH86.CMD: counted loops, then a line.

BENCH80: 40 x 65536 x (DCX B, MOV A,B, ORA C, JNZ), about 10.5M 8080 instructions.
BENCH86: 40 x 65536 x (MOV BX,CX, ADD AX,BX, LOOP), about 7.9M 8086 instructions.
"""
import os
import sys

d = sys.argv[1]
z80 = bytes([
    0x16, 40,               # mvi d,40
    0x01, 0, 0,             # l1: lxi b,0
    0x0b,                   # l2: dcx b
    0x78,                   # mov a,b
    0xb1,                   # ora c
    0xc2, 0x05, 0x01,       # jnz l2
    0x15,                   # dcr d
    0xc2, 0x02, 0x01,       # jnz l1
    0x11, 0x18, 0x01,       # lxi d,msg
    0x0e, 9,                # mvi c,9
    0xcd, 5, 0,             # call 5
    0xc9,                   # ret
]) + b'Z80 BENCH DONE\r\n$'
open(os.path.join(d, 'BENCH80.COM'), 'wb').write(z80 + bytes(-len(z80) % 128))

i86 = bytes([
    0xba, 40, 0,            # mov dx,40
    0x31, 0xc9,             # l1: xor cx,cx
    0x89, 0xcb,             # l2: mov bx,cx
    0x01, 0xd8,             # add ax,bx
    0xe2, 0xfa,             # loop l2
    0x4a,                   # dec dx
    0x75, 0xf5,             # jnz l1
    0xba, 0x1a, 0x01,       # mov dx,msg
    0xb1, 9,                # mov cl,9
    0xcd, 0xe0,             # int 0e0h
    0xb9, 0, 0,             # mov cx,0
    0xcd, 0xe0,             # int 0e0h
]) + b'I86 BENCH DONE\r\n$'
img = bytes(0x100) + i86
npar = (len(img) + 15) // 16
hdr = bytearray(128)
hdr[0:9] = bytes([1, npar & 0xff, npar >> 8, 0, 0, 0, 2, 0, 0])   # code group, G-Min 512 paragraphs
open(os.path.join(d, 'BENCH86.CMD'), 'wb').write(bytes(hdr) + img + bytes(npar * 16 - len(img)))
