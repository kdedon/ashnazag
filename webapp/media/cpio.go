package media

import (
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"path"
	"strings"
	"unicode/utf8"
)

var ErrChecksum = errors.New("cpio checksum mismatch")

type Limits struct {
	MaxArchiveBytes uint64
	MaxFileBytes    uint64
	MaxNameBytes    uint32
	MaxEntries      uint64
}

type CPIOHeader struct {
	Name                                      string
	Inode, Mode, UID, GID, NLink, MTime, Size uint32
	DevMajor, DevMinor, RDevMajor, RDevMinor  uint32
	Checksum                                  uint32
	CRC                                       bool
}

type CPIOReader struct {
	r               io.Reader
	limits          Limits
	offset, entries uint64
	remaining       uint32
	padding         uint32
	sum, want       uint32
	crc, done       bool
	err             error
}

func NewCPIOReader(r io.Reader, limits Limits) *CPIOReader {
	if limits.MaxArchiveBytes == 0 {
		limits.MaxArchiveBytes = 8 << 30
	}
	if limits.MaxFileBytes == 0 {
		limits.MaxFileBytes = 1 << 30
	}
	if limits.MaxNameBytes == 0 {
		limits.MaxNameBytes = 4096
	}
	if limits.MaxEntries == 0 {
		limits.MaxEntries = 1_000_000
	}
	return &CPIOReader{r: r, limits: limits}
}

func (r *CPIOReader) Offset() uint64 { return r.offset }

func (r *CPIOReader) fail(err error) error {
	r.err = fmt.Errorf("cpio at byte %d: %w", r.offset, err)
	return r.err
}

func (r *CPIOReader) exact(p []byte) error {
	if uint64(len(p)) > r.limits.MaxArchiveBytes-r.offset {
		return r.fail(errors.New("archive size limit exceeded"))
	}
	n, err := io.ReadFull(r.r, p)
	r.offset += uint64(n)
	if err != nil {
		if err == io.EOF {
			err = io.ErrUnexpectedEOF
		}
		return r.fail(err)
	}
	return nil
}

func (r *CPIOReader) skipPadding(n uint32) error {
	var b [3]byte
	return r.exact(b[:n])
}

func (r *CPIOReader) Next() (*CPIOHeader, error) {
	if r.err != nil {
		return nil, r.err
	}
	if r.done {
		return nil, io.EOF
	}
	if _, err := io.Copy(io.Discard, r); err != nil {
		return nil, err
	}
	if err := r.skipPadding(r.padding); err != nil {
		return nil, err
	}
	r.padding = 0
	var raw [110]byte
	if err := r.exact(raw[:]); err != nil {
		return nil, err
	}
	magic := string(raw[:6])
	if magic != "070701" && magic != "070702" {
		return nil, r.fail(errors.New("unsupported cpio magic"))
	}
	var v [13]uint32
	for i := range v {
		var b [4]byte
		if _, err := hex.Decode(b[:], raw[6+i*8:14+i*8]); err != nil {
			return nil, r.fail(errors.New("invalid hexadecimal header"))
		}
		v[i] = uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3])
	}
	namesize := v[11]
	if namesize < 1 || namesize > r.limits.MaxNameBytes {
		return nil, r.fail(errors.New("name size limit exceeded"))
	}
	if uint64(v[6]) > r.limits.MaxFileBytes {
		return nil, r.fail(errors.New("file size limit exceeded"))
	}
	namepad := (4 - (uint64(110)+uint64(namesize))%4) % 4
	datapad := (4 - v[6]%4) % 4
	needed := uint64(namesize) + namepad + uint64(v[6]) + uint64(datapad)
	if needed > r.limits.MaxArchiveBytes-r.offset {
		return nil, r.fail(errors.New("archive size limit exceeded"))
	}
	name := make([]byte, namesize)
	if err := r.exact(name); err != nil {
		return nil, err
	}
	if name[len(name)-1] != 0 || strings.IndexByte(string(name[:len(name)-1]), 0) >= 0 {
		return nil, r.fail(errors.New("invalid filename terminator"))
	}
	if err := r.skipPadding(uint32(namepad)); err != nil {
		return nil, err
	}
	if string(name[:len(name)-1]) == "TRAILER!!!" {
		if v[6] != 0 || v[12] != 0 {
			return nil, r.fail(errors.New("invalid trailer"))
		}
		r.done = true
		return nil, io.EOF
	}
	clean, err := NormalizePath(string(name[:len(name)-1]))
	if err != nil {
		return nil, r.fail(err)
	}
	r.entries++
	if r.entries > r.limits.MaxEntries {
		return nil, r.fail(errors.New("entry count limit exceeded"))
	}
	h := &CPIOHeader{Name: clean, Inode: v[0], Mode: v[1], UID: v[2], GID: v[3], NLink: v[4], MTime: v[5], Size: v[6], DevMajor: v[7], DevMinor: v[8], RDevMajor: v[9], RDevMinor: v[10], Checksum: v[12], CRC: magic == "070702"}
	r.remaining, r.padding = h.Size, datapad
	r.sum, r.want, r.crc = 0, h.Checksum, h.CRC
	if r.remaining == 0 && r.crc && r.want != 0 {
		return nil, r.fail(ErrChecksum)
	}
	return h, nil
}

func (r *CPIOReader) Read(p []byte) (int, error) {
	if r.err != nil {
		return 0, r.err
	}
	if r.remaining == 0 {
		return 0, io.EOF
	}
	if len(p) == 0 {
		return 0, nil
	}
	if uint64(len(p)) > uint64(r.remaining) {
		p = p[:r.remaining]
	}
	n, err := r.r.Read(p)
	r.offset += uint64(n)
	r.remaining -= uint32(n)
	if r.crc {
		for _, b := range p[:n] {
			r.sum += uint32(b)
		}
	}
	if r.remaining == 0 {
		if r.crc && r.sum != r.want {
			return n, r.fail(ErrChecksum)
		}
		if err == io.EOF {
			err = nil
		}
	} else if err == io.EOF {
		err = io.ErrUnexpectedEOF
	}
	if err != nil {
		return n, r.fail(err)
	}
	return n, nil
}

func NormalizePath(name string) (string, error) {
	if name == "" || strings.HasPrefix(name, "/") || strings.ContainsAny(name, "\\\x00") || !utf8.ValidString(name) {
		return "", errors.New("unsafe archive path")
	}
	for _, part := range strings.Split(name, "/") {
		if part == ".." {
			return "", errors.New("unsafe archive path")
		}
	}
	return path.Clean(name), nil
}
