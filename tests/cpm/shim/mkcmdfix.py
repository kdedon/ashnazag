#!/usr/bin/env python3

"""mkcmdfix.py - the synthetic .CMD fixtures `make i86test' loads.

    mkcmdfix.py <destdir>

src/shim/tests/i86corpus/ holds four of DRI's own .CMD files, and they answer "does
the loader read what a real file says".  They cannot answer anything else:
all four are small-model, all four carry A-Base 0, and none of them is
malformed.  So the header space the loader must refuse -- kill criterion K1
(CPM86-STAGE-ONE.md §6) and every CE_* error in src/shim/i86.h -- has no real
file behind it, and never will.  These fixtures are that half, built from
the format description in src/shim/i86load.c: a 128-byte header of eight
9-byte group descriptors (G-Form, G-Length, A-Base, G-Min, G-Max, all
little-endian paragraph counts) followed by the group images in descriptor
order.

Four of them also RUN.  The real files cannot: their first BDOS call has
nowhere to go until the INT 0E0h seam exists, so nothing here is a copy of
one.  They are the smallest programs that make a loader decision visible in
a result -- the 8080 model's entry at code offset 0x100, G-Max 0 resolving
to a whole segment through the base page the guest itself reads, and the
difference
between "we do not implement this" and "this is not an instruction", which
is the property that keeps future widening honest.

One fixture is not a .CMD at all.  GENCMD.CMD reads FILENAME.H86 and
writes FILENAME.CMD, and no corpus of .CMD files can supply its input, so
<destdir>/I86HEX.H86 is built here too -- real DRI hex, real checksums,
around a hand-assembled 8086 program.  See p_hex().  I86T.A86 is ASM86's
input, the same program's shape in source form.  See p_asm().

Output is <destdir>/*.CMD plus <destdir>/I86HEX.H86, I86T.A86 plus
<destdir>/MANIFEST, which is what src/shim/tests/i86test.c reads:

    LOAD <file> <CE_*> [model=|entry=|ng=|alloc=n,n|need=]
    RUN  <file> <X_*>  [ip=|ax=|bx=|cx=|dx=|sp=|steps=|w=seg:off:val]

Every expected value in the manifest is written here from the fixture's
intent -- the sum a program computes, the allocation rule stated in
i86load.c's galloc() -- and never read back out of the loader or the
executor, which would agree with themselves and prove nothing.
"""

import os
import sys

# G-Form values (src/shim/i86.h).
G_CODE = 1
G_DATA = 2
G_EXTRA = 3
G_STACK = 4
G_AUX1 = 5
G_SHCODE = 9

PARA = 16
HDR = 128
MAXPAR = 4096                           # paragraphs in one 64 KB segment

TAIL = "B:FIX.DAT"                      # the command tail i86test passes


# ---------------------------------------------------------------- assembler

class Asm:
    """A two-pass assembler just big enough for the fixture programs.

    Bytes are written out by hand so the encodings stay visible in the
    source; only the branch displacements are computed, because a
    hand-counted displacement is the one thing here that would be wrong
    silently rather than loudly.
    """

    def __init__(self, org=0):
        self.org = org
        self.pos = org
        self.items = []                 # (bytes, None) or (None, fixup)
        self.labels = {}

    def label(self, name):
        self.labels[name] = self.pos
        return self

    def at(self, name):
        return self.labels[name]

    def b(self, *vals):
        data = bytes(vals)
        self.items.append((data, None))
        self.pos += len(data)
        return self

    def rel8(self, op, target):
        self.items.append((None, (1, op, target, self.pos + 2)))
        self.pos += 2
        return self

    def rel16(self, op, target):
        self.items.append((None, (2, op, target, self.pos + 3)))
        self.pos += 3
        return self

    def abs16(self, op, target):
        """An 8080 jump or call: op, then the target's address."""
        self.items.append((None, (0, op, target, 0)))
        self.pos += 3
        return self

    def code(self):
        out = bytearray()
        for data, fix in self.items:
            if data is not None:
                out += data
                continue
            n, op, target, after = fix
            d = self.labels[target] - after
            if n == 0:
                out += bytes([op]) + w16(d)
                continue
            if n == 1:
                if not -128 <= d <= 127:
                    raise ValueError("rel8 out of range to %s" % target)
                out += bytes([op, d & 0xff])
            else:
                out += bytes([op, d & 0xff, (d >> 8) & 0xff])
        return bytes(out)


# ------------------------------------------------------------------- format

def w16(v):
    return bytes([v & 0xff, (v >> 8) & 0xff])


def header(groups):
    """groups: list of (form, glen, abase, gmin, gmax), in descriptor order."""
    h = bytearray(HDR)
    for i, (form, glen, abase, gmin, gmax) in enumerate(groups):
        o = i * 9
        h[o] = form
        h[o + 1:o + 3] = w16(glen)
        h[o + 3:o + 5] = w16(abase)
        h[o + 5:o + 7] = w16(gmin)
        h[o + 7:o + 9] = w16(gmax)
    return bytes(h)


def npar(image):
    """Paragraphs an image occupies -- G-Length is a paragraph count."""
    return (len(image) + PARA - 1) // PARA


def write(dest, name, groups, images=None, cut=None):
    """Write one fixture.  `images' pads to each descriptor's G-Length;
    `cut' truncates the finished file to that many bytes, which is the
    only way to build the two files that end early."""
    data = bytearray(header(groups))
    for i, (form, glen, abase, gmin, gmax) in enumerate(groups):
        img = images[i] if images and i < len(images) else b''
        if len(img) > glen * PARA:
            raise ValueError("%s: group %d image exceeds G-Length" % (name, i))
        data += img + bytes(glen * PARA - len(img))
    if cut is not None:
        data = data[:cut]
    open(os.path.join(dest, name), 'wb').write(bytes(data))
    return len(data)


# ----------------------------------------------------------------- programs

