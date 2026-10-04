package sources

import (
	"amigaux.org/imagebuilder/internal/lhadecode/crc16"
	"amigaux.org/imagebuilder/internal/lhadecode/lzhuff"
	"amigaux.org/imagebuilder/ufs"
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"strings"
	"time"
)

type limitedOutput struct {
	bytes.Buffer
	max int
	ctx context.Context
}

func (w *limitedOutput) Write(p []byte) (int, error) {
	if err := w.ctx.Err(); err != nil {
		return 0, err
	}
	if len(p) > w.max-w.Len() {
		return 0, fmt.Errorf("LHA expanded size exceeded")
	}
	return w.Buffer.Write(p)
}
func crc(b []byte) uint16 { h := crc16.NewIBM(); h.Write(b); return h.Sum16() }
func readLHA(ctx context.Context, r io.ReaderAt, size int64, l Limits) (Result, error) {
	out := Result{Metadata: map[string]Metadata{}}
	var pos, total int64
	le := binary.LittleEndian
	for pos < size {
		if err := ctx.Err(); err != nil {
			return out, err
		}
		first, err := readAt(r, pos, 1, size)
		if err != nil {
			return out, err
		}
		if first[0] == 0 {
			for off := pos + 1; off < size; {
				n := size - off
				if n > 4096 {
					n = 4096
				}
				tail, e := readAt(r, off, n, size)
				if e != nil {
					return out, e
				}
				for _, b := range tail {
					if b != 0 {
						return out, fmt.Errorf("LHA trailing data")
					}
				}
				off += n
			}
			return out, nil
		}
		base, err := readAt(r, pos, 21, size)
		if err != nil {
			return out, err
		}
		level := base[20]
		if level > 2 {
			return out, fmt.Errorf("unsupported LHA header level %d", level)
		}
		hlen := int64(base[0]) + 2
		if level == 2 {
			hlen = int64(le.Uint16(base))
		}
		if hlen < 24 || hlen > 65535 {
			return out, fmt.Errorf("LHA header length")
		}
		h, err := readAt(r, pos, hlen, size)
		if err != nil {
			return out, err
		}
		method := string(h[2:7])
		packed, unpacked := int64(le.Uint32(h[7:])), int64(le.Uint32(h[11:]))
		if unpacked > l.ExpandedBytes-total {
			return out, fmt.Errorf("LHA expanded limit")
		}
		total += unpacked
		name, dir, comment := "", "", ""
		mode := uint32(0644)
		var want uint16
		ext := 0
		end := pos + hlen
		if level < 2 {
			var sum byte
			for _, b := range h[2:] {
				sum += b
			}
			if sum != h[1] {
				return out, fmt.Errorf("LHA header checksum")
			}
			nl := int(h[21])
			if 22+nl+2 > len(h) {
				return out, fmt.Errorf("LHA filename bounds")
			}
			name = string(h[22 : 22+nl])
			want = le.Uint16(h[22+nl:])
			if level == 1 {
				if hlen < int64(27+nl) {
					return out, fmt.Errorf("LHA level1 header")
				}
				ext = int(le.Uint16(h[len(h)-2:]))
			}
		} else {
			if len(h) < 26 {
				return out, fmt.Errorf("LHA level2 header")
			}
			want = le.Uint16(h[21:])
			ext = int(le.Uint16(h[24:]))
			end = pos + 26
		}
		seenExt := map[byte]bool{}
		rawHeader := append([]byte(nil), h...)
		var headerCRC *uint16
		for ext > 0 {
			if ext < 3 || ext > 65535 {
				return out, fmt.Errorf("LHA extension bounds")
			}
			e, err := readAt(r, end, int64(ext), size)
			if err != nil {
				return out, err
			}
			if level == 2 && end+int64(ext) > pos+hlen {
				return out, fmt.Errorf("LHA extension beyond header")
			}
			if seenExt[e[0]] {
				return out, fmt.Errorf("duplicate LHA extension")
			}
			seenExt[e[0]] = true
			field := e[1 : len(e)-2]
			switch e[0] {
			case 0:
				if len(field) < 2 {
					return out, fmt.Errorf("LHA CRC extension")
				}
				v := le.Uint16(field)
				headerCRC = &v
				off := int(end-pos) + 1
				if level == 2 {
					rawHeader[off] = 0
					rawHeader[off+1] = 0
				} else {
					e = append([]byte(nil), e...)
					e[1] = 0
					e[2] = 0
				}
			case 1:
				name = string(field)
			case 2:
				dir = strings.ReplaceAll(string(field), "\xff", "/")
				if dir != "" && !strings.HasSuffix(dir, "/") {
					dir += "/"
				}
			case 0x3f:
				comment = string(field)
			case 0x50:
				if len(field) != 2 {
					return out, fmt.Errorf("LHA mode extension")
				}
				m := le.Uint16(field)
				if m&0170000 != 0 && m&0170000 != 0100000 && m&0170000 != 0040000 {
					return out, fmt.Errorf("unsupported LHA special file")
				}
				mode = uint32(m) & 07777
			case 0x39, 0x42:
				return out, fmt.Errorf("unsupported LHA multi-volume or large-file header")
			}
			if level == 1 {
				packed -= int64(ext)
				if packed < 0 {
					return out, fmt.Errorf("LHA packed size")
				}
				rawHeader = append(rawHeader, e...)
			}
			end += int64(ext)
			ext = int(le.Uint16(e[len(e)-2:]))
		}
		if level == 2 {
			end = pos + hlen
		}
		if headerCRC != nil && crc(rawHeader) != *headerCRC {
			return out, fmt.Errorf("LHA header CRC mismatch")
		}
		p, err := relative(dir + name)
		if err != nil {
			return out, fmt.Errorf("LHA path %q: %w", dir+name, err)
		}
		if len(out.Entries) >= l.Entries {
			return out, fmt.Errorf("LHA entry limit")
		}
		if packed < 0 || packed > size-end {
			return out, io.ErrUnexpectedEOF
		}
		e := ufs.Entry{Path: p, Kind: 'd', Mode: 0755}
		if method != "-lhd-" {
			w := &limitedOutput{max: int(unpacked), ctx: ctx}
			input := io.NewSectionReader(r, end, packed)
			switch method {
			case "-lh0-":
				if packed != unpacked {
					return out, fmt.Errorf("LHA stored length mismatch")
				}
				_, err = io.Copy(w, input)
			case "-lh4-", "-lh5-", "-lh6-", "-lh7-":
				bits, pbits, pnum := uint(13), 4, 14
				if method == "-lh4-" {
					bits = 12
				}
				if method == "-lh6-" {
					bits, pbits, pnum = 15, 5, 16
				}
				if method == "-lh7-" {
					bits, pbits, pnum = 16, 5, 17
				}
				_, _, err = lzhuff.Decode(lzhuff.NewStaticDecoder(input, pbits, pnum), w, bits, 253, int(unpacked))
			default:
				return out, fmt.Errorf("unsupported LHA method %s", method)
			}
			if err != nil {
				return out, err
			}
			if w.Len() != int(unpacked) || crc(w.Bytes()) != want {
				return out, fmt.Errorf("LHA file length/CRC mismatch")
			}
			e = file(p, w.Bytes())
			e.Mode = mode
		} else if packed != 0 || unpacked != 0 {
			return out, fmt.Errorf("LHA directory payload")
		}
		tm := le.Uint32(h[15:])
		if level < 2 {
			sec := int(tm&31) * 2
			min := int(tm >> 5 & 63)
			hour := int(tm >> 11 & 31)
			day := int(tm >> 16 & 31)
			month := int(tm >> 21 & 15)
			year := int(tm>>25) + 1980
			if month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59 {
				return out, fmt.Errorf("LHA invalid DOS date")
			}
			date := time.Date(year, time.Month(month), day, hour, min, sec, 0, time.UTC)
			if date.Day() != day {
				return out, fmt.Errorf("LHA invalid DOS calendar date")
			}
			tm = uint32(date.Unix())
		}
		e.ModTime = &tm
		out.Entries = append(out.Entries, e)
		out.Metadata[p] = Metadata{Comment: comment, Encoding: "archive-bytes"}
		pos = end + packed
	}
	return out, fmt.Errorf("LHA missing terminator")
}
