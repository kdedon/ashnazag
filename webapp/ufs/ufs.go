// Package ufs writes big-endian SVR4 UFS filesystems.
package ufs

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"path"
	"sort"
	"strings"
)

type Options struct {
	SizeMiB, InodesPerGroup int
	Timestamp               uint32
	MountPoint              string
}
type Entry struct {
	Path           string
	Kind           byte
	Mode, UID, GID uint32
	Size           int64
	Data           io.ReaderAt
	Target         string
	Major, Minor   uint32
	ModTime        *uint32
}
type Report struct {
	Bytes                                          int64
	Inodes, Directories, FreeBlocks, FreeFragments int
}
type inode struct {
	e      Entry
	links  int
	db     [12]uint32
	ib     [3]uint32
	blocks uint32
}
type fs struct {
	ctx                                                    context.Context
	dst                                                    io.WriterAt
	opt                                                    Options
	size, ncg, ipg, dblk, cssize, dsize, next, part, partk int
	free                                                   []byte
	nodes                                                  map[int]*inode
	dirs                                                   []int
}

func p32(b []byte, o int, v int) { binary.BigEndian.PutUint32(b[o:], uint32(v)) }
func p16(b []byte, o int, v int) { binary.BigEndian.PutUint16(b[o:], uint16(v)) }
func start(c int) int            { return 4096*c + 16*(c&15) }
func (s *fs) write(b []byte, off int64) error {
	if err := s.ctx.Err(); err != nil {
		return err
	}
	n, err := s.dst.WriteAt(b, off)
	if err == nil && n != len(b) {
		err = io.ErrShortWrite
	}
	return err
}
func (s *fs) put(f int, b []byte) error { return s.write(b, int64(f)*1024) }

