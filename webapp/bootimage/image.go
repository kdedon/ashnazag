// Package bootimage builds a Quadra HFS boot disk from an ELF and donor driver.
package bootimage

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"io"
)

// The embedded boot code runs before the kernel takes control.
var bootBlocks = decodeBootBlocks()

func decodeBootBlocks() []byte {
	b, err := base64.StdEncoding.DecodeString(bootBlocksBase64)
	if err != nil {
		panic(err)
	}
	return b
}

var be = binary.BigEndian

type Options struct{ CommandLine string }
type Kernel struct {
	Data                  []byte
	Load, End, Entry, Sum uint32
}

func Flatten(e []byte) (Kernel, error) {
	fail := func() (Kernel, error) { return Kernel{}, fmt.Errorf("bootimage: invalid big-endian m68k executable") }
	if len(e) < 52 || string(e[:4]) != "\x7fELF" || e[4] != 1 || e[5] != 2 || be.Uint16(e[16:]) != 2 || be.Uint16(e[18:]) != 4 {
		return fail()
	}
	po, ps, pn := uint64(be.Uint32(e[28:])), uint64(be.Uint16(e[42:])), uint64(be.Uint16(e[44:]))
	if ps < 32 || po+ps*pn > uint64(len(e)) {
		return fail()
	}
	k := Kernel{Load: ^uint32(0), Entry: be.Uint32(e[24:])}
	var hi uint64
	var spans [][2]uint64
	entryLoaded := false
	for i := uint64(0); i < pn; i++ {
		p := e[po+i*ps:]
		if be.Uint32(p) != 1 || be.Uint32(p[20:]) == 0 {
			continue
		}
		a, f, m, o := uint64(be.Uint32(p[8:])), uint64(be.Uint32(p[16:])), uint64(be.Uint32(p[20:])), uint64(be.Uint32(p[4:]))
		if f > m || a != uint64(be.Uint32(p[12:])) || a+m > 0xffffffff || o+f > uint64(len(e)) {
			return fail()
		}
		for _, span := range spans {
			if a < span[1] && span[0] < a+m {
				return fail()
			}
		}
		spans = append(spans, [2]uint64{a, a + m})
		if uint64(k.Entry) >= a && uint64(k.Entry) < a+f {
			entryLoaded = true
		}
		k.Load = min(k.Load, uint32(a))
		hi = max(hi, a+f)
		k.End = max(k.End, uint32(a+m))
	}
	if !entryLoaded || hi <= uint64(k.Load) || hi-uint64(k.Load) > 30<<20 || k.Entry < k.Load || uint64(k.Entry) >= hi {
		return fail()
	}
	k.Data = make([]byte, (hi-uint64(k.Load)+511)/512*512)
	for i := uint64(0); i < pn; i++ {
		p := e[po+i*ps:]
		if be.Uint32(p) != 1 || be.Uint32(p[20:]) == 0 {
			continue
		}
		a, f, o := be.Uint32(p[8:]), be.Uint32(p[16:]), be.Uint32(p[4:])
		if f > 0 {
			copy(k.Data[a-k.Load:], e[o:uint64(o)+uint64(f)])
		}
	}
	for i := 0; i < len(k.Data); i += 4 {
		k.Sum += be.Uint32(k.Data[i:])
	}
	return k, nil
}

