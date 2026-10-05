package amixtape

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/gzip"
	"context"
	"errors"
	"fmt"
	"math/rand"
	"runtime"
	"testing"
	"time"
)

// zeros streams n zero bytes.
type zeros struct{ n int64 }

func (z *zeros) Read(p []byte) (int, error) {
	if z.n <= 0 {
		return 0, errors.New("eof")
	}
	n := int(min(int64(len(p)), z.n))
	clear(p[:n])
	z.n -= int64(n)
	return n, nil
}

func TestCorruptionNoHangNoPanic(t *testing.T) {
	var zb bytes.Buffer
	zw := zip.NewWriter(&zb)
	for k := 0; k < 4; k++ {
		f, _ := zw.Create(fmt.Sprintf("%02d", k))
		f.Write(seg(k))
	}
	zw.Close()
	seeds := map[string][]byte{
		"a.tar.bz2": fixture(fixturePart1), "b.tar.bz2": fixture(fixturePart2),
		"t.tgz": tarball(true, 0, 1, 2, 3), "t.tar": tarball(false, 0, 1, 2, 3),
		"t.tap": tap(0, 1, 2, 3), "t.zip": zb.Bytes(),
		"raw": append(append(seg(0), seg(1)...), seg(2)...),
	}
	rng := rand.New(rand.NewSource(1))
	var before, after runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&before)
	start := time.Now()
	for name, seed := range seeds {
		for i := 0; i < 400; i++ {
			b := append([]byte(nil), seed...)
			for j := rng.Intn(8) + 1; j > 0; j-- {
				switch rng.Intn(4) {
				case 0:
					b[rng.Intn(len(b))] ^= byte(1 << rng.Intn(8))
				case 1:
					b[rng.Intn(len(b))] = byte(rng.Intn(256))
				case 2:
					b = b[:rng.Intn(len(b))+1]
				case 3:
					p := rng.Intn(len(b))
					copy(b[p:], []byte{0xff, 0xff, 0xff, 0xff, 0x7f, 0, 0, 0x80}[:min(8, len(b)-p)])
				}
			}
			func() {
				defer func() {
					if e := recover(); e != nil {
						t.Fatalf("%s #%d: panic %v", name, i, e)
					}
				}()
				ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
				defer cancel()
				Scan(ctx, table, []Part{part(name, b)}, nil, nil)
				if ctx.Err() != nil {
					t.Fatalf("%s #%d: hang", name, i)
				}
			}()
		}
	}
	runtime.GC()
	runtime.ReadMemStats(&after)
	if after.HeapAlloc > before.HeapAlloc+64<<20 {
		t.Fatalf("heap grew %d MiB", (after.HeapAlloc-before.HeapAlloc)>>20)
	}
	t.Logf("%v", time.Since(start))
}

func TestBombBounded(t *testing.T) {
	// A sparse-looking tar member far larger than the tape, gzipped.
	var c bytes.Buffer
	z, _ := gzip.NewWriterLevel(&c, gzip.BestCompression)
	tw := tar.NewWriter(z)
	const huge = 1 << 30
	tw.WriteHeader(&tar.Header{Name: "big", Mode: 0644, Size: huge, Typeflag: tar.TypeReg})
	buf := make([]byte, 1<<20)
	for i := 0; i < huge>>20; i++ {
		tw.Write(buf)
	}
	tw.Close()
	z.Close()
	var before, after runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&before)
	_, err := Scan(context.Background(), table, []Part{part("bomb.tgz", c.Bytes())}, nil, nil)
	runtime.ReadMemStats(&after)
	if !errors.Is(err, errExpands) {
		t.Fatalf("bomb not refused: %v", err)
	}
	if after.TotalAlloc-before.TotalAlloc > 512<<20 {
		t.Fatalf("allocated %d MiB", (after.TotalAlloc-before.TotalAlloc)>>20)
	}
}

func TestTapErrorRecord(t *testing.T) {
	var b bytes.Buffer
	w := func(v uint32) { b.Write([]byte{byte(v), byte(v >> 8), byte(v >> 16), byte(v >> 24)}) }
	w(0x80000000 | 5)
	b.Write([]byte("junk\x00\x00"))
	w(5)
	w(0xfffffffe)
	for d := seg(1); len(d) != 0; {
		r := d[:min(len(d), 998)]
		d = d[len(r):]
		w(uint32(len(r)))
		b.Write(r)
		w(uint32(len(r)))
	}
	w(0)
	w(0)
	w(0xffffffff)
	r, err := Scan(context.Background(), table, []Part{part("e.tap", b.Bytes())}, nil, nil)
	if err != nil || r.Found["01"] == "" {
		t.Fatalf("%v %+v", err, r)
	}
}
