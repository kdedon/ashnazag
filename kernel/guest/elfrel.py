# elfrel.py -- minimal ELF32 big-endian relocatable reader shared by the
# gate tools: sections, symbols, .rela.text entries.
import struct


class Elf:
    def __init__(self, path):
        self.path = path
        self.b = bytearray(open(path, 'rb').read())
        b = self.b
        if b[:4] != b'\x7fELF' or b[4] != 1 or b[5] != 2:
            raise SystemExit('ABORT: %s is not ELF32 MSB' % path)
        shoff, shent, shnum, shstr = self.u32(32), self.u16(46), self.u16(48), self.u16(50)
        self.sh = []
        for i in range(shnum):
            o = shoff + i * shent
            self.sh.append(dict(name=self.u32(o), type=self.u32(o + 4), addr=self.u32(o + 12),
                                offset=self.u32(o + 16), size=self.u32(o + 20),
                                link=self.u32(o + 24), info=self.u32(o + 28),
                                entsize=self.u32(o + 36)))
        so = self.sh[shstr]['offset']
        for s in self.sh:
            s['nm'] = self.cstr(so + s['name'])
        self.byname = {s['nm']: s for s in self.sh}
        st = self.byname['.symtab']
        self.symoff, self.nsym = st['offset'], st['size'] // 16
        self.stroff = self.sh[st['link']]['offset']
        self.syms = []
        for i in range(self.nsym):
            o = self.symoff + 16 * i
            self.syms.append(dict(name=self.cstr(self.stroff + self.u32(o)), value=self.u32(o + 4),
                                  size=self.u32(o + 8), info=b[o + 12], shndx=self.u16(o + 14)))

    def u16(self, o):
        return struct.unpack('>H', self.b[o:o + 2])[0]

    def u32(self, o):
        return struct.unpack('>I', self.b[o:o + 4])[0]

    def cstr(self, o):
        return self.b[o:self.b.index(b'\0', o)].decode('latin1')

    def sym(self, name, defined=True):
        hits = [i for i, s in enumerate(self.syms) if s['name'] == name and
                (not defined or s['shndx'] != 0) and (s['info'] >> 4) != 0]
        if not hits:
            raise SystemExit('ABORT: no global symbol %s in %s' % (name, self.path))
        return hits[0]

    def text_relocs(self):
        """{offset: (entry file offset, symbol index, type, addend)}"""
        r = self.byname['.rela.text']
        out = {}
        for i in range(r['size'] // 12):
            o = r['offset'] + 12 * i
            info = self.u32(o + 4)
            out[self.u32(o)] = (o, info >> 8, info & 0xff, self.u32(o + 8))
        return out

    def write(self):
        open(self.path, 'wb').write(self.b)