func Build(ctx context.Context, donor io.ReaderAt, donorSize int64, elf []byte, opts Options) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if len(opts.CommandLine) > 127 || bytes.IndexByte([]byte(opts.CommandLine), 0) >= 0 {
		return nil, fmt.Errorf("bootimage: command line must be at most 127 bytes without NUL")
	}
	k, err := Flatten(elf)
	if err != nil {
		return nil, err
	}
	if donorSize < 32768 {
		return nil, fmt.Errorf("bootimage: donor is too small")
	}
	head := make([]byte, 32768)
	if _, err = donor.ReadAt(head, 0); err != nil {
		return nil, err
	}
	if string(head[:2]) != "ER" || be.Uint16(head[2:]) != 512 {
		return nil, fmt.Errorf("bootimage: donor requires Apple 512-byte disk map")
	}
	n := int(be.Uint32(head[516:]))
	if n < 3 || n > 63 {
		return nil, fmt.Errorf("bootimage: invalid donor map size")
	}
	var mp, drv, hfs []byte
	for i := 1; i <= n; i++ {
		e := head[i*512 : (i+1)*512]
		if string(e[:2]) != "PM" || int(be.Uint32(e[4:])) != n {
			return nil, fmt.Errorf("bootimage: inconsistent donor map")
		}
		st, cnt := uint64(be.Uint32(e[8:])), uint64(be.Uint32(e[12:]))
		if cnt == 0 || (st+cnt)*512 > uint64(donorSize) {
			return nil, fmt.Errorf("bootimage: donor partition outside file")
		}
		typ := string(bytes.TrimRight(e[48:80], "\x00"))
		switch typ {
		case "Apple_partition_map":
			if mp != nil {
				return nil, fmt.Errorf("bootimage: duplicate map")
			}
			mp = e
		case "Apple_Driver":
			if drv != nil {
				return nil, fmt.Errorf("bootimage: duplicate driver")
			}
			drv = e
		case "Apple_HFS":
			if hfs != nil {
				return nil, fmt.Errorf("bootimage: duplicate HFS")
			}
			hfs = e
		}
	}
	if mp == nil || drv == nil || hfs == nil || be.Uint32(mp[8:]) != 1 || be.Uint32(mp[12:]) != 63 || be.Uint32(drv[8:]) != 64 || be.Uint32(drv[12:]) > 4096 {
		return nil, fmt.Errorf("bootimage: requires map at block 1 and driver at block 64")
	}
	dc := int(be.Uint32(drv[12:]))
	nd := int(be.Uint16(head[16:]))
	if nd < 1 || nd > 61 {
		return nil, fmt.Errorf("bootimage: invalid driver descriptors")
	}
	for i := 0; i < nd; i++ {
		d := head[18+i*8:]
		a, c := uint64(be.Uint32(d)), uint64(be.Uint16(d[4:]))
		if c == 0 || a < 64 || a+c > uint64(64+dc) {
			return nil, fmt.Errorf("bootimage: descriptor outside driver")
		}
	}
	vol := volume(k, opts.CommandLine)
	start := (64 + dc) * 512
	out := make([]byte, start+len(vol))
	copy(out, head[:512])
	be.PutUint32(out[4:], uint32(len(out)/512))
	for i, e := range [][]byte{mp, drv, hfs} {
		d := out[(i+1)*512 : (i+2)*512]
		copy(d, e)
		be.PutUint32(d[4:], 3)
		if i == 2 {
			be.PutUint32(d[8:], uint32(start/512))
			be.PutUint32(d[12:], uint32(len(vol)/512))
			be.PutUint32(d[80:], 0)
			be.PutUint32(d[84:], uint32(len(vol)/512))
		}
	}
	if _, err = donor.ReadAt(out[32768:start], 32768); err != nil {
		return nil, err
	}
	copy(out[start:], vol)
	if err = ctx.Err(); err != nil {
		return nil, err
	}
	return out, nil
}

