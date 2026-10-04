# MacBinary to A/UX AppleDouble: the data fork goes to <dir>/<name>, the
# Finder info and resource fork to <dir>/%<name>.  Names stay Mac Roman.
# usage: macbin2ad.py [-n name] file.bin dir
import sys, os, struct

def ad_header(finfo, rlen):
    # version 1, home file system "Macintosh", entries: Finder info, resource fork
    h = struct.pack('>II16sH', 0x00051607, 0x00010000, b'Macintosh       ', 2)
    h += struct.pack('>III', 9, 0xe0, 32) + struct.pack('>III', 2, 0x200, rlen)
    h = h.ljust(0xe0, b'\0') + finfo.ljust(32, b'\0')
    return h.ljust(0x200, b'\0')

def convert(data, dir, name=None):
    if len(data) < 128 or data[0] != 0 or not 1 <= data[1] <= 63 or data[74] != 0:
        raise ValueError('not MacBinary')
    dlen, rlen = struct.unpack('>II', data[83:91])
    if name is None:
        name = data[2:2 + data[1]]
    if isinstance(name, str):
        name = name.encode('mac_roman')
    if not name or b'/' in name or b'\0' in name or name in (b'.', b'..'):
        raise ValueError('bad file name')
    # Finder flags: high byte at 73, low at 101; drop "inited" so the Finder
    # places the icon itself
    flags = (data[73] << 8 | data[101]) & ~0x0100
    finfo = data[65:73] + struct.pack('>HIH', flags, 0, 0)
    doff = 128
    roff = doff + (dlen + 127) // 128 * 128
    rsrc = data[roff:roff + rlen]
    if len(data[doff:doff + dlen]) != dlen or len(rsrc) != rlen:
        raise ValueError('short MacBinary file')
    d = os.fsencode(dir)
    with open(os.path.join(d, name), 'xb') as f:
        f.write(data[doff:doff + dlen])
    with open(os.path.join(d, b'%' + name), 'xb') as f:
        f.write(ad_header(finfo, rlen) + rsrc)
    return name

if __name__ == '__main__':
    a = sys.argv[1:]
    name = None
    if a[:1] == ['-n']:
        name, a = a[1], a[2:]
    if len(a) != 2:
        sys.exit('usage: macbin2ad.py [-n name] file.bin dir')
    convert(open(a[0], 'rb').read(), a[1], name)
