package amixtape

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"reflect"
	"strings"
	"testing"
)

func seg(k int) []byte { return bytes.Repeat([]byte(fmt.Sprintf("seg%02d.", k)), 200+37*k) }

var table = func() []Segment {
	var t []Segment
	for k := 0; k < 4; k++ {
		h := sha256.Sum256(seg(k))
		t = append(t, Segment{fmt.Sprintf("%02d", k), int64(len(seg(k))), hex.EncodeToString(h[:])})
	}
	return t
}()

func part(name string, b []byte) Part { return Part{name, bytes.NewReader(b), int64(len(b))} }

func fixture(s string) []byte {
	b, err := base64.StdEncoding.DecodeString(s)
	if err != nil {
		panic(err)
	}
	return b
}

func scan(t *testing.T, want []string, parts ...Part) *Result {
	t.Helper()
	r, err := Scan(context.Background(), table, parts, want, nil)
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func digest(b []byte) Digest {
	h := sha256.Sum256(b)
	return Digest{int64(len(b)), hex.EncodeToString(h[:])}
}

func TestTable(t *testing.T) {
	if len(Table) != 29 || Table[0].ID != "00" || Table[28].ID != "28" || Table[2].Size != 11183104 {
		t.Fatalf("table: %d segments", len(Table))
	}
}

func TestSplitBzip2Parts(t *testing.T) {
	p1, p2 := fixture(fixturePart1), fixture(fixturePart2)
	r := scan(t, nil, part("a.tar.bz2", p1), part("b.tar.bz2", p2))
	if m := r.Missing(table); len(m) != 0 {
		t.Fatalf("missing %v", m)
	}
	if r.Found["02"] != "b.tar.bz2:Tape/02" || !reflect.DeepEqual(r.Parts, []Digest{digest(p1), digest(p2)}) {
		t.Fatalf("%+v", r)
	}
	r = scan(t, []string{"01", "03"}, part("a.tar.bz2", p1), part("b.tar.bz2", p2))
	if !bytes.Equal(r.Data["01"], seg(1)) || !bytes.Equal(r.Data["03"], seg(3)) || len(r.Data) != 2 || r.Need([]string{"01", "03"}) != nil {
		t.Fatal("wanted segments not kept")
	}
	// Found early: the second part is hashed, not read.
	r = scan(t, []string{"00"}, part("a.tar.bz2", p1), part("b.tar.bz2", p2))
	if _, ok := r.Found["02"]; ok || r.Parts[1] != digest(p2) {
		t.Fatalf("%+v", r)
	}
}

func TestMissingSegment(t *testing.T) {
	r := scan(t, []string{"01", "02", "03"}, part("a.tar.bz2", fixture(fixturePart1)))
	err := r.Need([]string{"01", "02", "03"})
	if err == nil || !strings.Contains(err.Error(), "segments 02, 03 not found") {
		t.Fatal(err)
	}
	if m := r.Missing(table); !reflect.DeepEqual(m, []string{"02", "03"}) {
		t.Fatal(m)
	}
}

func TestDamagedSegment(t *testing.T) {
	r := scan(t, []string{"02"}, part("a.tar.bz2", fixture(fixturePart1)), part("b.tar.bz2", fixture(fixturePart2Bad)))
	err := r.Need([]string{"02"})
	if err == nil || !strings.Contains(err.Error(), "segment 02 is damaged (b.tar.bz2:Tape/02") {
		t.Fatal(err)
	}
	// A good copy later supersedes it.
	r = scan(t, []string{"02"}, part("b.tar.bz2", fixture(fixturePart2Bad)), part("c.tar.bz2", fixture(fixturePart2)))
	if r.Need([]string{"02"}) != nil || !bytes.Equal(r.Data["02"], seg(2)) {
		t.Fatal(r.Damaged)
	}
}

func TestTruncatedArchive(t *testing.T) {
	p := fixture(fixturePart2)
	_, err := Scan(context.Background(), table, []Part{part("b.tar.bz2", p[:len(p)/2])}, nil, nil)
	if err == nil || !strings.Contains(err.Error(), "tape part 1 (b.tar.bz2)") {
		t.Fatal(err)
	}
}

func tarball(gz bool, ids ...int) []byte {
	var b bytes.Buffer
	w := tar.NewWriter(&b)
	for _, k := range ids {
		w.WriteHeader(&tar.Header{Name: fmt.Sprintf("x/%02d", k), Mode: 0644, Size: int64(len(seg(k))), Typeflag: tar.TypeReg})
		w.Write(seg(k))
	}
	w.Close()
	if !gz {
		return b.Bytes()
	}
	var c bytes.Buffer
	z := gzip.NewWriter(&c)
	z.Write(b.Bytes())
	z.Close()
	return c.Bytes()
}

// tap writes a SIMH image: each segment in 1000-byte records, then a mark.
func tap(ids ...int) []byte {
	var b bytes.Buffer
	n := func(v uint32) { binary.Write(&b, binary.LittleEndian, v) }
	for _, k := range ids {
		for d := seg(k); len(d) != 0; {
			r := d[:min(len(d), 999)]
			d = d[len(r):]
			n(uint32(len(r)))
			b.Write(r)
			if len(r)&1 != 0 {
				b.WriteByte(0)
			}
			n(uint32(len(r)))
		}
		n(0)
	}
	n(0)
	n(0xffffffff)
	return b.Bytes()
}

func TestOtherForms(t *testing.T) {
	var z bytes.Buffer
	zw := zip.NewWriter(&z)
	for k := 0; k < 4; k++ {
		f, _ := zw.Create(fmt.Sprintf("%02d", k))
		f.Write(seg(k))
	}
	zw.Close()
	raw := append(append(append(seg(0), seg(1)...), seg(2)...), seg(3)...)
	for name, parts := range map[string][]Part{
		"tar":   {part("t.tar", tarball(false, 0, 1, 2, 3))},
		"gzip":  {part("t.tgz", tarball(true, 0, 1)), part("u.tgz", tarball(true, 2, 3))},
		"zip":   {part("t.zip", z.Bytes())},
		"tap":   {part("t.TAP", tap(0, 1, 2, 3))},
		"loose": {part("00", seg(0)), part("01", seg(1)), part("x/02", seg(2)), part("03", seg(3))},
		"raw":   {part("tape.img", raw)},
	} {
		r, err := Scan(context.Background(), table, parts, []string{"00", "01", "02", "03"}, nil)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		for k := 0; k < 4; k++ {
			if id := fmt.Sprintf("%02d", k); !bytes.Equal(r.Data[id], seg(k)) {
				t.Errorf("%s: segment %s not split", name, id)
			}
		}
	}
	bad := tap(0, 1)
	if _, err := Scan(context.Background(), table, []Part{part("t.tap", bad[:len(bad)-20])}, nil, nil); err == nil || !strings.Contains(err.Error(), "truncated SIMH") {
		t.Fatal(err)
	}
	if _, err := Scan(context.Background(), table, []Part{part("junk", []byte("junk"))}, nil, nil); err == nil {
		t.Fatal("junk accepted")
	}
}

func TestCancel(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := Scan(ctx, table, []Part{part("a.tar.bz2", fixture(fixturePart1))}, nil, nil); err == nil {
		t.Fatal("not cancelled")
	}
}
