package rootfs

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"strings"
	"testing"

	"amigaux.org/imagebuilder/ufs"
)

type fixture struct {
	name, body         string
	mode, inode, nlink uint32
}

func cpio(fs ...fixture) []byte {
	var out bytes.Buffer
	fs = append(fs, fixture{name: "TRAILER!!!"})
	for _, f := range fs {
		var sum uint32
		for _, b := range []byte(f.body) {
			sum += uint32(b)
		}
		out.WriteString("070702")
		fields := []uint32{f.inode, f.mode, 42, 43, f.nlink, 12345, uint32(len(f.body)), 1, 2, 3, 4, uint32(len(f.name) + 1), sum}
		for _, n := range fields {
			fmt.Fprintf(&out, "%08x", n)
		}
		out.WriteString(f.name)
		out.WriteByte(0)
		for out.Len()%4 != 0 {
			out.WriteByte(0)
		}
		out.WriteString(f.body)
		for out.Len()%4 != 0 {
			out.WriteByte(0)
		}
	}
	return out.Bytes()
}
func scan(data []byte, opts Options) ([]ufs.Entry, error) {
	return Scan(context.Background(), bytes.NewReader(data), int64(len(data)), opts)
}
func TestMetadataAndDeferredPayload(t *testing.T) {
	data := cpio(fixture{".", "", 0040755, 1, 2}, fixture{"bin/a", "", 0104755, 7, 2}, fixture{"bin/b", "payload", 0104755, 7, 2}, fixture{"dev/tty", "", 0020620, 8, 1}, fixture{"link", "../bin/a", 0120777, 9, 1}, fixture{"pipe", "", 0010644, 10, 1})
	entries, err := scan(data, Options{})
	if err != nil {
		t.Fatal(err)
	}
	byName := map[string]ufs.Entry{}
	for _, e := range entries {
		byName[e.Path] = e
	}
	a, b := byName["/bin/a"], byName["/bin/b"]
	if a.Mode != 04755 || a.UID != 42 || a.GID != 43 || *a.ModTime != 12345 || a.Size != 7 || b.Kind != 'h' || b.Target != "/bin/a" {
		t.Fatalf("metadata: %+v %+v", a, b)
	}
	body, err := io.ReadAll(io.NewSectionReader(a.Data, 0, a.Size))
	if err != nil || string(body) != "payload" {
		t.Fatalf("body %q: %v", body, err)
	}
	if e := byName["/dev/tty"]; e.Kind != 'c' || e.Major != 3 || e.Minor != 4 {
		t.Fatalf("device: %+v", e)
	}
	if e := byName["/link"]; e.Target != "../bin/a" || e.Kind != 'l' {
		t.Fatalf("link: %+v", e)
	}
}
func TestRejectInvalidTrees(t *testing.T) {
	tests := []struct {
		name  string
		files []fixture
		want  string
	}{
		{"duplicate", []fixture{{"a", "x", 0100644, 1, 1}, {"./a", "y", 0100644, 2, 1}}, "duplicate"},
		{"parent", []fixture{{"a", "x", 0100644, 1, 1}, {"a/b", "y", 0100644, 2, 1}}, "not a directory"},
		{"root", []fixture{{".", "", 0100644, 1, 1}}, "root"},
		{"device body", []fixture{{"dev/x", "bad", 0020600, 1, 1}}, "unexpected payload"},
		{"conflict", []fixture{{"a", "x", 0100644, 1, 2}, {"b", "y", 0100644, 1, 2}}, "conflicting hardlink payload"},
		{"link metadata", []fixture{{"a", "x", 0100644, 1, 2}, {"b", "", 0100600, 1, 2}}, "conflicting hardlink metadata"},
		{"nul link", []fixture{{"a", "x\x00y", 0120777, 1, 1}}, "NUL"},
		{"socket", []fixture{{"a", "", 0140600, 1, 1}}, "unsupported"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := scan(cpio(tt.files...), Options{})
			if err == nil || !strings.Contains(err.Error(), tt.want) {
				t.Fatalf("want %s: %v", tt.want, err)
			}
		})
	}
}
func TestBoundsCancellationAndChecksum(t *testing.T) {
	data := cpio(fixture{"a", strings.Repeat("x", 1<<20), 0100644, 1, 1})
	if _, err := scan(data, Options{MaxMetadataBytes: 32}); err == nil || !strings.Contains(err.Error(), "metadata") {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := Scan(ctx, bytes.NewReader(data), int64(len(data)), Options{}); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	data[112] ^= 1
	if _, err := scan(data, Options{}); err == nil || !strings.Contains(err.Error(), "checksum") {
		t.Fatal(err)
	}
}

type boundedReaderAt struct {
	*bytes.Reader
	maxRead int
}

func (r *boundedReaderAt) ReadAt(p []byte, offset int64) (int, error) {
	if len(p) > r.maxRead {
		return 0, fmt.Errorf("oversized read: %d", len(p))
	}
	return r.Reader.ReadAt(p, offset)
}
func TestLargePayloadUsesBoundedReads(t *testing.T) {
	data := cpio(fixture{"big", strings.Repeat("x", 2<<20), 0100644, 1, 1})
	source := &boundedReaderAt{bytes.NewReader(data), 64 << 10}
	entries, err := Scan(context.Background(), source, int64(len(data)), Options{})
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 || entries[0].Size != 2<<20 {
		t.Fatalf("entries: %+v", entries)
	}
	// Changing the retained source changes the referenced payload.
	offset := bytes.Index(data, []byte(strings.Repeat("x", 32)))
	data[offset] = 'z'
	var b [1]byte
	if _, err := entries[0].Data.ReadAt(b[:], 0); err != nil || b[0] != 'z' {
		t.Fatalf("payload reference: %q %v", b, err)
	}
}
