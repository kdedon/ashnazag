# Give user Finders a Log Out command through the existing Shut Down handler.
import sys, os, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../tools'))
import rsrcedit


def logout_menu(r, title, fixed):
    if len(r) < title + 1 or r[title:title + 8] != b'\x07Special':
        raise ValueError('unknown Special menu header')
    h = title + 1 + r[title] + ((r[title] + 1) & 1)
    p, items = h, []
    while p < len(r):
        if p + fixed >= len(r):
            raise ValueError('truncated Special menu item')
        n = fixed + 1 + r[p + fixed] + ((r[p + fixed] + 1) & 1)
        if p + n > len(r):
            raise ValueError('truncated Special menu title')
        items.append(r[p:p + n])
        p += n
    count, = struct.unpack('>H', r[2:4])
    if count != len(items) + 1 or sum(i[:4] == b'shut' for i in items) != 1:
        raise ValueError('unknown Special menu commands')
    out = []
    for item in items:
        if item[:4] == b'rest':
            continue
        if item[:4] == b'shut':
            item = item[:fixed - 4] + b'\x81\0\0\0\x07Log Out'
        out.append(item)
    return r[:2] + struct.pack('>H', len(out) + 1) + r[4:h] + b''.join(out)


def logout_strings(r):
    count, = struct.unpack('>H', r[:2])
    p, out = 2, []
    for i in range(1, count + 1):
        if p >= len(r) or p + 1 + r[p] > len(r):
            raise ValueError('truncated Finder strings')
        text = r[p + 1:p + 1 + r[p]]
        p += 1 + r[p]
        if i in (57, 58):
            suffix = b' ' if i == 58 else b''
            if text not in (b'Shut Down' + suffix, b'Log Out' + suffix):
                raise ValueError('unknown shutdown label')
            text = b'Log Out' + suffix
        out.append(bytes([len(text)]) + text)
    if count < 58 or p != len(r):
        raise ValueError('unknown Finder string table')
    return r[:2] + b''.join(out)


def userfinder(d):
    try:
        rsrcedit.resource(d, b'fmnu', 1255)
        menus = [(b'fmnu', 1255, 12, 8)]
    except ValueError:
        menus = [(b'fmn2', 524, 16, 12), (b'fmn2', 530, 16, 12)]
    replacements = []
    for typ, rid, title, item in menus:
        o, l, _ = rsrcedit.resource(d, typ, rid)
        r = bytes(d[o:o + l])
        changed = logout_menu(r, title, item)
        replacements.append((typ, rid, changed))
    if menus[0][0] == b'fmn2':
        o, l, _ = rsrcedit.resource(d, b'STR#', 500)
        replacements.append((b'STR#', 500, logout_strings(bytes(d[o:o + l]))))
    for typ, rid, r in replacements:
        rsrcedit.replace(d, typ, rid, r)


if __name__ == '__main__':
    f = sys.argv[1]
    d = bytearray(open(f, 'rb').read())
    userfinder(d)
    open(f, 'wb').write(d)
