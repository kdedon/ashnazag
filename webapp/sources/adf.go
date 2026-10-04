package sources

import (
	"amigaux.org/imagebuilder/ufs"
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"strings"
)

func readADF(ctx context.Context, r io.ReaderAt, size int64, l Limits) (Result, error) {
	out := Result{Metadata: map[string]Metadata{}}
	if size != 901120 && size != 1802240 {
		return out, fmt.Errorf("ADF requires DD/HD floppy")
	}
	raw, err := readAt(r, 0, size, size)
	if err != nil {
		return out, err
	}
	if string(raw[:3]) != "DOS" || raw[3] > 3 {
		return out, fmt.Errorf("unsupported ADF filesystem")
	}
	blocks := int(size / 512)
	ffs := raw[3]&1 != 0
	used := map[uint32]bool{0: true, 1: true}
	var expanded int64
	block := func(n uint32, check bool) ([]byte, []uint32, error) {
		if n < 2 || n >= uint32(blocks) {
			return nil, nil, fmt.Errorf("ADF block outside image")
		}
		b := raw[int(n)*512 : int(n+1)*512]
		w := make([]uint32, 128)
		var sum uint32
		for i := range w {
			w[i] = binary.BigEndian.Uint32(b[i*4:])
			sum += w[i]
		}
		if check && sum != 0 {
			return nil, nil, fmt.Errorf("ADF checksum mismatch")
		}
		return b, w, nil
	}
	claim := func(n uint32) error {
		if used[n] {
			return fmt.Errorf("ADF cyclic/shared block")
		}
		used[n] = true
		return ctx.Err()
	}
	var contents func(uint32, []uint32) ([]byte, error)
	contents = func(number uint32, h []uint32) ([]byte, error) {
		length := int64(h[81])
		if length > size || length > l.ExpandedBytes-expanded {
			return nil, fmt.Errorf("ADF expanded limit")
		}
		expanded += length
		var data []byte
		seq := uint32(1)
		nextOFS := h[4]
		current := h
		for {
			count := int(current[2])
			if count > 72 {
				return nil, fmt.Errorf("ADF pointer count")
			}
			for _, p := range current[6 : 78-count] {
				if p != 0 {
					return nil, fmt.Errorf("ADF unused pointer")
				}
			}
			for i := 77; i >= 78-count; i-- {
				p := current[i]
				if err := claim(p); err != nil {
					return nil, err
				}
				b, w, err := block(p, !ffs)
				if err != nil {
					return nil, err
				}
				if ffs {
					data = append(data, b...)
				} else {
					if p != nextOFS || w[0] != 8 || w[1] != number || w[2] != seq || w[3] > 488 {
						return nil, fmt.Errorf("ADF OFS data")
					}
					data = append(data, b[24:24+w[3]]...)
					seq++
					nextOFS = w[4]
				}
				if int64(len(data)) > length+512 {
					return nil, fmt.Errorf("ADF data exceeds declared size")
				}
			}
			ext := current[126]
			if ext == 0 {
				break
			}
			if err := claim(ext); err != nil {
				return nil, err
			}
			_, w, err := block(ext, true)
			if err != nil {
				return nil, err
			}
			if w[0] != 16 || w[1] != ext || w[127] != 0xfffffffd || w[125] != number {
				return nil, fmt.Errorf("ADF extension invalid")
			}
			current = w
		}
		if !ffs && (int64(len(data)) != length || nextOFS != 0) {
			return nil, fmt.Errorf("ADF OFS chain/length mismatch")
		}
		cap := int64(488)
		if ffs {
			cap = 512
		}
		if int64(len(data)) < length || int64(len(data)) >= length+cap {
			return nil, fmt.Errorf("ADF file length mismatch")
		}
		return data[:length], nil
	}
	var dir func(uint32, []uint32, string, int) error
	dir = func(parent uint32, w []uint32, prefix string, depth int) error {
		if depth > 32 {
			return fmt.Errorf("ADF nesting limit")
		}
		for _, first := range w[6:78] {
			for n := first; n != 0; {
				if err := claim(n); err != nil {
					return err
				}
				b, h, err := block(n, true)
				if err != nil {
					return err
				}
				if h[0] != 2 || h[1] != n || h[125] != parent {
					return fmt.Errorf("ADF entry invalid")
				}
				length := int(b[432])
				if length < 1 || length > 30 {
					return fmt.Errorf("ADF name length")
				}
				name := string(b[433 : 433+length])
				if name == "." || name == ".." || strings.ContainsAny(name, "/\\:\x00") {
					return fmt.Errorf("ADF invalid filename component")
				}
				p, err := relative(prefix + name)
				if err != nil {
					return err
				}
				if len(out.Entries) >= l.Entries {
					return fmt.Errorf("ADF entry limit")
				}
				commentLen := int(b[328])
				if commentLen > 79 {
					return fmt.Errorf("ADF comment invalid")
				}
				tm64 := uint64(h[105])*86400 + uint64(h[106])*60 + uint64(h[107])/50 + 252460800
				if h[106] >= 1440 || h[107] >= 3000 || tm64 > 0xffffffff {
					return fmt.Errorf("ADF invalid timestamp")
				}
				tm := uint32(tm64)
				out.Metadata[p] = Metadata{Protection: h[80], Comment: string(b[329 : 329+commentLen]), Encoding: "latin1"}
				switch h[127] {
				case 2:
					out.Entries = append(out.Entries, ufs.Entry{Path: p, Kind: 'd', Mode: 0755, ModTime: &tm})
					if err := dir(n, h, p[1:]+"/", depth+1); err != nil {
						return err
					}
				case 0xfffffffd:
					data, err := contents(n, h)
					if err != nil {
						return err
					}
					e := file(p, data)
					e.ModTime = &tm
					e.Mode = 0644
					if h[80]&2 == 0 {
						e.Mode |= 0111
					}
					if h[80]&4 != 0 {
						e.Mode &^= 0222
					}
					out.Entries = append(out.Entries, e)
				default:
					return fmt.Errorf("unsupported ADF link or entry")
				}
				n = h[124]
			}
		}
		return nil
	}
	root := uint32(blocks / 2)
	if err := claim(root); err != nil {
		return out, err
	}
	_, h, err := block(root, true)
	if err != nil {
		return out, err
	}
	if h[0] != 2 || h[3] != 72 || h[127] != 1 {
		return out, fmt.Errorf("invalid ADF root")
	}
	err = dir(root, h, "", 0)
	return out, err
}
