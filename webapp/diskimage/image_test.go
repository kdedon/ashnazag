package diskimage

import (
	"bytes"
	"context"
	"errors"
	"io"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func bootFixture() []byte {
	b := make([]byte, 128*512)
	copy(b, "ER")
	be.PutUint16(b[2:], 512)
	be.PutUint32(b[4:], 128)
	for i, p := range []Partition{{"Apple", "Apple_partition_map", 1, 63}, {"Macintosh", "Apple_Driver", 64, 32}, {"MacOS", "Apple_HFS", 96, 32}} {
		e := entry(3, p, 0, 0)
		copy(b[(i+1)*512:], e[:])
	}
	for i := 64 * 512; i < len(b); i++ {
		b[i] = byte(i * 7)
	}
	return b
}
func rootFixture() []byte {
	b := make([]byte, 16384)
	sb := b[8192:]
	be.PutUint32(sb[1372:], 0x011954)
	be.PutUint32(sb[48:], 8192)
	be.PutUint32(sb[52:], 1024)
	be.PutUint32(sb[56:], 8)
	be.PutUint32(sb[36:], 16)
	be.PutUint32(sb[132:], 0x7c269d38)
	return b
}
func TestAssemble(t *testing.T) {
	b, r := bootFixture(), rootFixture()
	var out bytes.Buffer
	l, err := Assemble(context.Background(), &out, bytes.NewReader(b), int64(len(b)), bytes.NewReader(r), int64(len(r)), Options{SwapBytes: 1024, SpareBytes: []int64{512}})
	if err != nil {
		t.Fatal(err)
	}
	if l.SizeBytes != int64(len(b)+len(r)+1536) || int64(out.Len()) != l.SizeBytes || len(l.Partitions) != 6 {
		t.Fatal(l, out.Len())
	}
	got := out.Bytes()
	if !bytes.Equal(got[len(b):len(b)+len(r)], r) || !bytes.Equal(got[64*512:len(b)], b[64*512:]) {
		t.Fatal("payload changed")
	}
	if !bytes.Equal(got[len(b)+len(r):], make([]byte, 1536)) {
		t.Fatal("swap/spare not zero")
	}
	for i := 1; i <= 6; i++ {
		if be.Uint32(got[i*512+4:]) != 6 {
			t.Fatal("map count")
		}
	}
	if be.Uint32(got[4*512+0x88:]) != 0xABADBABE || be.Uint16(got[4*512+0x90:]) != 0xc000 || got[5*512+0x8d] != 3 {
		t.Fatal("bzb")
	}
}
func TestRejectBeforeWriting(t *testing.T) {
	tests := []struct {
		name     string
		mutate   func([]byte)
		rootSize int64
		opts     Options
	}{
		{"DDM", func(b []byte) { b[0] = 0 }, 16384, Options{SwapBytes: 512}},
		{"DDM size", func(b []byte) { be.PutUint32(b[4:], 129) }, 16384, Options{SwapBytes: 512}},
		{"map count", func(b []byte) { be.PutUint32(b[516:], 63) }, 16384, Options{SwapBytes: 512}},
		{"inconsistent count", func(b []byte) { be.PutUint32(b[1028:], 2) }, 16384, Options{SwapBytes: 512}},
		{"overlap", func(b []byte) { be.PutUint32(b[2*512+8:], 63) }, 16384, Options{SwapBytes: 512}},
		{"outside", func(b []byte) { be.PutUint32(b[3*512+12:], 100) }, 16384, Options{SwapBytes: 512}},
		{"map capacity", func(b []byte) { be.PutUint32(b[512+12:], 3); be.PutUint32(b[512+84:], 3) }, 16384, Options{SwapBytes: 512}},
		{"driver descriptors", func(b []byte) { be.PutUint16(b[16:], 62) }, 16384, Options{SwapBytes: 512}},
		{"driver range", func(b []byte) { be.PutUint16(b[16:], 1); be.PutUint32(b[18:], 128); be.PutUint16(b[22:], 1) }, 16384, Options{SwapBytes: 512}},
		{"partition data", func(b []byte) { be.PutUint32(b[3*512+84:], 33) }, 16384, Options{SwapBytes: 512}},
		{"root unaligned", nil, 16385, Options{SwapBytes: 512}},
		{"root overflow", nil, int64(math.MaxUint32) * 512, Options{SwapBytes: 512}},
		{"swap overflow", nil, 16384, Options{SwapBytes: math.MaxInt64 - 511}},
		{"zero swap", nil, 16384, Options{}},
		{"negative spare", nil, 16384, Options{SwapBytes: 512, SpareBytes: []int64{-512}}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			b := bootFixture()
			if tt.mutate != nil {
				tt.mutate(b)
			}
			var out bytes.Buffer
			_, err := Assemble(context.Background(), &out, bytes.NewReader(b), int64(len(b)), bytes.NewReader(rootFixture()), tt.rootSize, tt.opts)
			if err == nil || out.Len() != 0 {
				t.Fatalf("error=%v bytes=%d", err, out.Len())
			}
		})
	}
}
func TestRootValidation(t *testing.T) {
	for _, offset := range []int{1372, 48, 52, 56, 36, 132} {
		r := rootFixture()
		r[8192+offset] ^= 0xff
		if ValidateRoot(bytes.NewReader(r), int64(len(r))) == nil {
			t.Fatalf("accepted corrupt offset %d", offset)
		}
	}
}

