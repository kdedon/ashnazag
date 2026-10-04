package ufs

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

type pattern struct{}

func (pattern) ReadAt(b []byte, off int64) (int, error) {
	for j := range b {
		b[j] = byte((off + int64(j)) * 31)
	}
	return len(b), nil
}
func TestIndependentCheckerAndReference(t *testing.T) {
	if _, err := exec.LookPath("python3"); err != nil {
		t.Skip("python3 unavailable")
	}
	dir := t.TempDir()
	large := int64(17*1024*1024 + 73)
	f, err := os.Create(filepath.Join(dir, "large"))
	if err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 8192)
	for off := int64(0); off < large; {
		n := int64(len(buf))
		if large-off < n {
			n = large - off
		}
		pattern{}.ReadAt(buf[:n], off)
		if _, err = f.Write(buf[:n]); err != nil {
			t.Fatal(err)
		}
		off += n
	}
	f.Close()
	payload, err := os.Open(filepath.Join(dir, "large"))
	if err != nil {
		t.Fatal(err)
	}
	defer payload.Close()
	entries := []Entry{{Path: "/etc", Kind: 'd', Mode: 0755}, {Path: "/etc/large", Kind: 'f', Mode: 0640, UID: 70000, GID: 80000, Size: large, Data: payload}, {Path: "/hard", Kind: 'h', Target: "/etc/large"}, {Path: "/link", Kind: 'l', Mode: 0777, Target: "etc/large"}, {Path: "/dev/null", Kind: 'c', Mode: 0666, Major: 2, Minor: 3}, {Path: "/dev/disk", Kind: 'b', Mode: 0600, Major: 4, Minor: 5}, {Path: "/pipe", Kind: 'p', Mode: 0600}, {Path: "/empty", Kind: 'f', Mode: 0644}, {Path: "/short", Kind: 'f', Mode: 0644, Size: 3, Data: bytes.NewReader([]byte("abc"))}}
	image := filepath.Join(dir, "go.img")
	out, err := os.Create(image)
	if err != nil {
		t.Fatal(err)
	}
	if err = out.Truncate(32 << 20); err != nil {
		t.Fatal(err)
	}
	_, err = Build(context.Background(), out, Options{SizeMiB: 32, Timestamp: 0x2b000000}, entries)
	out.Close()
	if err != nil {
		t.Fatal(err)
	}
	checker := filepath.Join("..", "..", "kernel", "mac", "diskroot", "ufscheck.py")
	if output, err := exec.Command("python3", checker, image, "-x", filepath.Join(dir, "extract")).CombinedOutput(); err != nil {
		t.Fatalf("independent check: %v\n%s", err, output)
	}
	extracted, err := os.ReadFile(filepath.Join(dir, "extract", "etc", "large"))
	if err != nil {
		t.Fatal(err)
	}
	if int64(len(extracted)) != large {
		t.Fatal("size mismatch")
	}
	for j, v := range extracted {
		if v != byte(j*31) {
			t.Fatalf("payload mismatch at %d", j)
		}
	}
	manifest := "d /etc 755 0 0\nf /etc/large 640 70000 80000 large\nh /hard /etc/large\nl /link etc/large\nc /dev/null 666 0 0 2 3\nb /dev/disk 600 0 0 4 5\np /pipe 600 0 0\ne /empty 644 0 0\nf /short 644 0 0 short\n"
	os.WriteFile(filepath.Join(dir, "manifest"), []byte(manifest), 0600)
	os.WriteFile(filepath.Join(dir, "short"), []byte("abc"), 0600)
	reference := filepath.Join(dir, "python.img")
	script := filepath.Join("..", "..", "kernel", "mac", "diskroot", "mkufs.py")
	if output, err := exec.Command("python3", script, "-s", "32", "-r", dir, filepath.Join(dir, "manifest"), reference).CombinedOutput(); err != nil {
		t.Fatalf("reference: %v\n%s", err, output)
	}
	a, _ := os.ReadFile(image)
	b, _ := os.ReadFile(reference)
	if !bytes.Equal(a, b) {
		for j := range a {
			if a[j] != b[j] {
				t.Fatalf("reference differs at byte %d: go=%x python=%x", j, a[j], b[j])
			}
		}
		t.Fatal("reference lengths differ")
	}
}

type discard struct{ writes int }

func (d *discard) WriteAt(b []byte, off int64) (int, error) { d.writes++; return len(b), nil }
func TestLargeSparseAndCancellation(t *testing.T) {
	d := &discard{}
	r, err := Build(context.Background(), d, Options{SizeMiB: 2048}, nil)
	if err != nil || r.Bytes != 2<<30 {
		t.Fatalf("large sparse build: %+v %v", r, err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err = Build(ctx, d, Options{SizeMiB: 4}, nil)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("cancel: %v", err)
	}
}
func TestInvalidAndShortReads(t *testing.T) {
	for _, entries := range [][]Entry{{{Path: "/../escape", Kind: 'f'}}, {{Path: "/x", Kind: 'h', Target: "/missing"}}, {{Path: "/x", Kind: 'f', Size: 8, Data: bytes.NewReader(nil)}}, {{Path: "/x", Kind: 'f'}, {Path: "/x/y", Kind: 'f'}}} {
		if _, err := Build(context.Background(), &discard{}, Options{SizeMiB: 4}, entries); err == nil {
			t.Fatalf("accepted %+v", entries)
		}
	}
	if _, err := Build(context.Background(), &discard{}, Options{SizeMiB: 3}, nil); err == nil {
		t.Fatal("accepted invalid geometry")
	}
	_, err := Build(context.Background(), &discard{}, Options{SizeMiB: 4}, []Entry{{Path: "/big", Kind: 'f', Size: 5 << 20, Data: pattern{}}})
	if err == nil || errors.Is(err, io.EOF) {
		t.Fatalf("space error: %v", err)
	}
}

func TestManyInodesAndDirectoryBlocks(t *testing.T) {
	if _, err := exec.LookPath("python3"); err != nil {
		t.Skip("python3 unavailable")
	}
	var entries []Entry
	for n := 0; n < 1100; n++ {
		entries = append(entries, Entry{Path: fmt.Sprintf("/many/file-%04d", n), Kind: 'f', Mode: 0644, Size: 1, Data: bytes.NewReader([]byte{byte(n)})})
	}
	name := filepath.Join(t.TempDir(), "many.img")
	f, err := os.Create(name)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	if err = f.Truncate(8 << 20); err != nil {
		t.Fatal(err)
	}
	r, err := Build(context.Background(), f, Options{SizeMiB: 8}, entries)
	if err != nil {
		t.Fatal(err)
	}
	if r.Inodes != 1103 {
		t.Fatalf("inodes: %d", r.Inodes)
	}
	if output, err := exec.Command("python3", filepath.Join("..", "..", "kernel", "mac", "diskroot", "ufscheck.py"), name).CombinedOutput(); err != nil {
		t.Fatalf("independent check: %v\n%s", err, output)
	}
}

type shortWriter struct{}

func (shortWriter) WriteAt(b []byte, off int64) (int, error) { return len(b) - 1, nil }
func TestShortWrite(t *testing.T) {
	_, err := Build(context.Background(), shortWriter{}, Options{SizeMiB: 4}, nil)
	if !errors.Is(err, io.ErrShortWrite) {
		t.Fatalf("short write: %v", err)
	}
}