// Build writes allocated data and metadata. dst must initially contain SizeMiB
// MiB of zeroes; a newly truncated sparse file satisfies this requirement.
func Build(ctx context.Context, dst io.WriterAt, opt Options, entries []Entry) (Report, error) {
	if opt.SizeMiB < 4 || opt.SizeMiB%4 != 0 || opt.SizeMiB > 2048 {
		return Report{}, fmt.Errorf("size must be a multiple of 4 MiB between 4 and 2048")
	}
	if opt.InodesPerGroup == 0 {
		opt.InodesPerGroup = 960
	}
	if opt.InodesPerGroup < 64 || opt.InodesPerGroup > 2048 || opt.InodesPerGroup%64 != 0 {
		return Report{}, fmt.Errorf("inodes per group must be a multiple of 64 up to 2048")
	}
	if opt.MountPoint == "" {
		opt.MountPoint = "/"
	}
	if len(opt.MountPoint) > 511 || strings.ContainsRune(opt.MountPoint, 0) {
		return Report{}, fmt.Errorf("invalid mount point")
	}
	s := &fs{ctx: ctx, dst: dst, opt: opt, size: opt.SizeMiB * 1024, ncg: opt.SizeMiB / 4, ipg: opt.InodesPerGroup, dblk: 32 + opt.InodesPerGroup/8, part: -1, nodes: map[int]*inode{}}
	s.cssize = (s.ncg*16 + 1023) / 1024 * 1024
	s.free = make([]byte, s.size)
	s.dirs = make([]int, s.ncg)
	for c := 0; c < s.ncg; c++ {
		base, lo, hi, end := c*4096, start(c)+16, start(c)+s.dblk, (c+1)*4096
		if c == 0 {
			hi += s.cssize / 1024
		} else {
			for j := base; j < lo; j++ {
				s.free[j] = 1
			}
			s.dsize += lo - base
		}
		for j := hi; j < end; j++ {
			s.free[j] = 1
		}
		s.dsize += end - hi
	}
	if err := s.build(entries); err != nil {
		return Report{}, err
	}
	return s.finish()
}
func (s *fs) alloc(n int) (int, error) {
	if n < 8 && s.part >= 0 && s.partk+n <= 8 {
		f := s.part + s.partk
		for j := f; j < f+n; j++ {
			s.free[j] = 0
		}
		s.partk += n
		return f, nil
	}
	for f := s.next; f < s.size; f += 8 {
		ok := true
		for j := f; j < f+8; j++ {
			if s.free[j] == 0 {
				ok = false
				break
			}
		}
		if !ok {
			continue
		}
		s.next = f + 8
		for j := f; j < f+n; j++ {
			s.free[j] = 0
		}
		if n < 8 {
			s.part = f
			s.partk = n
		}
		return f, nil
	}
	return 0, fmt.Errorf("filesystem out of space")
}
func (s *fs) layout(i *inode, r io.ReaderAt, size int64) error {
	nb := (size + 8191) / 8192
	var extra []uint32
	buf := make([]byte, 8192)
	for l := int64(0); l < nb; l++ {
		if err := s.ctx.Err(); err != nil {
			return err
		}
		n := 8
		if l == nb-1 && nb <= 12 && size%8192 != 0 {
			n = int((size%8192 + 1023) / 1024)
		}
		f, err := s.alloc(n)
		if err != nil {
			return err
		}
		count := int64(8192)
		if size-l*8192 < count {
			count = size - l*8192
		}
		read, err := r.ReadAt(buf[:count], l*8192)
		if read != int(count) {
			return fmt.Errorf("read %s: %w", i.e.Path, io.ErrUnexpectedEOF)
		}
		if err != nil && err != io.EOF {
			return err
		}
		if err = s.put(f, buf[:count]); err != nil {
			return err
		}
		i.blocks += uint32(n * 2)
		if l < 12 {
			i.db[l] = uint32(f)
		} else {
			if l == 12 {
				f, err := s.alloc(8)
				if err != nil {
					return err
				}
				i.ib[0] = uint32(f)
				i.blocks += 16
			}
			extra = append(extra, uint32(f))
		}
	}
	writePtrs := func(f uint32, a []uint32) error {
		b := make([]byte, len(a)*4)
		for j, v := range a {
			p32(b, j*4, int(v))
		}
		return s.put(int(f), b)
	}
	if len(extra) > 0 {
		n := len(extra)
		if n > 2048 {
			n = 2048
		}
		if err := writePtrs(i.ib[0], extra[:n]); err != nil {
			return err
		}
		extra = extra[n:]
	}
	if len(extra) > 0 {
		f, err := s.alloc(8)
		if err != nil {
			return err
		}
		i.ib[1] = uint32(f)
		i.blocks += 16
		var level []uint32
		for len(extra) > 0 {
			f, err := s.alloc(8)
			if err != nil {
				return err
			}
			i.blocks += 16
			level = append(level, uint32(f))
			n := len(extra)
			if n > 2048 {
				n = 2048
			}
			if err = writePtrs(uint32(f), extra[:n]); err != nil {
				return err
			}
			extra = extra[n:]
		}
		return writePtrs(i.ib[1], level)
	}
	return nil
}
func validPath(p string) bool {
	if p == "" || p[0] != '/' || path.Clean(p) != p || strings.ContainsRune(p, 0) {
		return false
	}
	for _, n := range strings.Split(p, "/") {
		if len(n) > 255 {
			return false
		}
	}
	return true
}
func (s *fs) build(entries []Entry) error {
	tree := map[string]Entry{"/": {Path: "/", Kind: 'd', Mode: 0755}, "/lost+found": {Path: "/lost+found", Kind: 'd', Mode: 0755}}
	seen := map[string]bool{}
	for _, e := range entries {
		if !validPath(e.Path) || seen[e.Path] {
			return fmt.Errorf("invalid or duplicate path %q", e.Path)
		}
		seen[e.Path] = true
		if !strings.ContainsRune("dflhcbp", rune(e.Kind)) || e.Mode > 07777 || e.Size < 0 || e.Size > 2147483647 || e.Major > 16383 || e.Minor > 262143 {
			return fmt.Errorf("invalid entry %s", e.Path)
		}
		if e.Kind == 'f' && e.Size > 0 && e.Data == nil {
			return fmt.Errorf("missing data for %s", e.Path)
		}
		tree[e.Path] = e
	}
	if tree["/"].Kind != 'd' || tree["/lost+found"].Kind != 'd' {
		return fmt.Errorf("root and lost+found must be directories")
	}
	for p := range tree {
		for d := path.Dir(p); ; d = path.Dir(d) {
			if e, ok := tree[d]; ok {
				if e.Kind != 'd' {
					return fmt.Errorf("parent %s is not a directory", d)
				}
			} else {
				tree[d] = Entry{Path: d, Kind: 'd', Mode: 0755}
			}
			if d == "/" {
				break
			}
		}
	}
	var order []string
	for p := range tree {
		order = append(order, p)
	}
	sort.Slice(order, func(i, j int) bool {
		a, b := order[i], order[j]
		if a == "/" || b == "/" {
			return a == "/"
		}
		if a == "/lost+found" || b == "/lost+found" {
			return a == "/lost+found"
		}
		if (tree[a].Kind == 'h') != (tree[b].Kind == 'h') {
			return tree[a].Kind != 'h'
		}
		if strings.Count(a, "/") != strings.Count(b, "/") {
			return strings.Count(a, "/") < strings.Count(b, "/")
		}
		return a < b
	})
	inos := map[string]int{}
	next := 2
	for _, p := range order {
		if tree[p].Kind != 'h' {
			inos[p] = next
			next++
		}
	}
	if next > s.ncg*s.ipg {
		return fmt.Errorf("filesystem out of inodes")
	}
	for _, p := range order {
		e := tree[p]
		if e.Kind == 'h' {
			target, ok := tree[e.Target]
			if !ok || target.Kind == 'd' || target.Kind == 'h' {
				return fmt.Errorf("invalid hard link %s", p)
			}
			inos[p] = inos[e.Target]
		}
	}
	kids := map[string][]string{}
	links := map[int]int{}
	for _, p := range order {
		if p != "/" {
			kids[path.Dir(p)] = append(kids[path.Dir(p)], p)
			links[inos[p]]++
		}
	}
	links[2]++
	for _, p := range order {
		if tree[p].Kind == 'd' {
			links[inos[p]]++
			for _, k := range kids[p] {
				if tree[k].Kind == 'd' {
					links[inos[p]]++
				}
			}
		}
	}
	for _, p := range order {
		e := tree[p]
		if e.Kind == 'h' {
			continue
		}
		id := inos[p]
		if links[id] > 32767 {
			return fmt.Errorf("too many links for %s", p)
		}
		i := &inode{e: e, links: links[id]}
		var data io.ReaderAt
		var size int64
		switch e.Kind {
		case 'd':
			names := append([]string(nil), kids[p]...)
			sort.Strings(names)
			names = append([]string{p, path.Dir(p)}, names...)
			var out []byte
			var block []byte
			last := 0
			flush := func() {
				if len(block) > 0 {
					old := int(binary.BigEndian.Uint16(block[last+4:]))
					p16(block, last+4, old+512-len(block))
					out = append(out, block...)
					out = append(out, make([]byte, 512-len(block))...)
					block = nil
				}
			}
			for j, k := range names {
				name := path.Base(k)
				if j == 0 {
					name = "."
				}
				if j == 1 {
					name = ".."
				}
				n := 8 + (len(name)+4)&^3
				if len(block)+n > 512 {
					flush()
				}
				last = len(block)
				rec := make([]byte, n)
				p32(rec, 0, inos[k])
				p16(rec, 4, n)
				p16(rec, 6, len(name))
				copy(rec[8:], name)
				block = append(block, rec...)
			}
			flush()
			if p == "/lost+found" {
				for len(out)%8192 != 0 {
					b := make([]byte, 512)
					p16(b, 4, 512)
					out = append(out, b...)
				}
			}
			data = bytes.NewReader(out)
			size = int64(len(out))
			s.dirs[id/s.ipg]++
		case 'f':
			data = e.Data
			size = e.Size
		case 'l':
			if strings.ContainsRune(e.Target, 0) {
				return fmt.Errorf("invalid symlink %s", p)
			}
			data = strings.NewReader(e.Target)
			size = int64(len(e.Target))
		case 'c', 'b':
			i.db[1] = e.Major<<18 | e.Minor
		}
		i.e.Size = size
		if size > 0 {
			if err := s.layout(i, data, size); err != nil {
				return err
			}
		}
		s.nodes[id] = i
	}
	return nil
}
func (s *fs) finish() (Report, error) {
	types := map[byte]int{'d': 0040000, 'f': 0100000, 'l': 0120000, 'c': 0020000, 'b': 0060000, 'p': 0010000}
	for id, i := range s.nodes {
		b := make([]byte, 128)
		mode := types[i.e.Kind] | int(i.e.Mode)
		p16(b, 0, mode)
		p16(b, 2, i.links)
		uid, gid := i.e.UID, i.e.GID
		if uid > 65535 {
			uid = 65535
		}
		if gid > 65535 {
			gid = 65535
		}
		p16(b, 4, int(uid))
		p16(b, 6, int(gid))
		p32(b, 12, int(i.e.Size))
		mt := s.opt.Timestamp
		if i.e.ModTime != nil {
			mt = *i.e.ModTime
		}
		for _, off := range []int{16, 24, 32} {
			p32(b, off, int(mt))
		}
		for j, v := range i.db {
			p32(b, 40+j*4, int(v))
		}
		for j, v := range i.ib {
			p32(b, 88+j*4, int(v))
		}
		p32(b, 104, int(i.blocks))
		p32(b, 112, mode)
		p32(b, 116, int(i.e.UID))
		p32(b, 120, int(i.e.GID))
		p32(b, 124, 0x90909090)
		off := int64(start(id/s.ipg)+32+(id%s.ipg)/8)*1024 + int64(id%8)*128
		if err := s.write(b, off); err != nil {
			return Report{}, err
		}
	}
	var total [4]int
	summary := make([]byte, s.ncg*16)
	for c := 0; c < s.ncg; c++ {
		cg := make([]byte, 2048)
		var freeBlocks, freeFrags int
		var runs [8]int
		var btot [32]int
		var rot [32][8]int
		for d := 0; d < 4096; d += 8 {
			n := 0
			for j := 0; j < 8; j++ {
				n += int(s.free[c*4096+d+j])
			}
			if n == 8 {
				freeBlocks++
				btot[d/256]++
				rot[d/256][d%16/2]++
				continue
			}
			run := 0
			for j := 0; j <= 8; j++ {
				if j < 8 && s.free[c*4096+d+j] != 0 {
					run++
					freeFrags++
				} else if run > 0 {
					runs[run]++
					run = 0
				}
			}
		}
		used := 0
		for id := range s.nodes {
			if id/s.ipg == c {
				used++
				cg[724+(id%s.ipg)/8] |= 1 << uint(id%8)
			}
		}
		if c == 0 {
			used += 2
			cg[724] |= 3
		}
		cs := [4]int{s.dirs[c], freeBlocks, s.ipg - used, freeFrags}
		for j, n := range cs {
			total[j] += n
			p32(summary, c*16+j*4, n)
			p32(cg, 24+j*4, n)
		}
		p32(cg, 8, int(s.opt.Timestamp))
		p32(cg, 12, c)
		if c < s.ncg-1 {
			p16(cg, 16, 16)
		}
		p16(cg, 18, s.ipg)
		p32(cg, 20, 4096)
		for j, n := range runs {
			p32(cg, 52+j*4, n)
		}
		for j, n := range btot {
			p32(cg, 84+j*4, n)
			for k, v := range rot[j] {
				p16(cg, 212+(j*8+k)*2, v)
			}
		}
		p32(cg, 980, 0x090255)
		for d := 0; d < 4096; d++ {
			if s.free[c*4096+d] != 0 {
				cg[984+d/8] |= 1 << uint(d%8)
			}
		}
		if err := s.put(start(c)+24, cg); err != nil {
			return Report{}, err
		}
	}
	if err := s.put(s.dblk, summary); err != nil {
		return Report{}, err
	}
	sb := make([]byte, 2048)
	fields := map[int]int{8: 16, 12: 24, 16: 32, 20: s.dblk, 24: 16, 28: -16, 32: int(s.opt.Timestamp), 36: s.size, 40: s.dsize, 44: s.ncg, 48: 8192, 52: 1024, 56: 8, 60: 10, 68: 60, 72: -8192, 76: -1024, 80: 13, 84: 10, 88: 1, 92: 256, 96: 3, 100: 1, 104: 2048, 108: -512, 112: 9, 116: 2048, 120: 64, 124: 2, 132: int(uint32(0x7c269d38) - s.opt.Timestamp), 152: s.dblk, 156: s.cssize, 160: 2048, 164: 16, 168: 32, 172: 512, 176: s.ncg * 16, 180: 16, 184: s.ipg, 188: 4096, 856: 1, 1372: 0x011954}
	for off, v := range fields {
		p32(sb, off, v)
	}
	for j, n := range total {
		p32(sb, 192+j*4, n)
	}
	copy(sb[212:], s.opt.MountPoint)
	var post [32][8]int
	for c := range post {
		for r := range post[c] {
			post[c][r] = -1
		}
	}
	for f := 248; f >= 0; f -= 8 {
		c, r, blk := f/256, f%16/2, f/8
		if post[c][r] != -1 {
			sb[1376+blk] = byte(post[c][r] - blk)
		}
		post[c][r] = blk
	}
	for c := range post {
		for r, v := range post[c] {
			p16(sb, 860+(c*8+r)*2, v)
		}
	}
	if err := s.write(sb, 8192); err != nil {
		return Report{}, err
	}
	for c := 0; c < s.ncg; c++ {
		if err := s.put(start(c)+16, sb); err != nil {
			return Report{}, err
		}
	}
	return Report{Bytes: int64(s.size) * 1024, Inodes: len(s.nodes), Directories: total[0], FreeBlocks: total[1], FreeFragments: total[3]}, nil
}
