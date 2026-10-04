package sources

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"path"
	"sort"
	"strings"

	"amigaux.org/imagebuilder/ufs"
)

var hbe = binary.BigEndian

type hExtent struct{ start, count uint16 }
type hKey struct {
	id    uint32
	fork  byte
	start uint16
}
type hVolume struct {
	ctx               context.Context
	r                 io.ReaderAt
	size, base, block int64
	count             uint32
	limit             Limits
	overflow          map[hKey][]hExtent
	used              map[uint16]bool
	bitmap            []byte
}

func hExt(b []byte) []hExtent {
	out := []hExtent{}
	for i := 0; i < 12; i += 4 {
		out = append(out, hExtent{hbe.Uint16(b[i:]), hbe.Uint16(b[i+2:])})
	}
	return out
}
func (v *hVolume) fork(id uint32, kind byte, n uint32, ext []hExtent) ([]byte, error) {
	if int64(n) > v.limit.ExpandedBytes {
		return nil, fmt.Errorf("HFS fork exceeds limit")
	}
	result := make([]byte, 0, int(n))
	var logical uint32
	empty := false
	for {
		for _, e := range ext {
			if e.count == 0 {
				empty = true
				continue
			}
			if empty {
				return nil, fmt.Errorf("HFS nonempty extent after terminator")
			}
			if uint32(e.start)+uint32(e.count) > v.count {
				return nil, fmt.Errorf("HFS extent outside allocation area")
			}
			for b := uint32(e.start); b < uint32(e.start)+uint32(e.count); b++ {
				if v.bitmap[b/8]&(0x80>>(b%8)) == 0 {
					return nil, fmt.Errorf("HFS extent references free allocation")
				}
				if v.used[uint16(b)] {
					return nil, fmt.Errorf("HFS overlapping allocation")
				}
				v.used[uint16(b)] = true
			}
			take := int64(e.count) * v.block
			if take > int64(n)-int64(len(result)) {
				take = int64(n) - int64(len(result))
			}
			if take > 0 {
				if err := v.ctx.Err(); err != nil {
					return nil, err
				}
				b, err := readAt(v.r, v.base+int64(e.start)*v.block, take, v.size)
				if err != nil {
					return nil, err
				}
				result = append(result, b...)
			}
			logical += uint32(e.count)
		}
		if len(result) == int(n) {
			return result, nil
		}
		if logical > 65535 {
			return nil, fmt.Errorf("HFS extent overflow")
		}
		next, ok := v.overflow[hKey{id, kind, uint16(logical)}]
		if !ok || empty {
			return nil, fmt.Errorf("HFS missing overflow extent")
		}
		ext = next
	}
}
func (v *hVolume) overflowTree(n uint32, ext []hExtent) ([]byte, error) {
	if int64(n) > v.limit.ExpandedBytes {
		return nil, fmt.Errorf("HFS overflow tree exceeds limit")
	}
	var blocks uint32
	for _, e := range ext {
		blocks += uint32(e.count)
	}
	resident := int64(blocks) * v.block
	if resident > int64(n) {
		resident = int64(n)
	}
	tree, err := v.fork(3, 0, uint32(resident), ext)
	if err != nil {
		return nil, err
	}
	for len(tree) < int(n) {
		candidates := map[uint16][]hExtent{}
		for off := 512; off+512 <= len(tree); off += 512 {
			node := tree[off : off+512]
			if node[8] != 255 {
				continue
			}
			records, err := hRecords(node)
			if err != nil {
				return nil, err
			}
			for _, r := range records {
				if len(r) < 20 || r[0] != 7 {
					return nil, fmt.Errorf("invalid overflow bootstrap record")
				}
				if r[1] == 0 && hbe.Uint32(r[2:]) == 3 {
					key := hbe.Uint16(r[6:])
					if _, ok := candidates[key]; ok {
						return nil, fmt.Errorf("duplicate bootstrap extent")
					}
					candidates[key] = hExt(r[8:20])
				}
			}
		}
		if blocks > 65535 {
			return nil, fmt.Errorf("overflow bootstrap bounds")
		}
		next, ok := candidates[uint16(blocks)]
		if !ok {
			return nil, fmt.Errorf("overflow tree bootstrap extent unavailable")
		}
		var count uint32
		for _, e := range next {
			count += uint32(e.count)
		}
		if count == 0 {
			return nil, fmt.Errorf("empty overflow bootstrap extent")
		}
		take := int64(count) * v.block
		if take > int64(n)-int64(len(tree)) {
			take = int64(n) - int64(len(tree))
		}
		more, err := v.fork(3, 0, uint32(take), next)
		if err != nil {
			return nil, err
		}
		tree = append(tree, more...)
		blocks += count
	}
	return tree, nil
}

