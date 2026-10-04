package media

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"strings"
	"testing"
)

func entry(name, body string, crc bool) []byte {
	v := [13]uint32{7, 0100644, 42, 43, 2, 123456, uint32(len(body)), 8, 9, 10, 11, uint32(len(name) + 1), 0}
	magic := "070701"
	if crc {
		magic = "070702"
		for _, b := range []byte(body) {
			v[12] += uint32(b)
		}
	}
	var out bytes.Buffer
	out.WriteString(magic)
	for _, n := range v {
		fmt.Fprintf(&out, "%08x", n)
	}
	out.WriteString(name)
	out.WriteByte(0)
	for out.Len()%4 != 0 {
		out.WriteByte(0)
	}
	out.WriteString(body)
	for out.Len()%4 != 0 {
		out.WriteByte(0)
	}
	return out.Bytes()
}

func archive(parts ...[]byte) []byte {
	return bytes.Join(append(parts, entry("TRAILER!!!", "", false)), nil)
}

type smallReads struct{ io.Reader }

func (r smallReads) Read(p []byte) (int, error) {
	if len(p) > 3 {
		p = p[:3]
	}
	return r.Reader.Read(p)
}

func TestStreamingAndMetadata(t *testing.T) {
	data := archive(entry("./usr//bin/test", "abcde", true), entry("./etc", "", false))
	r := NewCPIOReader(smallReads{bytes.NewReader(data)}, Limits{})
	h, err := r.Next()
	if err != nil {
		t.Fatal(err)
	}
	if h.Name != "usr/bin/test" || h.Inode != 7 || h.Mode != 0100644 || h.UID != 42 || h.GID != 43 || h.NLink != 2 || h.MTime != 123456 || h.Size != 5 || h.DevMajor != 8 || h.DevMinor != 9 || h.RDevMajor != 10 || h.RDevMinor != 11 || !h.CRC {
		t.Fatalf("metadata: %+v", h)
	}
	b := make([]byte, 2)
	if _, err := io.ReadFull(r, b); err != nil || string(b) != "ab" {
		t.Fatalf("read %q: %v", b, err)
	}
	h, err = r.Next()
	if err != nil || h.Name != "etc" {
		t.Fatalf("next: %+v %v", h, err)
	}
	if _, err = r.Next(); err != io.EOF {
		t.Fatal(err)
	}
	if r.Offset() != uint64(len(data)) {
		t.Fatalf("offset %d, length %d", r.Offset(), len(data))
	}
	if _, err = r.Next(); err != io.EOF {
		t.Fatal(err)
	}
}

func TestChecksumAndSkippedBody(t *testing.T) {
	for _, skip := range []bool{false, true} {
		data := entry("file", "body", true)
		data[len(data)-1] ^= 1
		r := NewCPIOReader(bytes.NewReader(archive(data)), Limits{})
		if _, err := r.Next(); err != nil {
			t.Fatal(err)
		}
		var err error
		if skip {
			_, err = r.Next()
		} else {
			_, err = io.ReadAll(r)
		}
		if !errors.Is(err, ErrChecksum) {
			t.Fatalf("skip=%v: %v", skip, err)
		}
		if _, again := r.Next(); again != err {
			t.Fatalf("error not sticky: %v %v", err, again)
		}
	}
}

func TestUnsafePaths(t *testing.T) {
	for _, name := range []string{"", "/etc/passwd", "../file", "etc/../file", "x/../../file", "x\\..\\file", "x\x00file", string([]byte{255})} {
		t.Run(fmt.Sprintf("%q", name), func(t *testing.T) {
			r := NewCPIOReader(bytes.NewReader(archive(entry(name, "", false))), Limits{})
			if _, err := r.Next(); err == nil {
				t.Fatalf("accepted %q", name)
			}
		})
	}
	if got, err := NormalizePath("./"); err != nil || got != "." {
		t.Fatalf("root: %q %v", got, err)
	}
}

func TestBoundsAndMalformedInput(t *testing.T) {
	valid := archive(entry("file", "contents", true))
	tests := []struct {
		name   string
		data   []byte
		limits Limits
		want   string
	}{
		{"file limit", valid, Limits{MaxFileBytes: 7}, "file size"},
		{"name limit", valid, Limits{MaxNameBytes: 4}, "name size"},
		{"archive limit", valid, Limits{MaxArchiveBytes: 115}, "archive size"},
		{"entry limit", archive(entry("a", "", false), entry("b", "", false)), Limits{MaxEntries: 1}, "entry count"},
		{"missing trailer", entry("file", "contents", false), Limits{}, "unexpected EOF"},
		{"body truncated", valid[:120], Limits{}, "unexpected EOF"},
		{"header truncated", valid[:70], Limits{}, "unexpected EOF"},
		{"bad magic", []byte(strings.Repeat("0", 110)), Limits{}, "magic"},
		{"bad trailer", entry("TRAILER!!!", "x", false), Limits{}, "trailer"},
	}
	badHex := bytes.Clone(valid)
	badHex[6] = 'z'
	tests = append(tests, struct {
		name   string
		data   []byte
		limits Limits
		want   string
	}{"bad hex", badHex, Limits{}, "hexadecimal"})
	badNUL := bytes.Clone(valid)
	badNUL[114] = 'x'
	tests = append(tests, struct {
		name   string
		data   []byte
		limits Limits
		want   string
	}{"bad terminator", badNUL, Limits{}, "terminator"})
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			r := NewCPIOReader(bytes.NewReader(tt.data), tt.limits)
			var err error
			for err == nil {
				_, err = r.Next()
			}
			if err == io.EOF || !strings.Contains(err.Error(), tt.want) {
				t.Fatalf("want %q: %v", tt.want, err)
			}
		})
	}
}

func TestPreservesSymlinkPayload(t *testing.T) {
	b := entry("usr/lib/link", "../../lib/target", true)
	copy(b[14:22], []byte("0000a1ff"))
	r := NewCPIOReader(bytes.NewReader(archive(b)), Limits{})
	h, err := r.Next()
	if err != nil {
		t.Fatal(err)
	}
	data, err := io.ReadAll(r)
	if err != nil || h.Mode != 0120777 || string(data) != "../../lib/target" {
		t.Fatalf("%+v %q %v", h, data, err)
	}
}
