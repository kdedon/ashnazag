package sources

import (
	"bytes"
	"context"
	"io"
	"os"
	"testing"
)

func TestHFSGeneratedBootVolume(t *testing.T) {
	p := "/tmp/ash-hfs/boot.img"
	f, e := os.Open(p)
	if e != nil {
		t.Skip("local boot fixture unavailable")
	}
	defer f.Close()
	st, _ := f.Stat()
	out, e := Read(context.Background(), "hfs", f, st.Size(), Limits{})
	if e != nil {
		t.Fatal(e)
	}
	found := false
	for _, x := range out.Entries {
		if x.Path == "/unix" {
			found = true
			if x.Size == 0 {
				t.Fatal("empty unix")
			}
			if expected, err := os.ReadFile("/tmp/ash-hfs/expected.bin"); err == nil {
				actual, _ := io.ReadAll(io.NewSectionReader(x.Data, 0, x.Size))
				if !bytes.Equal(actual, expected) {
					t.Fatal("boot unix differs from independent expected payload")
				}
			}
		}
	}
	if !found {
		t.Fatal("unix absent")
	}
}
func hSynthetic() []byte {
	b := make([]byte, 64*512)
	m := b[1024:1536]
	hbe.PutUint16(m, 0x4244)
	hbe.PutUint16(m[18:], 56)
	hbe.PutUint16(m[14:], 3)
	for i := 1536; i < 1543; i++ {
		b[i] = 255
	}
	hbe.PutUint32(m[20:], 512)
	hbe.PutUint16(m[28:], 4)
	hbe.PutUint32(m[130:], 512)
	hbe.PutUint16(m[136:], 1)
	hbe.PutUint32(m[146:], 1024)
	hbe.PutUint16(m[150:], 1)
	hbe.PutUint16(m[152:], 2)
	node := func(b []byte, records [][]byte, kind byte) {
		b[8] = kind
		if kind == 255 {
			b[9] = 1
		}
		hbe.PutUint16(b[10:], uint16(len(records)))
		off := 14
		for i, r := range records {
			hbe.PutUint16(b[510-i*2:], uint16(off))
			copy(b[off:], r)
			off += len(r)
		}
		hbe.PutUint16(b[510-len(records)*2:], uint16(off))
	}
	header := func(first, count, total uint32) []byte {
		h := make([]byte, 106)
		hbe.PutUint32(h[6:], count)
		hbe.PutUint32(h[10:], first)
		hbe.PutUint32(h[14:], first)
		hbe.PutUint16(h[18:], 512)
		hbe.PutUint32(h[22:], total)
		return h
	}
	node(b[2048:2560], [][]byte{header(0, 0, 1), make([]byte, 128), make([]byte, 32)}, 1)
	node(b[2560:3072], [][]byte{header(1, 2, 2), make([]byte, 128), make([]byte, 32)}, 1)
	record := func(parent uint32, name string, data []byte) []byte {
		k := make([]byte, (8+len(name)+1)&^1)
		k[0] = byte(7 + len(name))
		hbe.PutUint32(k[2:], parent)
		k[6] = byte(len(name))
		copy(k[7:], name)
		return append(k, data...)
	}
	root := make([]byte, 70)
	root[0] = 1
	hbe.PutUint32(root[6:], 2)
	f := make([]byte, 102)
	f[0] = 2
	copy(f[4:20], "TEXTttxt01234567")
	hbe.PutUint32(f[20:], 16)
	hbe.PutUint32(f[26:], 4)
	hbe.PutUint32(f[36:], 3)
	copy(f[56:72], "abcdefghijklmnop")
	hbe.PutUint16(f[74:], 3)
	hbe.PutUint16(f[76:], 1)
	hbe.PutUint16(f[86:], 4)
	hbe.PutUint16(f[88:], 1)
	node(b[3072:3584], [][]byte{record(1, "Test", root), record(2, "File", f)}, 255)
	copy(b[3584:], "data")
	copy(b[4096:], "res")
	return b
}
func TestHFSResourceFork(t *testing.T) {
	b := hSynthetic()
	out, e := Read(context.Background(), "hfs", bytes.NewReader(b), int64(len(b)), Limits{})
	if e != nil {
		t.Fatal(e)
	}
	if len(out.Entries) != 2 {
		t.Fatal(len(out.Entries))
	}
	for _, x := range out.Entries {
		data, _ := io.ReadAll(io.NewSectionReader(x.Data, 0, x.Size))
		switch x.Path {
		case "/File":
			if string(data) != "data" {
				t.Fatal(string(data))
			}
		case "/%File":
			if len(data) != 515 || string(data[512:]) != "res" || string(data[224:240]) != "TEXTttxt01234567" || string(data[240:256]) != "abcdefghijklmnop" {
				t.Fatalf("bad AppleDouble %x", data)
			}
		default:
			t.Fatal(x.Path)
		}
	}
}
func TestHFSRejectsCorruption(t *testing.T) {
	for _, change := range []func([]byte){func(b []byte) { hbe.PutUint32(b[3072:], 1) }, func(b []byte) { hbe.PutUint16(b[3072+510:], 511) }, func(b []byte) { hbe.PutUint16(b[1024+150:], 0) }, func(b []byte) { hbe.PutUint32(b[1024+20:], 0xffffffff) }} {
		b := hSynthetic()
		change(b)
		if _, e := Read(context.Background(), "hfs", bytes.NewReader(b), int64(len(b)), Limits{}); e == nil {
			t.Fatal("corruption accepted")
		}
	}
	b := hSynthetic()
	if _, e := Read(context.Background(), "hfs", bytes.NewReader(b), int64(len(b)), Limits{ExpandedBytes: 512}); e == nil {
		t.Fatal("limit ignored")
	}
}