func hRecords(node []byte) ([][]byte, error) {
	if len(node) < 512 {
		return nil, fmt.Errorf("short HFS node")
	}
	count := int(hbe.Uint16(node[10:]))
	if count > (len(node)-16)/4 {
		return nil, fmt.Errorf("HFS node record count")
	}
	out := make([][]byte, 0, count)
	prev := 14
	for i := 0; i <= count; i++ {
		off := int(hbe.Uint16(node[len(node)-2*(i+1):]))
		if off < prev || off > len(node)-2*(count+1) {
			return nil, fmt.Errorf("HFS node record bounds")
		}
		if i > 0 {
			out = append(out, node[prev:off])
		}
		prev = off
	}
	return out, nil
}
func hLeaves(ctx context.Context, tree []byte, limit int) ([][]byte, error) {
	if len(tree) < 512 || tree[8] != 1 {
		return nil, fmt.Errorf("invalid HFS B-tree header")
	}
	ns := int(hbe.Uint16(tree[32:]))
	if ns != 512 {
		return nil, fmt.Errorf("unsupported HFS B-tree node size")
	}
	rs, e := hRecords(tree[:ns])
	if e != nil || len(rs) != 3 || len(rs[0]) < 30 {
		return nil, fmt.Errorf("invalid HFS header records")
	}
	h := rs[0]
	total := hbe.Uint32(h[22:])
	if uint64(total)*uint64(ns) > uint64(len(tree)) {
		return nil, fmt.Errorf("truncated HFS B-tree")
	}
	first, last := hbe.Uint32(h[10:]), hbe.Uint32(h[14:])
	expected := hbe.Uint32(h[6:])
	out := [][]byte{}
	seen := map[uint32]bool{}
	var previous uint32
	for id := first; id != 0; {
		if e := ctx.Err(); e != nil {
			return nil, e
		}
		if id >= total || seen[id] {
			return nil, fmt.Errorf("HFS leaf cycle or bounds")
		}
		seen[id] = true
		n := tree[int(id)*ns : int(id+1)*ns]
		if n[8] != 255 || n[9] != 1 || hbe.Uint32(n[4:]) != previous {
			return nil, fmt.Errorf("HFS invalid leaf links")
		}
		records, e := hRecords(n)
		if e != nil {
			return nil, e
		}
		if len(records) > limit-len(out) {
			return nil, fmt.Errorf("HFS record limit")
		}
		out = append(out, records...)
		previous = id
		id = hbe.Uint32(n)
	}
	if previous != last || len(out) != int(expected) {
		return nil, fmt.Errorf("HFS leaf count mismatch")
	}
	return out, nil
}
func readHFS(ctx context.Context, r io.ReaderAt, size int64, l Limits) (Result, error) {
	bad := func(e error) (Result, error) { return Result{}, e }
	start := int64(0)
	length := size
	b, e := readAt(r, 0, 512, size)
	if e != nil {
		return bad(e)
	}
	if hbe.Uint16(b) == 0x4552 {
		bs := int64(hbe.Uint16(b[2:]))
		if bs != 512 {
			return bad(fmt.Errorf("unsupported APM block size"))
		}
		first, e := readAt(r, 512, 512, size)
		if e != nil {
			return bad(e)
		}
		count := hbe.Uint32(first[4:])
		if count == 0 || count > 4096 {
			return bad(fmt.Errorf("APM partition count"))
		}
		found := false
		for i := uint32(1); i <= count; i++ {
			p, e := readAt(r, int64(i)*512, 512, size)
			if e != nil {
				return bad(e)
			}
			if hbe.Uint16(p) != 0x504d || hbe.Uint32(p[4:]) != count {
				return bad(fmt.Errorf("invalid APM entry"))
			}
			if strings.TrimRight(string(p[48:80]), "\x00") == "Apple_HFS" {
				if found {
					return bad(fmt.Errorf("ambiguous HFS partitions"))
				}
				start = int64(hbe.Uint32(p[8:])) * 512
				length = int64(hbe.Uint32(p[12:])) * 512
				if start < 512*int64(count+1) || length < 1536 || start > size-length {
					return bad(fmt.Errorf("HFS partition bounds"))
				}
				found = true
			}
		}
		if !found {
			return bad(fmt.Errorf("no HFS partition"))
		}
	}
	volume := io.NewSectionReader(r, start, length)
	mdb, e := readAt(volume, 1024, 512, length)
	if e != nil {
		return bad(e)
	}
	if hbe.Uint16(mdb) != 0x4244 {
		return bad(fmt.Errorf("classic HFS volume required"))
	}
	block := int64(hbe.Uint32(mdb[20:]))
	count := uint32(hbe.Uint16(mdb[18:]))
	base := int64(hbe.Uint16(mdb[28:])) * 512
	if block < 512 || block%512 != 0 || count == 0 || base < 1536 || base+int64(count)*block > length-1024 {
		return bad(fmt.Errorf("invalid HFS geometry"))
	}
	bitmapStart := int64(hbe.Uint16(mdb[14:])) * 512
	bitmapLength := (int64(count) + 7) / 8
	if bitmapStart < 1536 || bitmapStart+bitmapLength > base {
		return bad(fmt.Errorf("HFS bitmap bounds"))
	}
	bitmap, e := readAt(volume, bitmapStart, bitmapLength, length)
	if e != nil {
		return bad(e)
	}
	v := hVolume{ctx: ctx, r: volume, size: length, base: base, block: block, count: count, limit: l, overflow: map[hKey][]hExtent{}, used: map[uint16]bool{}, bitmap: bitmap}
	// Bootstrap the overflow tree from its resident allocation extents.
	xtSize := hbe.Uint32(mdb[130:])
	xt, e := v.overflowTree(xtSize, hExt(mdb[134:146]))
	if e != nil {
		return bad(fmt.Errorf("HFS overflow tree: %w", e))
	}
	records, e := hLeaves(ctx, xt, l.Entries)
	if e != nil {
		return bad(e)
	}
	for _, record := range records {
		if len(record) < 20 || record[0] != 7 {
			return bad(fmt.Errorf("invalid HFS extent key"))
		}
		key := hKey{hbe.Uint32(record[2:]), record[1], hbe.Uint16(record[6:])}
		if key.fork != 0 && key.fork != 255 {
			return bad(fmt.Errorf("invalid HFS fork kind"))
		}
		if _, ok := v.overflow[key]; ok {
			return bad(fmt.Errorf("duplicate HFS overflow key"))
		}
		v.overflow[key] = hExt(record[8:20])
	}
	cat, e := v.fork(4, 0, hbe.Uint32(mdb[146:]), hExt(mdb[150:162]))
	if e != nil {
		return bad(e)
	}
	records, e = hLeaves(ctx, cat, l.Entries*2+1)
	if e != nil {
		return bad(e)
	}
	type catalog struct {
		id, parent uint32
		name       string
		data       []byte
		dir        bool
	}
	items := map[uint32]catalog{}
	for _, record := range records {
		if len(record) < 8 {
			return bad(fmt.Errorf("short HFS catalog key"))
		}
		kl := int(record[0])
		nameLen := int(record[6])
		off := (kl + 2) &^ 1
		if kl < 6 || kl > 37 || nameLen > 31 || 7+nameLen > kl+1 || off+2 > len(record) {
			return bad(fmt.Errorf("invalid HFS catalog key"))
		}
		data := record[off:]
		kind := hbe.Uint16(data)
		if kind == 0x300 || kind == 0x400 {
			continue
		}
		isdir := kind == 0x100
		if !isdir && kind != 0x200 {
			return bad(fmt.Errorf("unknown HFS catalog record"))
		}
		needed := 102
		if isdir {
			needed = 70
		}
		if len(data) < needed {
			return bad(fmt.Errorf("short HFS catalog record"))
		}
		id := hbe.Uint32(data[20:])
		if isdir {
			id = hbe.Uint32(data[6:])
		}
		if _, ok := items[id]; ok || id < 2 {
			return bad(fmt.Errorf("duplicate HFS catalog ID"))
		}
		name := string(record[7 : 7+nameLen])
		if name == "" || strings.ContainsAny(name, "/:\x00\\\r\n\t") || name == "." || name == ".." {
			return bad(fmt.Errorf("unsafe HFS name"))
		}
		items[id] = catalog{id, hbe.Uint32(record[2:]), name, data, isdir}
		if len(items) > l.Entries {
			return bad(fmt.Errorf("HFS entry limit"))
		}
	}
	root, ok := items[2]
	if !ok || !root.dir || root.parent != 1 {
		return bad(fmt.Errorf("missing HFS root"))
	}
	paths := map[uint32]string{2: "/"}
	var resolve func(uint32, map[uint32]bool) (string, error)
	resolve = func(id uint32, seen map[uint32]bool) (string, error) {
		if p, ok := paths[id]; ok {
			return p, nil
		}
		x, ok := items[id]
		if !ok || seen[id] || len(seen) > 1024 {
			return "", fmt.Errorf("HFS missing parent or directory cycle")
		}
		parent, ok := items[x.parent]
		if !ok || !parent.dir {
			return "", fmt.Errorf("HFS parent is not directory")
		}
		seen[id] = true
		p, e := resolve(x.parent, seen)
		if e != nil {
			return "", e
		}
		p = path.Join(p, x.name)
		paths[id] = p
		return p, nil
	}
	out := Result{}
	var expanded int64
	add := func(x ufs.Entry) error {
		if len(out.Entries) >= l.Entries || x.Size > l.ExpandedBytes-expanded {
			return fmt.Errorf("HFS expansion limit")
		}
		expanded += x.Size
		out.Entries = append(out.Entries, x)
		return nil
	}
	ids := make([]uint32, 0, len(items))
	for id := range items {
		ids = append(ids, id)
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	for _, id := range ids {
		if id == 2 {
			continue
		}
		x := items[id]
		p, e := resolve(id, map[uint32]bool{})
		if e != nil {
			return bad(e)
		}
		var finder [32]byte
		var resource []byte
		if x.dir {
			copy(finder[:16], x.data[22:38])
			copy(finder[16:], x.data[38:54])
			if e := add(ufs.Entry{Path: p, Kind: 'd', Mode: 0755}); e != nil {
				return bad(e)
			}
		} else {
			dn, rn := hbe.Uint32(x.data[26:]), hbe.Uint32(x.data[36:])
			if int64(dn)+int64(rn)+512 > l.ExpandedBytes-expanded {
				return bad(fmt.Errorf("HFS expansion limit"))
			}
			data, e := v.fork(id, 0, dn, hExt(x.data[74:86]))
			if e != nil {
				return bad(e)
			}
			resource, e = v.fork(id, 255, rn, hExt(x.data[86:98]))
			if e != nil {
				return bad(e)
			}
			copy(finder[:16], x.data[4:20])
			copy(finder[16:], x.data[56:72])
			f := file(p, data)
			tm := hbe.Uint32(x.data[48:])
			if tm >= 2082844800 {
				tm -= 2082844800
			} else {
				tm = 0
			}
			f.ModTime = &tm
			if e := add(f); e != nil {
				return bad(e)
			}
		}
		ad := make([]byte, 512+len(resource))
		hbe.PutUint32(ad, 0x00051607)
		hbe.PutUint32(ad[4:], 0x00010000)
		copy(ad[8:24], "Macintosh       ")
		hbe.PutUint16(ad[24:], 2)
		hbe.PutUint32(ad[26:], 9)
		hbe.PutUint32(ad[30:], 0xe0)
		hbe.PutUint32(ad[34:], 32)
		hbe.PutUint32(ad[38:], 2)
		hbe.PutUint32(ad[42:], 0x200)
		hbe.PutUint32(ad[46:], uint32(len(resource)))
		copy(ad[0xe0:], finder[:])
		copy(ad[512:], resource)
		if e := add(file(path.Join(path.Dir(p), "%"+path.Base(p)), ad)); e != nil {
			return bad(e)
		}
	}
	return out, nil
}