def p_8080():
    """RUN8080.CMD -- the 8080 model, and the entry point that comes with it.

    i86load.c's finding, made against DDT86, TOD and SETUP: a code-only
    .CMD is the 8080 model and enters at code offset 0x100, with the first
    256 bytes of the image reserved for the base page.  So this image holds
    256 zero bytes and then a program, and the STEP COUNT in the manifest
    is what pins the entry: enter at 0 instead and the 256 zeros decode as
    128 harmless `add [bx+si],al' before falling into the same program with
    the same answer, but 128 instructions late.

    It sums 1..10 with LOOP (55), doubles it in a near CALL (110), stores
    that, and reads the command tail's length out of the base page at 0x80
    -- the two halves of i86bpage(), the group table and the tail, seen
    from inside the guest.
    """
    a = Asm(0x100)
    a.b(0x31, 0xc0)                     # xor ax,ax
    a.b(0xb9, 0x0a, 0x00)               # mov cx,10
    a.label('sum')
    a.b(0x01, 0xc8)                     # add ax,cx
    a.rel8(0xe2, 'sum')                 # loop sum       -- 10 iterations
    a.rel16(0xe8, 'dbl')                # call dbl
    a.b(0xa3, 0x00, 0x04)               # mov [0x400],ax
    a.b(0x8a, 0x1e, 0x80, 0x00)         # mov bl,[0x80]  -- tail length
    a.label('halt')
    a.b(0xf4)                           # hlt
    a.label('dbl')
    a.b(0x51)                           # push cx
    a.b(0x89, 0xc1)                     # mov cx,ax
    a.b(0x01, 0xc8)                     # add ax,cx
    a.b(0x59)                           # pop cx
    a.b(0xc3)                           # ret
    img = bytes(0x100) + a.code()
    # 2 setup + 10*(add+loop) + call + 5 in the subroutine + 2 stores + hlt
    steps = 2 + 20 + 1 + 5 + 2 + 1
    exp = "ip=0x%04x ax=110 bx=%d cx=0 steps=%d w=cs:0x400:110" % (
        a.at('halt'), len(TAIL), steps)
    return img, exp


def p_small():
    """RUNSMALL.CMD -- the small model, and G-Max 0 read by the guest.

    The data group supplies 17 paragraphs, asks for 512 and declares G-Max
    0.  i86load.c: zero is not a bound of zero, it is "no maximum", so the
    allocation grows from G-Min to the whole 64 KB segment the group owns
    -- and this program does not take the loader's word for it, it reads
    its own data group's size out of the base page and stores it.  A group
    table entry is six bytes and its length is the 24-bit last byte
    offset, so a whole 64 KB segment is 0x00FFFF and the word at DS:7
    holds its top two bytes, 0x00FF.  A loader that read G-Max literally
    would put 0 there; one that stopped at G-Min would put 0x001F.

    The array it sums sits at DS:0x100, immediately above the base page,
    which is also the check that the data image landed at DS:0 with only
    its first 256 bytes overwritten.
    """
    a = Asm(0)
    a.b(0xa1, 0x07, 0x00)               # mov ax,[7]     -- data G size, top
    a.b(0xa3, 0x00, 0x02)               # mov [0x200],ax
    a.b(0xbe, 0x00, 0x01)               # mov si,0x100
    a.b(0xb9, 0x05, 0x00)               # mov cx,5
    a.b(0x31, 0xdb)                     # xor bx,bx
    a.label('sum')
    a.b(0x03, 0x1c)                     # add bx,[si]
    a.b(0x83, 0xc6, 0x02)               # add si,2
    a.rel8(0xe2, 'sum')                 # loop sum
    a.b(0x89, 0x1e, 0x02, 0x02)         # mov [0x202],bx
    a.label('halt')
    a.b(0xf4)                           # hlt
    words = (0x0101, 0x0202, 0x0303, 0x0404, 0x0505)    # sum 0x0f0f
    data = bytes(0x100) + b''.join(w16(v) for v in words)
    steps = 5 + 5 * 3 + 2
    top = (MAXPAR * 16 - 1) >> 8
    exp = ("ip=0x%04x ax=%d bx=0x0f0f steps=%d w=ds:0x200:%d "
           "w=ds:0x202:0x0f0f" % (a.at('halt'), top, steps, top))
    return a.code(), data, exp


def p_8080size():
    """RUN80SZ.CMD -- an 8080-model program sizing its memory.

    DRI's loader copies the code group's length into the data group's
    entry at base page 6 for the 8080 model.  With G-Max 0 the group is a
    whole segment, last byte 0x00FFFF.
    """
    a = Asm(0x100)
    a.b(0xa1, 0x06, 0x00)               # mov ax,[6]
    a.b(0x8a, 0x1e, 0x08, 0x00)         # mov bl,[8]
    a.label('halt')
    a.b(0xf4)                           # hlt
    exp = "ip=0x%04x ax=0xffff bx=0 steps=3" % a.at('halt')
    return bytes(0x100) + a.code(), exp


def p_short():
    """RUNSHORT.CMD -- a file that ends before its code group does.

    The group declares 32 paragraphs and the file holds 17.  The load
    goes ahead and the missing bytes read as zero.
    """
    a = Asm(0x100)
    a.b(0xb8, 0x34, 0x12)               # mov ax,0x1234
    a.b(0xa1, 0xf0, 0x01)               # mov ax,[0x1f0]
    a.label('halt')
    a.b(0xf4)                           # hlt
    exp = "ip=0x%04x ax=0 steps=3" % a.at('halt')
    return bytes(0x100) + a.code(), exp


def p_poll():
    """I86POLL.CMD -- console polling, the target's fixture.

    Reads one key through function 50's CONIN and echoes it, then 64
    times writes a dot with function 6 and polls with E = 0FFh, which
    must answer 0 at once.  Prints I86POLL DONE, or I86POLL KEY if a
    poll saw a key.
    """
    a = Asm(0x100)
    a.b(0xb1, 50)                       # mov cl,50
    a.b(0xba, 0x80, 0x01)               # mov dx,0x180   -- CONIN block
    a.b(0xcd, 0xe0)                     # int 0xe0
    a.b(0x88, 0xc2)                     # mov dl,al
    a.b(0xb1, 0x02)                     # mov cl,2
    a.b(0xcd, 0xe0)                     # int 0xe0       -- echo it
    a.b(0xbe, 0x40, 0x00)               # mov si,64
    a.label('loop')
    a.b(0xb1, 0x06, 0xb2, 0x2e)         # mov cl,6 / mov dl,'.'
    a.b(0xcd, 0xe0)                     # int 0xe0
    a.b(0xb1, 0x06, 0xb2, 0xff)         # mov cl,6 / mov dl,0xff
    a.b(0xcd, 0xe0)                     # int 0xe0
    a.b(0x84, 0xc0)                     # test al,al
    a.rel8(0x75, 'bad')                 # jnz bad
    a.b(0x4e)                           # dec si
    a.rel8(0x75, 'loop')                # jnz loop
    a.b(0xba, 0x90, 0x01)               # mov dx,0x190   -- done text
    a.rel8(0xeb, 'out')                 # jmp out
    a.label('bad')
    a.b(0xba, 0xa0, 0x01)               # mov dx,0x1a0   -- key text
    a.label('out')
    a.b(0xb1, 0x09)                     # mov cl,9
    a.b(0xcd, 0xe0)                     # int 0xe0
    a.b(0xb1, 0x00, 0xb2, 0x00)         # mov cl,0 / mov dl,0
    a.b(0xcd, 0xe0)                     # int 0xe0
    code = a.code()
    if len(code) > 0x80:
        raise ValueError("I86POLL.CMD: code overlaps its data")
    img = bytearray(0x1b0)
    img[0x100:0x100 + len(code)] = code
    img[0x180] = 3                      # CONIN
    img[0x190:0x190 + 16] = b'\r\nI86POLL DONE$'.ljust(16, b'\0')
    img[0x1a0:0x1a0 + 16] = b'\r\nI86POLL KEY$'.ljust(16, b'\0')
    return bytes(img)