type shortWriter struct{}

func (shortWriter) Write(b []byte) (int, error) { return len(b) - 1, nil }

type cancelWriter struct{ cancel context.CancelFunc }

func (w cancelWriter) Write(b []byte) (int, error) { w.cancel(); return len(b), nil }
func TestIOFailures(t *testing.T) {
	b, r := bootFixture(), rootFixture()
	run := func(ctx context.Context, w io.Writer, root io.ReaderAt) error {
		_, err := Assemble(ctx, w, bytes.NewReader(b), int64(len(b)), root, int64(len(r)), Options{SwapBytes: 512})
		return err
	}
	if err := run(context.Background(), shortWriter{}, bytes.NewReader(r)); !errors.Is(err, io.ErrShortWrite) {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	if err := run(ctx, cancelWriter{cancel}, bytes.NewReader(r)); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	if err := run(context.Background(), io.Discard, bytes.NewReader(r[:9000])); err == nil {
		t.Fatal("accepted truncated root")
	}
}
func TestPythonParity(t *testing.T) {
	python, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("python3 unavailable")
	}
	script := filepath.Join("..", "..", "kernel", "mac", "diskroot", "addparts.py")
	if _, err = os.Stat(script); err != nil {
		t.Skip("reference assembler unavailable")
	}
	dir := t.TempDir()
	bp, rp := filepath.Join(dir, "disk.img"), filepath.Join(dir, "root.img")
	b, r := bootFixture(), rootFixture()
	if err = os.WriteFile(bp, b, 0600); err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(rp, r, 0600); err != nil {
		t.Fatal(err)
	}
	if out, err := exec.Command(python, script, bp, rp, "1", "1").CombinedOutput(); err != nil {
		t.Fatalf("%v: %s", err, out)
	}
	want, err := os.ReadFile(bp)
	if err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	_, err = Assemble(context.Background(), &out, bytes.NewReader(b), int64(len(b)), bytes.NewReader(r), int64(len(r)), Options{SwapBytes: 1 << 20, SpareBytes: []int64{1 << 20}})
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(want, out.Bytes()) {
		t.Fatal("output differs from reference assembler")
	}
}

type boundedReader struct {
	io.ReaderAt
	max int
}

func (r *boundedReader) ReadAt(p []byte, offset int64) (int, error) {
	if len(p) > r.max {
		return 0, errors.New("unbounded read")
	}
	return r.ReaderAt.ReadAt(p, offset)
}

type boundedWriter struct{ size int64 }

func (w *boundedWriter) Write(p []byte) (int, error) {
	if len(p) > bufferSize {
		return 0, errors.New("unbounded write")
	}
	w.size += int64(len(p))
	return len(p), nil
}
func TestBoundedStreaming(t *testing.T) {
	b := bootFixture()
	r := make([]byte, 1<<20)
	copy(r, rootFixture())
	be.PutUint32(r[8192+36:], uint32(len(r)/1024))
	w := new(boundedWriter)
	l, err := Assemble(context.Background(), w, &boundedReader{bytes.NewReader(b), bufferSize}, int64(len(b)), &boundedReader{bytes.NewReader(r), bufferSize}, int64(len(r)), Options{SwapBytes: 2 << 20})
	if err != nil {
		t.Fatal(err)
	}
	if w.size != l.SizeBytes {
		t.Fatalf("size %d != %d", w.size, l.SizeBytes)
	}
}