func volume(k Kernel, cmd string) []byte {
	// Fixed geometry keeps the single contiguous extent within HFS's 16-bit limits.
	size := max(4<<20, (len(k.Data)+(2<<20)-1)/(1<<20)*(1<<20))
	const absize = 1024
	const abstart = 19
	nab := (size/512 - abstart - 2) / 2
	const trees = 4
	const fileBlock = trees * 2
	v := make([]byte, size)
	m := v[1024:1536]
	u16 := func(o int, n uint16) { be.PutUint16(m[o:], n) }
	u32 := func(o int, n uint32) { be.PutUint32(m[o:], n) }
	blocks := (len(k.Data) + absize - 1) / absize
	u16(0, 0x4244)
	u32(2, 0xe0000000)
	u32(6, 0xe0000000)
	u16(10, 0x100)
	u16(12, 1)
	u16(14, 3)
	u16(16, uint16(fileBlock+blocks))
	u16(18, uint16(nab))
	u32(20, absize)
	u32(24, absize*4)
	u16(28, abstart)
	u32(30, 17)
	u16(34, uint16(nab-fileBlock-blocks))
	copy(m[36:], []byte{4, 'U', 'n', 'i', 'x'})
	u32(70, 1)
	u32(74, absize*trees)
	u32(78, absize*trees)
	u32(84, 1)
	u32(130, absize*trees)
	u16(134, 0)
	u16(136, trees)
	u32(146, absize*trees)
	u16(150, trees)
	u16(152, trees)
	for i := 0; i < fileBlock+blocks; i++ {
		v[1536+i/8] |= 0x80 >> uint(i%8)
	}
	tree := func(start int, cat bool) {
		n := v[start : start+512]
		n[8] = 1
		be.PutUint16(n[10:], 3)
		for i, o := range []uint16{14, 120, 248, 504} {
			be.PutUint16(n[510-i*2:], o)
		}
		h := n[14:]
		be.PutUint16(h[18:], 512)
		be.PutUint16(h[20:], 7)
		be.PutUint32(h[22:], 8)
		be.PutUint32(h[26:], 7)
		n[248] = 0x80
		if cat {
			be.PutUint16(h, 1)
			be.PutUint32(h[2:], 1)
			be.PutUint32(h[6:], 3)
			be.PutUint32(h[10:], 1)
			be.PutUint32(h[14:], 1)
			be.PutUint16(h[20:], 37)
			be.PutUint32(h[26:], 6)
			n[248] = 0xc0
		}
	}
	base := abstart * 512
	tree(base, false)
	tree(base+trees*absize, true)
	n := v[base+trees*absize+512 : base+trees*absize+1024]
	n[8] = 0xff
	n[9] = 1
	be.PutUint16(n[10:], 3)
	for i, o := range []uint16{14, 96, 150, 264} {
		be.PutUint16(n[510-i*2:], o)
	}
	key := func(o int, parent uint32, name string) {
		n[o] = byte(7 + len(name))
		be.PutUint32(n[o+2:], parent)
		n[o+6] = byte(len(name))
		copy(n[o+7:], name)
	}
	key(14, 1, "Unix")
	d := n[26:96]
	d[0] = 1
	be.PutUint16(d[4:], 1)
	be.PutUint32(d[6:], 2)
	be.PutUint32(d[10:], 0xe0000000)
	be.PutUint32(d[14:], 0xe0000000)
	key(96, 2, "")
	d = n[104:150]
	d[0] = 3
	be.PutUint32(d[10:], 1)
	copy(d[14:], []byte{4, 'U', 'n', 'i', 'x'})
	key(150, 2, "unix")
	d = n[162:264]
	d[0] = 2
	copy(d[4:], "????UNIX")
	be.PutUint32(d[20:], 16)
	be.PutUint16(d[24:], fileBlock)
	be.PutUint32(d[26:], uint32(len(k.Data)))
	be.PutUint32(d[30:], uint32(blocks*absize))
	be.PutUint32(d[44:], 0xe0000000)
	be.PutUint32(d[48:], 0xe0000000)
	be.PutUint16(d[74:], fileBlock)
	be.PutUint16(d[76:], uint16(blocks))
	off := base + fileBlock*absize
	copy(v[off:], k.Data)
	copy(v[size-1024:], m)
	copy(v, bootBlocks)
	for i, x := range []uint32{uint32(off), uint32(len(k.Data)), k.Load, k.End, k.Entry, k.Sum} {
		be.PutUint32(v[0x90+i*4:], x)
	}
	clear(v[896:1024])
	copy(v[896:1024], cmd)
	return v
}