def p_keyq():
    """I86KEYQ.CMD -- keys typed while the guest prints.

    Announces itself, writes 200 characters with function 2, then reads
    four keys through function 50's CONIN and echoes each.  The BDOS
    polls the console every eight output characters and keeps what it
    finds, so a run whose console mode still allows that poll never sees
    the keys and waits at its first CONIN.
    """
    a = Asm(0x100)
    a.b(0xba, 0x90, 0x01)               # mov dx,0x190   -- go text
    a.b(0xb1, 0x09)                     # mov cl,9
    a.b(0xcd, 0xe0)                     # int 0xe0
    a.b(0xbe, 0xc8, 0x00)               # mov si,200
    a.label('dots')
    a.b(0xb1, 0x02, 0xb2, 0x2e)         # mov cl,2 / mov dl,'.'
    a.b(0xcd, 0xe0)                     # int 0xe0
    a.b(0x4e)                           # dec si
    a.rel8(0x75, 'dots')                # jnz dots
    a.b(0xbe, 0x04, 0x00)               # mov si,4
    a.label('keys')
    a.b(0xb1, 50)                       # mov cl,50
    a.b(0xba, 0x80, 0x01)               # mov dx,0x180   -- CONIN block
    a.b(0xcd, 0xe0)                     # int 0xe0
    a.b(0x88, 0xc2)                     # mov dl,al
    a.b(0xb1, 0x02)                     # mov cl,2
    a.b(0xcd, 0xe0)                     # int 0xe0       -- echo it
    a.b(0x4e)                           # dec si
    a.rel8(0x75, 'keys')                # jnz keys
    a.b(0xba, 0xa0, 0x01)               # mov dx,0x1a0   -- done text
    a.b(0xb1, 0x09)                     # mov cl,9
    a.b(0xcd, 0xe0)                     # int 0xe0
    a.b(0xb1, 0x00, 0xb2, 0x00)         # mov cl,0 / mov dl,0
    a.b(0xcd, 0xe0)                     # int 0xe0
    code = a.code()
    if len(code) > 0x80:
        raise ValueError("I86KEYQ.CMD: code overlaps its data")
    img = bytearray(0x1b0)
    img[0x100:0x100 + len(code)] = code
    img[0x180] = 3                      # CONIN
    img[0x190:0x190 + 16] = b'\r\nI86KEYQ GO$'.ljust(16, b'\0')
    img[0x1a0:0x1a0 + 16] = b'\r\nI86KEYQ DONE$'.ljust(16, b'\0')
    return bytes(img)


GXPB = 0x200                            # GX86's parameter block
GXMSG = 0x220
GXREC = 0x280                           # its call records
GXEXP = 0x900                           # the surface it expects
GXSURF = 0x1000                         # where the test device draws
GXW, GXH = 64, 32
GXLIVE = 162                            # contrl 6, intin 10, ptsin 8,
GXOFF = (0, 12, 32, 48, 138)            # intout 45, ptsout 12 words
GXNONE = 0xEEEE                         # an output nobody wrote


def gxtodev(x, y):
    """Normalised coordinates that land mid-pixel on the 64 x 32 test
    surface: GDOS takes (2v * pixels) >> 16."""
    return [512 * x + 100, 1024 * y + 200]


def gx32k(v, p):
    """GDOS's TransformTo32K: (v:2p-1) / p, halved; pinned when the
    divide would overflow."""
    if v >= p:
        return 32767
    return ((v << 16) + 2 * p - 1) // p >> 1


def gxrec(contrl, intin, ptsin, cout, intout=(), ptsout=()):
    """One call: what the guest passes, then what it must read back."""
    def words(ws, n, fill):
        ws = list(ws) + [fill] * (n - len(ws))
        return b''.join(w16(w & 0xffff) for w in ws)
    live = (words(contrl, 6, 0) + words(intin, 10, 0) + words(ptsin, 8, 0)
            + words((), 45, GXNONE) + words((), 12, GXNONE))
    exp = (words(cout, 6, 0) + words(intin, 10, 0) + words(ptsin, 8, 0)
           + words(intout, 45, GXNONE) + words(ptsout, 12, GXNONE))
    assert len(live) == len(exp) == GXLIVE
    return live + exp


