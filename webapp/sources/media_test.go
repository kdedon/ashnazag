package sources

import (
	"bytes"
	"context"
	"encoding/binary"
	"io"
	"os"
	"os/exec"
	"strings"
	"testing"
)

func lhaStored(name string, data []byte) []byte {
	h := make([]byte, 24+len(name))
	h[0] = byte(len(h) - 2)
	copy(h[2:], "-lh0-")
	binary.LittleEndian.PutUint32(h[7:], uint32(len(data)))
	binary.LittleEndian.PutUint32(h[11:], uint32(len(data)))
	binary.LittleEndian.PutUint32(h[15:], uint32(1<<21|1<<16))
	h[21] = byte(len(name))
	copy(h[22:], name)
	binary.LittleEndian.PutUint16(h[22+len(name):], crc(data))
	for _, b := range h[2:] {
		h[1] += b
	}
	return append(append(h, data...), 0)
}
func importBytes(format string, b []byte, l Limits) (Result, error) {
	return Read(context.Background(), format, bytes.NewReader(b), int64(len(b)), l)
}
func TestLHAStoredAndHeaders(t *testing.T) {
	for _, level := range []byte{0, 1, 2} {
		var b []byte
		if level == 0 {
			b = lhaStored("dir\\file", []byte("data"))
		} else {
			ext := []byte{1, 'f', 'i', 'l', 'e', 0, 0}
			h := make([]byte, 27)
			if level == 2 {
				h = make([]byte, 26)
			}
			copy(h[2:], "-lh0-")
			binary.LittleEndian.PutUint32(h[7:], 4)
			binary.LittleEndian.PutUint32(h[11:], 4)
			h[20] = level
			if level == 1 {
				h[0] = 25
				binary.LittleEndian.PutUint32(h[7:], 11)
				binary.LittleEndian.PutUint32(h[15:], 1<<21|1<<16)
				binary.LittleEndian.PutUint16(h[22:], crc([]byte("data")))
				h[24] = 'U'
				binary.LittleEndian.PutUint16(h[25:], 7)
				for _, v := range h[2:] {
					h[1] += v
				}
			} else {
				binary.LittleEndian.PutUint16(h, uint16(len(h)+len(ext)))
				binary.LittleEndian.PutUint16(h[21:], crc([]byte("data")))
				h[23] = 'U'
				binary.LittleEndian.PutUint16(h[24:], 7)
			}
			b = append(append(append(h, ext...), []byte("data")...), 0)
		}
		out, e := importBytes("lha", b, Limits{})
		if e != nil {
			t.Fatalf("level%d: %v", level, e)
		}
		if len(out.Entries) != 1 {
			t.Fatal(len(out.Entries))
		}
		x := out.Entries[0]
		got, _ := io.ReadAll(io.NewSectionReader(x.Data, 0, x.Size))
		if string(got) != "data" || x.ModTime == nil {
			t.Fatal(x)
		}
	}
}
func TestLHABadSources(t *testing.T) {
	for _, name := range []string{"../escape", "dir/../escape", "/absolute", "dir\\..\\escape", "", "dir//file"} {
		if _, e := importBytes("lha", lhaStored(name, []byte("data")), Limits{}); e == nil {
			t.Fatal(name)
		}
	}
	good := lhaStored("file", []byte("data"))
	for i := 1; i < len(good); i++ {
		if _, e := importBytes("lha", good[:i], Limits{}); e == nil {
			t.Fatalf("accepted truncation %d", i)
		}
	}
	bad := append([]byte(nil), good...)
	bad[len(bad)-2] ^= 1
	if _, e := importBytes("lha", bad, Limits{}); e == nil {
		t.Fatal("CRC ignored")
	}
	if _, e := importBytes("lha", append(good, 1), Limits{}); e == nil {
		t.Fatal("trailing data ignored")
	}
	twice := append(append([]byte(nil), good[:len(good)-1]...), good...)
	if _, e := importBytes("lha", twice, Limits{}); e == nil {
		t.Fatal("duplicate accepted")
	}
	if _, e := importBytes("lha", good, Limits{ExpandedBytes: 3}); e == nil {
		t.Fatal("expanded limit ignored")
	}
}
func TestLH5ConstantSymbol(t *testing.T) {
	var bits []byte
	put := func(v, n int) {
		for i := n - 1; i >= 0; i-- {
			bits = append(bits, byte(v>>i&1))
		}
	}
	put(1, 16)
	put(0, 5)
	put(0, 5)
	put(0, 9)
	put(65, 9)
	put(0, 4)
	put(0, 4)
	payload := make([]byte, (len(bits)+7)/8)
	for i, b := range bits {
		payload[i/8] |= b << uint(7-i%8)
	}
	b := lhaStored("A", []byte("A"))
	h := append([]byte(nil), b[:int(b[0])+2]...)
	copy(h[2:], "-lh5-")
	binary.LittleEndian.PutUint32(h[7:], uint32(len(payload)))
	h[1] = 0
	for _, v := range h[2:] {
		h[1] += v
	}
	b = append(append(h, payload...), 0)
	out, e := importBytes("lha", b, Limits{})
	if e != nil {
		t.Fatal(e)
	}
	got := make([]byte, 1)
	out.Entries[0].Data.ReadAt(got, 0)
	if string(got) != "A" {
		t.Fatal(got)
	}
}
func TestLocalPicassoMedia(t *testing.T) {
	b, e := os.ReadFile("../../Picasso96.lha")
	if e != nil {
		t.Skip(e)
	}
	if _, e := importBytes("lha", b, Limits{}); e == nil || !strings.Contains(e.Error(), "path") {
		t.Fatalf("nameless archive entries must fail: %v", e)
	}
	var pos int
	for pos < len(b) && b[pos] != 0 {
		h := b[pos : pos+int(b[pos])+2]
		if h[21] == 0 {
			break
		}
		pos += len(h) + int(binary.LittleEndian.Uint32(h[7:]))
	}
	prefix := append(append([]byte(nil), b[:pos]...), 0)
	out, e := importBytes("lha", prefix, Limits{})
	if e != nil {
		t.Fatal(e)
	}
	if len(out.Entries) != 98 {
		t.Fatalf("entries=%d", len(out.Entries))
	}
	cmdPath, e := exec.LookPath("7z")
	if e != nil {
		t.Skip("7z unavailable")
	}
	x := out.Entries[0]
	cmd := exec.Command(cmdPath, "x", "-so", "../../Picasso96.lha", strings.TrimPrefix(x.Path, "/"))
	want, e := cmd.Output()
	if e != nil {
		t.Fatal(e)
	}
	got, _ := io.ReadAll(io.NewSectionReader(x.Data, 0, x.Size))
	if !bytes.Equal(want, got) {
		t.Fatal("7z output differs")
	}
}
func adfChecksum(b []byte) {
	binary.BigEndian.PutUint32(b[20:], 0)
	var sum uint32
	for i := 0; i < 512; i += 4 {
		sum += binary.BigEndian.Uint32(b[i:])
	}
	binary.BigEndian.PutUint32(b[20:], 0-sum)
}
func adfFixture(ffs bool) []byte {
	b := make([]byte, 901120)
	copy(b, "DOS")
	if ffs {
		b[3] = 1
	}
	root := b[880*512 : 881*512]
	put := func(b []byte, i int, v uint32) { binary.BigEndian.PutUint32(b[4*i:], v) }
	put(root, 0, 2)
	put(root, 3, 72)
	put(root, 6, 3)
	put(root, 127, 1)
	adfChecksum(root)
	f := b[3*512 : 4*512]
	put(f, 0, 2)
	put(f, 1, 3)
	put(f, 2, 1)
	put(f, 4, 4)
	put(f, 77, 4)
	put(f, 81, 4)
	put(f, 125, 880)
	put(f, 127, 0xfffffffd)
	f[432] = 4
	copy(f[433:], "file")
	adfChecksum(f)
	data := b[4*512 : 5*512]
	if ffs {
		copy(data, "data")
	} else {
		put(data, 0, 8)
		put(data, 1, 3)
		put(data, 2, 1)
		put(data, 3, 4)
		copy(data[24:], "data")
		adfChecksum(data)
	}
	return b
}
func TestADFVariants(t *testing.T) {
	for _, ffs := range []bool{false, true} {
		out, e := importBytes("adf", adfFixture(ffs), Limits{})
		if e != nil {
			t.Fatal(e)
		}
		if len(out.Entries) != 1 || out.Entries[0].Path != "/file" {
			t.Fatal(out)
		}
		x := out.Entries[0]
		data, _ := io.ReadAll(io.NewSectionReader(x.Data, 0, x.Size))
		if string(data) != "data" || x.ModTime == nil || *x.ModTime != 252460800 {
			t.Fatal(x)
		}
	}
}
func TestADFRejects(t *testing.T) {
	for _, change := range []func([]byte){func(b []byte) { b[3*512+433] = '/' }, func(b []byte) { binary.BigEndian.PutUint32(b[3*512+124*4:], 3) }, func(b []byte) { binary.BigEndian.PutUint32(b[3*512+77*4:], 3) }, func(b []byte) { binary.BigEndian.PutUint32(b[3*512+81*4:], 1) }, func(b []byte) { binary.BigEndian.PutUint32(b[3*512+106*4:], 1440) }} {
		b := adfFixture(false)
		change(b)
		adfChecksum(b[3*512 : 4*512])
		if _, e := importBytes("adf", b, Limits{}); e == nil {
			t.Fatal("accepted corrupt ADF")
		}
	}
	b := adfFixture(true)
	b[3*512] ^= 1
	if _, e := importBytes("adf", b, Limits{}); e == nil {
		t.Fatal("checksum ignored")
	}
}
