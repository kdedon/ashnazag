// Package amixtape finds the AMIX 2.1 tape's segments in what the user has:
// archive parts (.tar.bz2, .tar.gz, .tar, .zip), a SIMH .tap image, loose
// segment files or a raw image holding the segments back to back. Segments
// are recognised by size and SHA-256, whatever the member names.
package amixtape

import (
	"archive/tar"
	"archive/zip"
	"bufio"
	"bytes"
	"compress/bzip2"
	"compress/gzip"
	"context"
	"crypto/sha256"
	_ "embed"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"hash"
	"io"
	"path"
	"sort"
	"strings"
)

//go:embed segments.json
var tableJSON []byte

type Segment struct {
	ID     string `json:"id"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

// Table lists the AMIX 2.1 segments in tape order.
var Table []Segment

func init() {
	var t struct {
		Segments []Segment `json:"segments"`
	}
	if err := json.Unmarshal(tableJSON, &t); err != nil || len(t.Segments) == 0 {
		panic("amixtape: bad segment table")
	}
	Table = t.Segments
}

// Part is one file the user supplied.
type Part struct {
	Name   string
	Reader io.ReaderAt
	Size   int64
}

// Digest identifies a part's bytes.
type Digest struct {
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

type Result struct {
	Found   map[string]string // segment id: where it was found
	Damaged map[string]string // segment id: why a member of that name failed
	Data    map[string][]byte // the wanted segments
	Parts   []Digest          // each part, in the order given
}

// Missing lists the ids not found, in table order.
func (r *Result) Missing(table []Segment) []string {
	var out []string
	for _, s := range table {
		if _, ok := r.Found[s.ID]; !ok {
			out = append(out, s.ID)
		}
	}
	return out
}

// Need names each of ids that was not found.
func (r *Result) Need(ids []string) error {
	var missing, damaged []string
	for _, id := range ids {
		if _, ok := r.Found[id]; ok {
			continue
		}
		if why, ok := r.Damaged[id]; ok {
			damaged = append(damaged, fmt.Sprintf("segment %s is damaged (%s)", id, why))
		} else {
			missing = append(missing, id)
		}
	}
	if len(missing) != 0 {
		s := ""
		if len(missing) > 1 {
			s = "s"
		}
		damaged = append(damaged, fmt.Sprintf("segment%s %s not found", s, strings.Join(missing, ", ")))
	}
	if len(damaged) != 0 {
		return fmt.Errorf("AMIX 2.1 tape: %s", strings.Join(damaged, "; "))
	}
	return nil
}

type scanner struct {
	table    []Segment
	ctx      context.Context
	bySize   map[int64][]Segment
	label    map[string]bool
	want     map[string]bool // still wanted; nil scans everything
	res      *Result
	progress func(id string)
	buf      []byte
	maxSeg   int64
	total    int64
	budget   int64 // decompressed bytes the current part may still produce
}

var errExpands = errors.New("archive expands far beyond the size of the tape")

// capped bounds the bytes read through it by the scanner's part budget.
type capped struct {
	r io.Reader
	s *scanner
}

func (c capped) Read(p []byte) (int, error) {
	if c.s.budget <= 0 {
		return 0, errExpands
	}
	n, err := c.r.Read(p[:min(int64(len(p)), c.s.budget)])
	c.s.budget -= int64(n)
	return n, err
}

// Scan reads parts in order and identifies the segments in them against
// table. The bytes of each segment in want are kept; once all are found the
// remaining archives are only hashed. With want nil every part is read in
// full. Memory stays bounded by the wanted segments.
func Scan(ctx context.Context, table []Segment, parts []Part, want []string, progress func(id string)) (*Result, error) {
	s := &scanner{table: table, ctx: ctx, bySize: map[int64][]Segment{}, label: map[string]bool{}, buf: make([]byte, 256<<10), progress: progress,
		res: &Result{Found: map[string]string{}, Damaged: map[string]string{}, Data: map[string][]byte{}}}
	for _, seg := range table {
		s.bySize[seg.Size] = append(s.bySize[seg.Size], seg)
		s.label[seg.ID] = true
		s.maxSeg = max(s.maxSeg, seg.Size)
		s.total += seg.Size
	}
	if want != nil {
		s.want = map[string]bool{}
		for _, id := range want {
			s.want[id] = true
		}
	}
	for i, p := range parts {
		d, err := s.part(p)
		if err != nil {
			return nil, fmt.Errorf("tape part %d (%s): %w", i+1, p.Name, err)
		}
		s.res.Parts = append(s.res.Parts, d)
	}
	return s.res, nil
}

func (s *scanner) done() bool { return s.want != nil && len(s.want) == 0 }

// keep returns how many bytes of a member of size bytes to keep, since it
// may be a wanted segment; size -1 is unknown.
func (s *scanner) keep(size int64) int64 {
	var limit int64
	for _, seg := range s.table {
		if s.want[seg.ID] && (size < 0 || seg.Size == size) {
			limit = max(limit, seg.Size)
		}
	}
	return limit
}

// member identifies one candidate file. label is the segment it claims to
// be, for naming damage.
func (s *scanner) member(where, label string, r io.Reader, size int64) error {
	if s.done() || size > s.maxSeg {
		return nil
	}
	limit := s.keep(size)
	var data []byte
	if limit > 0 && size >= 0 {
		data = make([]byte, 0, size)
	}
	h := sha256.New()
	var n int64
	for {
		if err := s.ctx.Err(); err != nil {
			return err
		}
		got, err := r.Read(s.buf)
		h.Write(s.buf[:got])
		n += int64(got)
		if limit > 0 && n <= limit {
			data = append(data, s.buf[:got]...)
		} else {
			data = nil
		}
		if err == io.EOF {
			break
		}
		if err != nil {
			return fmt.Errorf("%s: %w", where, err)
		}
	}
	if size >= 0 && n != size {
		return fmt.Errorf("%s: %w", where, io.ErrUnexpectedEOF)
	}
	if n == 0 {
		return nil
	}
	sum := hex.EncodeToString(h.Sum(nil))
	id := ""
	for _, seg := range s.bySize[n] {
		if seg.SHA256 == sum {
			id = seg.ID
		}
	}
	if id == "" {
		if _, found := s.res.Found[label]; s.label[label] && !found {
			s.res.Damaged[label] = fmt.Sprintf("%s: %d bytes, sha256 %s…", where, n, sum[:12])
		}
		return nil
	}
	if _, ok := s.res.Found[id]; ok {
		return nil
	}
	s.res.Found[id] = where
	delete(s.res.Damaged, id)
	if s.want[id] {
		s.res.Data[id] = data
		delete(s.want, id)
	}
	if s.progress != nil {
		s.progress(id)
	}
	return nil
}

// hashing passes a part through once while hashing it.
type hashing struct {
	r io.Reader
	h hash.Hash
	n int64
}

func (t *hashing) Read(p []byte) (int, error) {
	n, err := t.r.Read(p)
	t.h.Write(p[:n])
	t.n += int64(n)
	return n, err
}

func (s *scanner) part(p Part) (Digest, error) {
	head := make([]byte, min(p.Size, 512))
	if n, err := p.Reader.ReadAt(head, 0); n != len(head) {
		return Digest{}, fmt.Errorf("read: %v", err)
	}
	t := &hashing{r: io.NewSectionReader(p.Reader, 0, p.Size), h: sha256.New()}
	s.budget = 2*s.total + 64<<20
	var err error
	name := path.Base(p.Name)
	switch {
	case bytes.HasPrefix(head, []byte("BZh")):
		err = s.tar(name, capped{bzip2.NewReader(bufio.NewReader(t)), s})
	case bytes.HasPrefix(head, []byte{0x1f, 0x8b}):
		var z *gzip.Reader
		if z, err = gzip.NewReader(bufio.NewReader(t)); err == nil {
			err = s.tar(name, capped{z, s})
		}
	case len(head) == 512 && string(head[257:262]) == "ustar":
		err = s.tar(name, t)
	case bytes.HasPrefix(head, []byte("PK\x03\x04")):
		err = s.zip(name, p)
	case bytes.HasPrefix(head, []byte("\xfd7zXZ")):
		return Digest{}, errors.New("xz archives are not supported; use .tar.bz2, .tar.gz or the extracted segments")
	case strings.HasSuffix(strings.ToLower(name), ".tap"):
		err = s.tap(name, bufio.NewReaderSize(t, 1<<16))
	case len(s.bySize[p.Size]) != 0:
		err = s.member(name, name, io.NewSectionReader(p.Reader, 0, p.Size), p.Size)
	default:
		err = s.raw(name, p)
	}
	if err != nil {
		return Digest{}, err
	}
	// Hash what the formats above left unread.
	if _, err = io.CopyBuffer(io.Discard, t, s.buf); err != nil {
		return Digest{}, err
	}
	if t.n != p.Size {
		return Digest{}, io.ErrUnexpectedEOF
	}
	return Digest{Size: p.Size, SHA256: hex.EncodeToString(t.h.Sum(nil))}, nil
}

func (s *scanner) tar(name string, r io.Reader) error {
	tr := tar.NewReader(r)
	for !s.done() {
		h, err := tr.Next()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return fmt.Errorf("archive is damaged or incomplete: %w", err)
		}
		if h.Typeflag == tar.TypeReg {
			if err = s.member(name+":"+h.Name, path.Base(h.Name), tr, h.Size); err != nil {
				return err
			}
		}
	}
	return nil
}

func (s *scanner) zip(name string, p Part) error {
	z, err := zip.NewReader(p.Reader, p.Size)
	if err != nil {
		return err
	}
	files := append([]*zip.File(nil), z.File...)
	sort.SliceStable(files, func(i, j int) bool { return files[i].Name < files[j].Name })
	for _, f := range files {
		if f.FileInfo().IsDir() || s.done() {
			continue
		}
		r, err := f.Open()
		if err != nil {
			return err
		}
		err = s.member(name+":"+f.Name, path.Base(f.Name), capped{io.LimitReader(r, int64(f.UncompressedSize64)+1), s}, int64(f.UncompressedSize64))
		r.Close()
		if err != nil {
			return err
		}
	}
	return nil
}

// tapFile reads one file of a SIMH tape image: the records up to a tape mark.
type tapFile struct {
	r         *bufio.Reader
	left      int64
	skip      int
	mark, end bool
}

func (t *tapFile) Read(p []byte) (int, error) {
	for t.left == 0 {
		if t.mark || t.end {
			return 0, io.EOF
		}
		if _, err := t.r.Discard(t.skip); err != nil {
			return 0, errors.New("truncated SIMH image")
		}
		t.skip = 0
		var hdr [4]byte
		if n, err := io.ReadFull(t.r, hdr[:]); err != nil {
			if n == 0 && err == io.EOF {
				t.end = true
				return 0, io.EOF
			}
			return 0, errors.New("truncated SIMH image")
		}
		switch n := binary.LittleEndian.Uint32(hdr[:]); {
		case n == 0:
			t.mark = true
		case n == 0xffffffff:
			t.end = true
		case n>>28 == 0xf:
			// Gap and reserved markers carry no data and no trailer.
		default:
			t.left = int64(n & 0xffffff)
			if n&0x80000000 != 0 {
				// Error record: its data is not trustworthy, skip it.
				t.skip = int(t.left) + int(t.left&1) + 4
				t.left = 0
				continue
			}
			// Odd records are padded; each ends with its length again.
			t.skip = int(t.left&1) + 4
		}
	}
	n, err := t.r.Read(p[:min(int64(len(p)), t.left)])
	t.left -= int64(n)
	if err == io.EOF && t.left != 0 {
		err = errors.New("truncated SIMH image")
	} else if err == io.EOF {
		err = nil
	}
	return n, err
}

func (s *scanner) tap(name string, r *bufio.Reader) error {
	t := &tapFile{r: r}
	for k := 0; !t.end && !s.done(); {
		t.mark = false
		c := &counter{r: t}
		if err := s.member(fmt.Sprintf("%s:file%d", name, k), fmt.Sprintf("%02d", k), c, -1); err != nil {
			return err
		}
		if c.n != 0 {
			k++
		}
	}
	return nil
}

type counter struct {
	r io.Reader
	n int64
}

func (c *counter) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.n += int64(n)
	return n, err
}

// raw takes a file of no known format as the segments back to back.
func (s *scanner) raw(name string, p Part) error {
	var off int64
	for _, seg := range s.table {
		if s.done() || off+seg.Size > p.Size {
			break
		}
		if err := s.member(fmt.Sprintf("%s@%d", name, off), seg.ID, io.NewSectionReader(p.Reader, off, seg.Size), seg.Size); err != nil {
			return err
		}
		off += seg.Size
	}
	if off == 0 {
		return errors.New("not an archive, .tap, segment file or raw AMIX tape")
	}
	return nil
}