def gxsurface():
    """The 64 x 32 x 1 surface after the calls, pixel by pixel; raster
    row = 31 - device y, most significant bit leftmost."""
    px = set()
    for x in range(2, 21):              # polyline (2,28)-(20,28)
        px.add((x, 3))
    for r in range(3, 12):              # ... -(20,20)
        px.add((20, r))
    for r in range(19, 27):             # solid fill (30,5)..(40,12),
        for x in range(30, 41):         # outline included
            px.add((x, r))
    tee = [0x7F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00]
    for i, b in enumerate(tee):         # 'T' at (44,4): top row is
        for c in range(8):              # 4 + 6 above the point
            if b >> (7 - c) & 1:
                px.add((44 + c, 21 + i))
    s = bytearray(GXW // 8 * GXH)
    for x, r in px:
        s[r * GXW // 8 + x // 8] |= 0x80 >> (x % 8)
    return bytes(s)


def p_gsx86():
    """GX86.CMD -- GSX-86 through INT 0E0h, CX = 0473h.

    Five calls: Open Workstation, polyline, fill area, text, Close.  After
    each it compares its arrays with the answers beside them, then the
    test device's surface in its own segment with the drawing worked out
    in gxsurface().  Prints GX86 OK, or GX86 FAIL n naming the call (6 is
    the surface).  With no device, GDOS's own refusal ends it at once.
    """
    a = Asm(0x100)
    a.b(0x8c, 0xd8)                     # mov ax,ds
    a.b(0x8e, 0xc0)                     # mov es,ax
    for k in range(5):
        a.b(0xa3, *w16(GXPB + 4 * k + 2))   # mov [pb+4k+2],ax
    a.b(0xbb, *w16(GXREC))              # mov bx,records
    a.b(0xbd, 0x05, 0x00)               # mov bp,5
    a.b(0xfc)                           # cld
    a.label('call')
    a.b(0x89, 0xd8)                     # mov ax,bx
    a.b(0xa3, *w16(GXPB))               # mov [pb],ax
    for k in range(1, 5):
        a.b(0x05, *w16(GXOFF[k] - GXOFF[k - 1]))    # add ax,next
        a.b(0xa3, *w16(GXPB + 4 * k))               # mov [pb+4k],ax
    a.b(0xb9, 0x73, 0x04)               # mov cx,0473h
    a.b(0xba, *w16(GXPB))               # mov dx,pb
    a.b(0xcd, 0xe0)                     # int 0xe0
    a.b(0x89, 0xde)                     # mov si,bx
    a.b(0x8d, 0xbf, *w16(GXLIVE))       # lea di,[bx+live]
    a.b(0xb9, *w16(GXLIVE))             # mov cx,live
    a.b(0xf3, 0xa6)                     # repe cmpsb
    a.rel8(0x75, 'fail')                # jne fail
    a.b(0x81, 0xc3, *w16(2 * GXLIVE))   # add bx,record
    a.b(0xfe, 0x06, *w16(GXMSG + 0x1c)) # inc byte [digit]
    a.b(0x4d)                           # dec bp
    a.rel8(0x75, 'call')                # jnz call
    a.b(0xbe, *w16(GXSURF))             # mov si,surface
    a.b(0xbf, *w16(GXEXP))              # mov di,expected
    a.b(0xb9, *w16(GXW // 8 * GXH + 16))    # mov cx,surface+guard
    a.b(0xf3, 0xa6)                     # repe cmpsb
    a.rel8(0x75, 'fail')                # jne fail
    a.b(0xba, *w16(GXMSG))              # mov dx,ok
    a.rel8(0xeb, 'out')                 # jmp out
    a.label('fail')
    a.b(0xba, *w16(GXMSG + 0x10))       # mov dx,fail
    a.label('out')
    a.b(0xb1, 0x09)                     # mov cl,9
    a.b(0xcd, 0xe0)                     # int 0xe0
    a.b(0xb1, 0x00, 0xb2, 0x00)         # mov cl,0 / mov dl,0
    a.b(0xcd, 0xe0)                     # int 0xe0
    return gximage(a.code(), 'GX86')


def gximage(code, name):
    """The image from 0: code at 0x100, then the calls, their answers and
    the surface, the same for both guests."""
    if len(code) > GXPB - 0x100:
        raise ValueError("%s: code overlaps its data" % name)

    # Open Workstation on 64 x 32 x 1: the device's extents, pixel size
    # for a 256 x 192 mm picture, two colours; the rest is its table.
    intout = [-1] * 45
    intout[0:16] = [GXW - 1, GXH - 1, 1, 256000 // GXW, 192000 // GXH,
                    8, 8, 1, 8, 1, 1, 8, 8, 2, 1, 1]
    intout[25] = 3
    intout[35:45] = [0, 1, 1, 0, 2, 0, 0, 0, 0, 0]
    raw = [7, 7, 56, 56, 1, 0, 1, 0, 0, 7, 0, 7]
    ptsout = [gx32k(v, GXH if i & 1 else GXW) for i, v in enumerate(raw)]
    x = 0x7777                          # a count the driver must set
    recs = [
        gxrec([1, 0, x, 10, x, 0], [1] * 10, [],
              [1, 0, 6, 10, 45, 0], intout, ptsout),
        gxrec([6, 3, x, 0, x, 0], [],
              gxtodev(2, 28) + gxtodev(20, 28) + gxtodev(20, 20),
              [6, 3, 0, 0, 0, 0]),
        gxrec([9, 4, x, 0, x, 0], [],
              gxtodev(30, 5) + gxtodev(40, 5) + gxtodev(40, 12)
              + gxtodev(30, 12),
              [9, 4, 0, 0, 0, 0]),
        gxrec([8, 1, x, 1, x, 0], [ord('T')], gxtodev(44, 4),
              [8, 1, 0, 1, 0, 0]),
        gxrec([2, 0, x, 0, x, 0], [], [], [2, 0, 0, 0, 0, 0]),
    ]
    img = bytearray(GXSURF + GXW // 8 * GXH + 16)
    img[0x100:0x100 + len(code)] = code
    img[GXMSG:GXMSG + 16] = ('\r\n%s OK$' % name).encode().ljust(16, b'\0')
    img[GXMSG + 0x10:GXMSG + 0x20] = (
        '\r\n%s FAIL 1$' % name).encode().ljust(16, b'\0')
    img[GXMSG + 0x20:GXMSG + 0x30] = (
        '\r\n%s REFUSED$' % name).encode().ljust(16, b'\0')
    r = b''.join(recs)
    if GXREC + len(r) > GXEXP:
        raise ValueError("%s: records overlap the surface answer" % name)
    img[GXREC:GXREC + len(r)] = r
    # Open Workstation clears the 0x55 the surface starts as; the guard
    # after it must survive.
    img[GXEXP:GXEXP + GXW // 8 * GXH] = gxsurface()
    img[GXEXP + GXW // 8 * GXH:GXEXP + GXW // 8 * GXH + 16] = b'\x55' * 16
    img[GXSURF:] = b'\x55' * (GXW // 8 * GXH + 16)
    return bytes(img)


def p_gsx80():
    """GX80.COM -- GX86's calls from an 8080, through BDOS 115 with DE
    at five 16-bit addresses.  An Open that returns no intout prints
    GX80 REFUSED."""
    recp, cnt = GXPB + 12, GXPB + 14
    a = Asm(0x100)
    a.b(0x21, *w16(GXREC))              # lxi h,records
    a.b(0x22, *w16(recp))               # shld recp
    a.b(0x3e, 5, 0x32, *w16(cnt))       # mvi a,5 / sta cnt
    a.label('call')
    a.b(0x2a, *w16(recp))               # lhld recp
    a.b(0x22, *w16(GXPB))               # shld pb
    for k in range(1, 5):
        a.b(0x11, *w16(GXOFF[k] - GXOFF[k - 1]))    # lxi d,next
        a.b(0x19)                                   # dad d
        a.b(0x22, *w16(GXPB + 2 * k))               # shld pb+2k
    a.b(0x0e, 0x73, 0x11, *w16(GXPB))   # mvi c,73h / lxi d,pb
    a.b(0xcd, 0x05, 0x00)               # call 5
    a.b(0x3a, *w16(cnt), 0xfe, 5)       # lda cnt / cpi 5
    a.abs16(0xc2, 'live')               # jnz live
    a.b(0x2a, *w16(recp))               # lhld recp
    a.b(0x11, 8, 0, 0x19)               # lxi d,8 / dad d
    a.b(0x7e, 0x23, 0xb6)               # mov a,m / inx h / ora m
    a.abs16(0xca, 'refused')            # jz refused
    a.label('live')
    a.b(0x2a, *w16(recp), 0xeb)         # lhld recp / xchg
    a.b(0x21, *w16(GXLIVE), 0x19)       # lxi h,live / dad d
    a.b(0x01, *w16(GXLIVE))             # lxi b,live
    a.abs16(0xcd, 'cmpn')               # call cmpn
    a.b(0x22, *w16(recp))               # shld recp
    a.b(0x21, *w16(GXMSG + 0x1c), 0x34) # lxi h,digit / inr m
    a.b(0x21, *w16(cnt), 0x35)          # lxi h,cnt / dcr m
    a.abs16(0xc2, 'call')               # jnz call
    a.b(0x11, *w16(GXSURF))             # lxi d,surface
    a.b(0x21, *w16(GXEXP))              # lxi h,expected
    a.b(0x01, *w16(GXW // 8 * GXH + 16))    # lxi b,surface+guard
    a.abs16(0xcd, 'cmpn')               # call cmpn
    a.b(0x11, *w16(GXMSG))              # lxi d,ok
    a.abs16(0xc3, 'out')                # jmp out
    a.label('refused')
    a.b(0x11, *w16(GXMSG + 0x20))       # lxi d,refused
    a.abs16(0xc3, 'out')                # jmp out
    a.label('fail')
    a.b(0x11, *w16(GXMSG + 0x10))       # lxi d,fail
    a.label('out')
    a.b(0x0e, 0x09, 0xcd, 0x05, 0x00)   # mvi c,9 / call 5
    a.b(0xc3, 0x00, 0x00)               # jmp 0
    # BC bytes at DE and HL must match
    a.label('cmpn')
    a.b(0x1a, 0xbe)                     # ldax d / cmp m
    a.abs16(0xc2, 'fail')               # jnz fail
    a.b(0x13, 0x23, 0x0b)               # inx d / inx h / dcx b
    a.b(0x78, 0xb1)                     # mov a,b / ora c
    a.abs16(0xc2, 'cmpn')               # jnz cmpn
    a.b(0xc9)                           # ret
    return gximage(a.code(), 'GX80')[0x100:]


def p_multi():
    """I86MG.CMD -- the large model, and the only multi-group .CMD there is.

    All 15 files in DRI's cpm86pc drop are small model or 8080, so nothing
    real declares an extra, a stack or an auxiliary group and the compact
    and large paths of src/shim/i86load.c i86place() have no corpus behind
    them.  This is that corpus: five groups -- code, data, extra, stack and
    aux 1 -- built here from the format description, which is why nothing
    binary is shipped for it.

    IT DOES NOT TAKE THE LOADER'S WORD FOR ANY OF IT.  Every claim
    i86place() makes is checked from inside the guest, by a program that
    can only pass if the claim holds:

      1. ES is a DIFFERENT segment from DS.  It writes 0xBEEF at ES:0x300,
         then 0xCAFE at DS:0x300, then reads ES:0x300 back.  One segment
         behind both -- which is what the loader did before the extra
         group had a segment of its own -- gives 0xCAFE and fails here.
      2. SS is a different segment again.  It pushes the marker and reads
         it back through an explicit SS: override at the address the push
         must have used.
      3. The auxiliary group has a segment and NO segment register, so the
         only way to it is the base page: the program loads its paragraph
         from DS:0x1b -- the base word of the fifth six-byte group-table
         entry, which i86bpage() fills from g->par -- into ES, and writes
         and reads through it.
         i86resolve() has to answer that paragraph without a slow-path
         escape, which is what makes `slow segment resolutions 0' on the
         target a statement about this program.

    Then it prints and exits through the BDOS, so the same file is a target
    gate (tests/verify.mk verify-i86) and not only a host fixture.  The
    manifest entry stops at the FIRST INT, where DX names which of the two
    messages is about to be printed: the expectations below are what the
    registers hold if and only if all three checks passed.
    """
    a = Asm(0)
    a.b(0xb8, 0xef, 0xbe)               # mov ax,0xbeef
    a.b(0x26, 0xa3, 0x00, 0x03)         # mov es:[0x300],ax
    a.b(0xbb, 0xfe, 0xca)               # mov bx,0xcafe
    a.b(0x89, 0x1e, 0x00, 0x03)         # mov [0x300],bx      -- into DS
    a.b(0x26, 0x8b, 0x0e, 0x00, 0x03)   # mov cx,es:[0x300]
    a.b(0x39, 0xc1)                     # cmp cx,ax
    a.rel8(0x75, 'fail')                # jne fail            -- 1: ES != DS
    a.b(0xbc, 0x00, 0x04)               # mov sp,0x400
    a.b(0x50)                           # push ax
    a.b(0x5a)                           # pop dx
    a.b(0x39, 0xc2)                     # cmp dx,ax
    a.rel8(0x75, 'fail')
    a.b(0x36, 0x8b, 0x3e, 0xfe, 0x03)   # mov di,ss:[0x3fe]
    a.b(0x39, 0xc7)                     # cmp di,ax
    a.rel8(0x75, 'fail')                # jne fail            -- 2: SS
    a.b(0x8b, 0x36, 0x1b, 0x00)         # mov si,[0x1b]  -- aux1 paragraph
    a.b(0x8e, 0xc6)                     # mov es,si
    a.b(0xb8, 0xaa, 0x55)               # mov ax,0x55aa
    a.b(0x26, 0xa3, 0x00, 0x02)         # mov es:[0x200],ax
    a.b(0x26, 0x8b, 0x2e, 0x00, 0x02)   # mov bp,es:[0x200]
    a.b(0x39, 0xc5)                     # cmp bp,ax
    a.rel8(0x75, 'fail')                # jne fail            -- 3: aux 1
    a.b(0xba, 0x00, 0x01)               # mov dx,MSGOK
    a.label('print')
    a.b(0xb9, 0x09, 0x00)               # mov cx,9       -- print string
    a.b(0xcd, 0xe0)                     # int 0e0h
    a.b(0xb9, 0x00, 0x00)               # mov cx,0       -- system reset
    a.b(0xcd, 0xe0)                     # int 0e0h
    a.label('fail')
    a.b(0xba, 0x10, 0x01)               # mov dx,MSGBAD
    a.b(0xb9, 0x09, 0x00)
    a.b(0xcd, 0xe0)
    a.b(0xb9, 0x00, 0x00)
    a.b(0xcd, 0xe0)

    # The two messages, in the DATA group above the base page: the first
    # 256 bytes of a small/compact/large data image are overwritten by
    # i86bpage(), so nothing may live there.
    msgs = bytearray(0x100)
    msgs += b"I86MG OK\r\n$"             # at DS:0x100
    msgs += bytes(0x110 - len(msgs))
    msgs += b"I86MG BAD\r\n$"            # at DS:0x110

    # Check 1 is six instructions and its JNE; check 2 is four and its
    # JNE; check 3's SS: read is two and its JNE; the auxiliary group is
    # six and its JNE; then MOV DX, MOV CX and the INT itself, which is
    # the step ldrun() stops on.
    steps = (6 + 1) + (4 + 1) + (2 + 1) + (6 + 1) + 1 + 1 + 1
    exp = ("ip=0x%04x ax=0x55aa bx=0xcafe cx=9 dx=0x100 sp=0x400 bp=0x55aa "
           "di=0xbeef si=0x5000 steps=%d w=ds:0x300:0xcafe "
           "w=ss:0x3fe:0xbeef w=es:0x200:0x55aa"
           % (a.at('print') + 5, steps))
    return a.code(), bytes(msgs), exp


def p_pload():
    """I86PL.CMD -- a guest that loads a guest, through BDOS function 59.

    Function 59 is how a CP/M-86 debugger gets the program it debugs into
    memory.  Nothing DRI shipped but DDT86 calls it, and DDT86's answer is
    an interactive transcript, so this is the caller: it reads the console
    width out of the system data (49), loads PIP.CMD by the FCB the command
    tail filled, and gives the segments back (57).

    IT DOES NOT TAKE THE LOADER'S WORD FOR ANY OF IT.  The answer is a
    paragraph, and the program reads through it: the group table at the
    base page names the code and data groups, the data base must be the
    base page's own paragraph, and PIP's first four bytes -- 9C 58 FA 8C,
    its prologue -- must be at the code paragraph named.  Those bytes can
    only be there if the load really read the file.
    """
    a = Asm(0)
    a.b(0xbe, 0xff, 0xff)               # mov si,0xffff  -- the refusal
    a.b(0xb9, 0x31, 0x00)               # mov cx,49      -- get SCB
    a.b(0xba, 0x00, 0x02)               # mov dx,0x200
    a.b(0xcd, 0xe0)                     # int 0e0h
    a.b(0x26, 0x80, 0x7f, 0x40, 0x50)   # cmp byte es:[bx+40h],80
    a.rel8(0x75, 'fail')                #                -- console width
    a.b(0xb9, 0x3b, 0x00)               # mov cx,59      -- program load
    a.b(0xba, 0x5c, 0x00)               # mov dx,0x5c    -- the tail's FCB
    a.b(0xcd, 0xe0)
    a.b(0x39, 0xf0)                     # cmp ax,si
    a.rel8(0x74, 'fail')                # je fail        -- 0FFFFh
    a.b(0x8e, 0xc0)                     # mov es,ax      -- its base page
    a.b(0x26, 0x8b, 0x1e, 0x03, 0x00)   # mov bx,es:[3]  -- code base
    a.b(0x26, 0x8b, 0x16, 0x09, 0x00)   # mov dx,es:[9]  -- data base
    a.b(0x39, 0xc2)                     # cmp dx,ax      -- base page in it
    a.rel8(0x75, 'fail')
    a.b(0x8e, 0xc3)                     # mov es,bx
    a.b(0x26, 0x8b, 0x3e, 0x00, 0x00)   # mov di,es:[0]
    a.b(0x26, 0x8b, 0x2e, 0x02, 0x00)   # mov bp,es:[2]
    a.b(0xb8, 0x9c, 0x58)               # mov ax,0x589c  -- PIP's prologue
    a.b(0x39, 0xc7)                     # cmp di,ax
    a.rel8(0x75, 'fail')
    a.b(0xb8, 0xfa, 0x8c)               # mov ax,0x8cfa
    a.b(0x39, 0xc5)                     # cmp bp,ax
    a.rel8(0x75, 'fail')
    a.b(0x31, 0xc0)                     # xor ax,ax      -- MCB base 0:
    a.b(0xa3, 0x00, 0x02)               # mov [0x200],ax    all of it
    a.b(0xa3, 0x02, 0x02)               # mov [0x202],ax
    a.b(0xb9, 0x39, 0x00)               # mov cx,57      -- free memory
    a.b(0xba, 0x00, 0x02)               # mov dx,0x200
    a.b(0xcd, 0xe0)
    a.b(0xba, 0x00, 0x01)               # mov dx,MSGOK
    a.b(0xb9, 0x09, 0x00)               # print string
    a.b(0xcd, 0xe0)
    a.b(0xb9, 0x00, 0x00)               # system reset
    a.b(0xcd, 0xe0)
    a.label('fail')
    a.b(0xba, 0x10, 0x01)               # mov dx,MSGBAD
    a.b(0xb9, 0x09, 0x00)
    a.b(0xcd, 0xe0)
    a.b(0xb9, 0x00, 0x00)
    a.b(0xcd, 0xe0)

    msgs = bytearray(0x100)
    msgs += b"I86PL OK\r\n$"             # at DS:0x100
    msgs += bytes(0x110 - len(msgs))
    msgs += b"I86PL BAD\r\n$"            # at DS:0x110
    return a.code(), bytes(msgs)


def p_refuse(bad, comment):
    """RUNUNIMP.CMD and RUNBAD.CMD -- the same program twice, differing in
    one byte, so that "decoded, not implemented" and "not an instruction"
    are told apart by the fixture and not only by the return code.  Both
    must stop AT the offending byte with the work before it done: a refusal
    that cannot name its own address is not a refusal, it is a crash."""
    a = Asm(0x100)
    a.b(0xb8, 0x34, 0x12)               # mov ax,0x1234
    a.label('stop')
    a.b(bad)                            # the byte under test: %s
    a.b(0xf4)                           # hlt (never reached)
    img = bytes(0x100) + a.code()
    exp = "ip=0x%04x ax=0x1234 steps=2" % a.at('stop')
    return img, exp + "  # " + comment


# --------------------------------------------------------------- DRI hex

# GENCMD.CMD's input format, and it is Intel hex only in its punctuation.
# A record is
#
#	:LL AAAA TT <LL data bytes> CC
#
# with LL the byte count, AAAA the load offset, TT the record type and CC
# the two's-complement checksum of everything between the colon and it.
# What DRI added is the TYPE FIELD: 00 is an ordinary Intel data record,
# 01 an end of file and 03 a start address, but 81 says "this data belongs
# to the CODE segment" and 82 "to the DATA segment" -- that is how one hex
# file carries the several groups a .CMD header has to describe.
#
# READ OFF A REAL FILE, not remembered: the CCP/M source drop's
# LBDOS.H86 (cpm8000/ref/incoming/ccpmsrc/CCPM/) opens with
# `:0400000300000000F9' -- a type 03 start address of 0000:0000, whose
# checksum verifies as 0x100 - 7 -- and then runs type 81 records from
# offset 0000 through type 82 records for its data.  Everything below
# is that shape.
H86_EOF   = 0x01
H86_START = 0x03
H86_CODE  = 0x81
H86_DATA  = 0x82

# Records are 27 data bytes wide because LBDOS.H86's are (0x1B).
H86_WIDTH = 27


def h86rec(typ, addr, data):
    body = bytes([len(data), (addr >> 8) & 0xff, addr & 0xff, typ]) + data
    ck = (-sum(body)) & 0xff
    return (':' + ''.join('%02X' % b for b in body) + '%02X' % ck
            + '\r\n').encode('ascii')


def h86group(typ, org, data):
    """One segment's image as a run of records, `H86_WIDTH' bytes each.

    Leading zeros are NOT emitted: a hex file states the bytes it has and
    the loader fills the rest, which is exactly how the data group's first
    256 bytes -- the base page's own space -- come to be absent below."""
    out = b''
    i = 0
    while i < len(data):
        chunk = data[i:i + H86_WIDTH]
        out += h86rec(typ, org + i, chunk)
        i += len(chunk)
    return out


def p_hex():
    """I86HEX.H86 -- a real hex file for GENCMD.CMD to convert.

    src/shim/tests/i86corpus/ can supply a .CMD to load but nothing to FEED a
    corpus binary, and GENCMD is the one of the four whose input is not a
    command line: it reads FILENAME.H86 and writes FILENAME.CMD.  Without
    an .H86 the furthest it can be driven is "CANNOT OPEN SOURCE", which
    tests the default FCB and nothing else.

    So this is a program, hand-assembled here so the encodings stay
    visible, in the small model GENCMD will infer from the segments the
    records name:

	code, at CS:0000		data, at DS:0100
	    mov dx,0100h		    "I86HEX OK", CR, LF, '$'
	    mov cl,9   ; print string
	    int 0E0h
	    mov cl,0   ; system reset
	    int 0E0h

    DS:0100 and not DS:0000 because the small model puts the base page at
    DS:0000 (src/shim/i86load.c i86hdr), so the first 256 bytes of a data
    group are not the program's to use.  INT 0E0h because that is the
    CP/M-86 BDOS entry and therefore our seam.

    The point of choosing a program that PRINTS is that the .CMD GENCMD
    writes can then be loaded and run by this same shim, so the round
    trip is checked by its output and not by re-reading the header we
    would have written ourselves.
    """
    a = Asm(0)
    a.b(0xba, 0x00, 0x01)               # mov dx,0100h
    a.b(0xb1, 0x09)                     # mov cl,9      -- print string
    a.b(0xcd, 0xe0)                     # int 0E0h
    a.b(0xb1, 0x00)                     # mov cl,0      -- system reset
    a.b(0xcd, 0xe0)                     # int 0E0h
    msg = b'I86HEX OK\r\n$'
    hexf = h86rec(H86_START, 0, bytes(4))
    hexf += h86group(H86_CODE, 0, a.code())
    hexf += h86group(H86_DATA, 0x100, msg)
    hexf += h86rec(H86_EOF, 0, b'')
    # A CP/M text file ends with ^Z and is a whole number of 128-byte
    # records; GENCMD reads it with sequential reads and stops at the ^Z.
    hexf += b'\x1a'
    if len(hexf) % 128:
        hexf += b'\x1a' * (128 - len(hexf) % 128)
    return hexf, msg


def textfile(s):
    """CRLF lines, ^Z, padded to a whole record."""
    b = s.replace('\n', '\r\n').encode('ascii') + b'\x1a'
    return b + b'\x1a' * (-len(b) % 128)


def p_asm():
    """I86T.A86 -- ASM86's input: print a string, then system reset."""
    return textfile(
        "\tCSEG\n"
        "\tMOV\tCL,9\n"
        "\tMOV\tDX,OFFSET MSG\n"
        "\tINT\t224\n"
        "\tMOV\tCL,0\n"
        "\tMOV\tDL,0\n"
        "\tINT\t224\n"
        "\tDSEG\n"
        "\tORG\t100H\n"
        "MSG\tDB\t'HELLO FROM ASM86$'\n"
        "\tEND\n")


# --------------------------------------------------------------------- main

def main(argv):
    if len(argv) != 2:
        sys.exit("usage: mkcmdfix.py <destdir>")
    d = argv[1]
    if not os.path.isdir(d):
        os.makedirs(d)
    man = []
    n = 0

    def load(name, groups, expect, keys='', images=None, cut=None):
        write(d, name, groups, images, cut)
        man.append("LOAD %s %s %s" % (name, expect, keys))

    # ---- K1: the two refusals no real .CMD can produce.  All 15 files in
    # DRI's cpm86pc drop carry A-Base 0 and no group over 4,096 paragraphs
    # (the merged sweep's measurement), so this is the whole evidence there
    # is that the check fires at all.
    load('ABASE.CMD', [(G_CODE, 8, 0x0800, 8, 0)], 'CE_BASE')
    # An in-file overlap is not representable: a group's image offset is
    # DERIVED from the G-Lengths before it, in descriptor order, so the
    # images tile the file by construction.  The only overlap the format
    # can express is two absolute groups claiming the same paragraphs, and
    # the first A-Base refuses before the second is read.
    load('OVERLAP.CMD', [(G_CODE, 16, 0x1000, 16, 0),
                         (G_DATA, 16, 0x1008, 16, 0)], 'CE_BASE')
    load('BIGLEN.CMD', [(G_CODE, MAXPAR + 1, 0, MAXPAR + 1, 0)], 'CE_BIG')
    load('BIGMIN.CMD', [(G_CODE, 8, 0, 5000, 0)], 'CE_BIG')
    load('BIGMAX.CMD', [(G_CODE, 8, 0, 8, 9000)], 'CE_BIG')

    # ---- malformed files.  SHORTHDR.CMD is 40 bytes that are not a
    # header at all -- a truncated copy, or something misnamed -- and it is
    # why i86hdr() checks its own length before it reads a descriptor.
    # Without that check the missing 88 bytes are whatever the caller's
    # buffer held, and the refusal is "no code group": a statement about a
    # header we never had.  Every other short file is caught later anyway,
    # by the images not fitting; this one is not.
    load('SHORTHDR.CMD', [], 'CE_TRUNC', cut=40)
    # A header whose images end early loads: DRI's loader reads what is
    # there and does not refuse.
    load('HDRONLY.CMD', [(G_CODE, 8, 0, 8, 0)], 'CE_OK', cut=HDR)
    load('PASTEOF.CMD', [(G_CODE, 100, 0, 100, 0)], 'CE_OK',
         cut=HDR + PARA)
    load('SHCODE.CMD', [(G_CODE, 8, 0, 8, 0), (G_SHCODE, 8, 0, 8, 0)],
         'CE_FORM')
    load('FORM12.CMD', [(G_CODE, 8, 0, 8, 0), (12, 8, 0, 8, 0)], 'CE_FORM')
    load('DUPCODE.CMD', [(G_CODE, 8, 0, 8, 0), (G_CODE, 8, 0, 8, 0)],
         'CE_DUP')
    load('NOCODE.CMD', [(G_DATA, 8, 0, 8, 0)], 'CE_NOCODE')

    # ---- valid, and at the edges of galloc()'s rule.
    load('GMAX0.CMD', [(G_CODE, 5, 0, 5, 0), (G_DATA, 5, 0, 200, 0)],
         'CE_OK', 'model=small entry=0 ng=2 alloc=%d,%d need=288'
         % (MAXPAR, MAXPAR))
    # The same file with a G-Max that says something: the ask is
    # honoured, and it is neither G-Min nor the whole segment.
    load('GMAX300.CMD', [(G_CODE, 5, 0, 5, 900), (G_DATA, 5, 0, 200, 300)],
         'CE_OK', 'model=small entry=0 ng=2 alloc=900,300 need=288')
    # Exactly one segment is legal; one paragraph more is CE_BIG above.
    load('FULLSEG.CMD', [(G_CODE, 8, 0, MAXPAR, MAXPAR)], 'CE_OK',
         'model=8080 entry=0x100 ng=1 alloc=4096 need=256')
    # G-Max BELOW G-Min.  G-Min is what the program needs and G-Max what it
    # will accept, so the two cannot both be honoured; the loader follows
    # G-Min and does not cap.  Pinned because it is a choice, not a law --
    # and it is the one direction galloc()'s growth must not go.
    load('MAXLT.CMD', [(G_CODE, 8, 0, 8, 0), (G_DATA, 4, 0, 64, 10)],
         'CE_OK', 'model=small entry=0 ng=2 alloc=%d,64 need=320' % MAXPAR)
    # A declared group that supplies and needs nothing still gets memory:
    # galloc() never returns zero, so CE_EMPTY cannot fire.  With G-Max 0
    # on both groups that is the whole segment each.
    load('ZEROLEN.CMD', [(G_CODE, 4, 0, 4, 0), (G_DATA, 0, 0, 0, 0)],
         'CE_OK', 'model=small entry=0 ng=2 alloc=%d,%d need=192'
         % (MAXPAR, MAXPAR))

    # ---- the four that run.
    img, exp = p_8080()
    # G-Min far above G-Length, the way every DRI utility is built: the
    # difference is BSS, and here it is also where the stack goes.
    write(d, 'RUN8080.CMD', [(G_CODE, npar(img), 0, 512, 0)], [img])
    man.append("RUN RUN8080.CMD X_HALT " + exp)

    code, data, exp = p_small()
    write(d, 'RUNSMALL.CMD',
          [(G_CODE, npar(code), 0, npar(code), 0),
           (G_DATA, npar(data), 0, 512, 0)], [code, data])
    man.append("RUN RUNSMALL.CMD X_HALT " + exp)

    img, exp = p_8080size()
    write(d, 'RUN80SZ.CMD', [(G_CODE, npar(img), 0, 512, 0)], [img])
    man.append("RUN RUN80SZ.CMD X_HALT " + exp)

    img, exp = p_short()
    write(d, 'RUNSHORT.CMD', [(G_CODE, 32, 0, 32, 32)], [img],
              cut=HDR + len(img))
    man.append("RUN RUNSHORT.CMD X_HALT " + exp)

    img, exp = p_refuse(0xce, 'into: a real instruction we do not run')
    write(d, 'RUNUNIMP.CMD', [(G_CODE, npar(img), 0, 64, 0)], [img])
    man.append("RUN RUNUNIMP.CMD X_UNIMP " + exp)

    img, exp = p_refuse(0xd6, '0xd6: no 8086 instruction at all')
    write(d, 'RUNBAD.CMD', [(G_CODE, npar(img), 0, 64, 0)], [img])
    man.append("RUN RUNBAD.CMD X_BAD " + exp)

    # ---- the multi-group one, which is both a host fixture and a target
    # gate.  Five groups: code and data with images, and an extra, a stack
    # and an auxiliary group that supply no bytes at all -- G-Length 0 --
    # because what they are for is ADDRESS SPACE.  G-Max 0 on each, so
    # galloc() gives each the whole 64 KB segment it owns; the guest's
    # writes at 0x200-0x400 are inside every one of them.
    code, data, exp = p_multi()
    write(d, 'I86MG.CMD',
          [(G_CODE, npar(code), 0, npar(code), 0),
           (G_DATA, npar(data), 0, 512, 0),
           (G_EXTRA, 0, 0, 256, 0),
           (G_STACK, 0, 0, 256, 0),
           (G_AUX1, 0, 0, 256, 0)], [code, data])
    man.append("LOAD I86MG.CMD CE_OK model=large entry=0 ng=5 "
               "alloc=%d,%d,%d,%d,%d" % ((MAXPAR,) * 5))
    man.append("RUN I86MG.CMD X_INT " + exp)

    # ---- the function 59 caller.  No manifest entry: a fixture run stops
    # at the first INT, which here is the third instruction, so the host
    # suite calls the seam directly instead and this file is the target's.
    code, data = p_pload()
    write(d, 'I86PL.CMD',
          [(G_CODE, npar(code), 0, npar(code), 0),
           (G_DATA, npar(data), 0, 512, 0)], [code, data])

    img = p_poll()
    write(d, 'I86POLL.CMD', [(G_CODE, npar(img), 0, 512, 0)], [img])

    img = p_keyq()
    write(d, 'I86KEYQ.CMD', [(G_CODE, npar(img), 0, 512, 0)], [img])

    img = p_gsx86()
    write(d, 'GX86.CMD', [(G_CODE, npar(img), 0, 512, 0)], [img])

    # ---- and the one fixture that is not a .CMD: GENCMD's input.
    hexf, msg = p_hex()
    open(os.path.join(d, 'I86HEX.H86'), 'wb').write(hexf)
    open(os.path.join(d, 'I86T.A86'), 'wb').write(p_asm())
    open(os.path.join(d, 'GX80.COM'), 'wb').write(p_gsx80())

    n = len(man)
    fp = open(os.path.join(d, 'MANIFEST'), 'w')
    fp.write("# generated by tools/mkcmdfix.py -- do not edit\n")
    for line in man:
        fp.write(line.rstrip() + "\n")
    fp.close()
    print("mkcmdfix: %d fixtures in %s, plus I86HEX.H86 (%d bytes)"
          % (n, d, len(hexf)))


main(sys.argv)