func TestHFSOverflowExtents(t *testing.T) {
	b := hSynthetic()
	m := b[1024:1536]
	hbe.PutUint32(m[130:], 1024)
	hbe.PutUint16(m[138:], 5)
	hbe.PutUint16(m[140:], 1)
	h := b[2048+14:]
	hbe.PutUint32(h[6:], 1)
	hbe.PutUint32(h[10:], 1)
	hbe.PutUint32(h[14:], 1)
	hbe.PutUint32(h[22:], 2)
	node := b[2048+5*512 : 2048+6*512]
	node[8] = 255
	node[9] = 1
	hbe.PutUint16(node[10:], 1)
	hbe.PutUint16(node[510:], 14)
	hbe.PutUint16(node[508:], 34)
	rec := node[14:34]
	rec[0] = 7
	hbe.PutUint32(rec[2:], 16)
	hbe.PutUint16(rec[6:], 3)
	hbe.PutUint16(rec[8:], 10)
	hbe.PutUint16(rec[10:], 1)
	records, e := hRecords(b[3072:3584])
	if e != nil {
		t.Fatal(e)
	}
	raw := records[1]
	f := raw[(int(raw[0])+2)&^1:]
	hbe.PutUint32(f[26:], 1539)
	hbe.PutUint16(f[78:], 6)
	hbe.PutUint16(f[80:], 1)
	hbe.PutUint16(f[82:], 8)
	hbe.PutUint16(f[84:], 1)
	copy(b[2048+10*512:], "end")
	out, e := Read(context.Background(), "hfs", bytes.NewReader(b), int64(len(b)), Limits{})
	if e != nil {
		t.Fatal(e)
	}
	for _, x := range out.Entries {
		if x.Path == "/File" {
			data, _ := io.ReadAll(io.NewSectionReader(x.Data, 0, x.Size))
			if len(data) != 1539 || string(data[1536:]) != "end" {
				t.Fatal("overflow payload missing")
			}
		}
	}
}

func TestHFSIndependentVolume(t *testing.T) {
	f, e := os.Open("/tmp/ash-hfs/vol")
	if e != nil {
		t.Skip("local hfsutils fixture unavailable")
	}
	defer f.Close()
	st, _ := f.Stat()
	out, e := Read(context.Background(), "hfs", f, st.Size(), Limits{})
	if e != nil {
		t.Fatal(e)
	}
	for _, x := range out.Entries {
		if x.Path == "/unix" {
			return
		}
	}
	t.Fatal("unix absent")
}
