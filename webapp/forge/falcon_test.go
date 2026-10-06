package forge

import (
	"bytes"
	"context"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// sink keeps the head of the disk and one range, and drops the rest.
type sink struct {
	pos, from, to int64
	head          []byte
	part          *os.File
}

func (s *sink) Write(b []byte) (int, error) {
	n := len(b)
	if s.pos < 8192*512 {
		s.head = append(s.head, b[:min(len(b), 8192*512-int(s.pos))]...)
	}
	if lo, hi := max(s.pos, s.from), min(s.pos+int64(n), s.to); lo < hi {
		if _, err := s.part.Write(b[lo-s.pos : hi-s.pos]); err != nil {
			return 0, err
		}
	}
	s.pos += int64(n)
	return n, nil
}

func scratch(t *testing.T, mib int) *os.File {
	f, err := os.Create(filepath.Join(t.TempDir(), "fs"))
	if err != nil {
		t.Fatal(err)
	}
	if err = f.Truncate(int64(mib) << 20); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { f.Close() })
	return f
}

func falconSelection() Selection {
	p, _ := Lookup("falcon030")
	s := Selection{Preset: p.ID, Machine: p.Machine, Settings: p.Settings}
	s.Settings.RootMiB, s.Settings.SwapMiB = 64, 4
	return s
}

func TestFalconBuild(t *testing.T) {
	files := fixtureMedia()[:4]
	s := falconSelection()
	if err := s.Runnable(); err != nil {
		t.Fatal(err)
	}
	if r := Roles(s); len(r) != 2 || r[1] != "kernel" {
		t.Fatalf("roles %v", r)
	}
	r, err := NewRecipe(s, digests(t, s, files))
	if err != nil {
		t.Fatal(err)
	}
	rootMiB, homeMiB, err := FalconSizes(s, int64(len(files[3])))
	if err != nil || rootMiB != 64 || homeMiB != 436 {
		t.Fatalf("sizes %d %d %v", rootMiB, homeMiB, err)
	}
	// AXB is 4 MiB at sector 64; root follows it.
	rootAt := int64(64+4*2048) * 512
	part, err := os.Create(filepath.Join(t.TempDir(), "root.img"))
	if err != nil {
		t.Fatal(err)
	}
	defer part.Close()
	out := &sink{from: rootAt, to: rootAt + 64<<20, part: part}
	if err = Falcon(context.Background(), r, media(files), scratch(t, rootMiB), scratch(t, homeMiB), out); err != nil {
		t.Fatal(err)
	}
	if out.pos != 512<<20 {
		t.Fatalf("disk is %d bytes", out.pos)
	}
	h := out.head
	if sum := func() (n uint16) {
		for i := 0; i < 512; i += 2 {
			n += binary.BigEndian.Uint16(h[i:])
		}
		return
	}(); sum != 0x1234 {
		t.Fatalf("root sector sum %#x", sum)
	}
	for i, want := range []struct {
		id       string
		start, n uint32
	}{{"AXB", 64, 8192}, {"AXR", 8256, 131072}, {"AXS", 139328, 8192}, {"AXU", 147520, 892928}} {
		e := h[0x1C6+12*i:]
		if e[0] != 1 || string(e[1:4]) != want.id || binary.BigEndian.Uint32(e[4:]) != want.start || binary.BigEndian.Uint32(e[8:]) != want.n {
			t.Errorf("partition %d: % x", i, e[:12])
		}
	}
	if !bytes.Equal(h[64*512+16*512:][:len(files[3])], files[3]) || string(h[64*512+15*512:][:12]) != FalconCmdLine+"\x00" {
		t.Error("kernel or command line is not in AXB")
	}
	// Round trip: the root slice reads back through the independent checker.
	checker := filepath.Join("..", "..", "kernel", "mac", "diskroot", "ufscheck.py")
	if _, err := os.Stat(checker); err != nil {
		t.Skip("checker unavailable")
	}
	dir := filepath.Join(t.TempDir(), "x")
	if b, err := exec.Command("python3", checker, part.Name(), "-x", dir).CombinedOutput(); err != nil {
		t.Fatalf("root filesystem check: %v\n%s", err, b)
	}
	for p, want := range map[string]string{
		"etc/TIMEZONE": "TZ=CST6CDT\nexport TZ\n",
		"etc/nodename": "falcon\n",
		"etc/vfstab":   "/dev/dsk/c0d0s1\t/dev/rdsk/c0d0s1\t/\tufs\t1\tno\t-\nproc\t-\t/proc\tproc\t0\tno\t-\nfd\t-\t/dev/fd\tfd\t0\tno\t-\n/dev/dsk/c0d0s3\t/dev/rdsk/c0d0s3\t/home\tufs\t2\tyes\t-\n",
	} {
		got, err := os.ReadFile(filepath.Join(dir, p))
		if err != nil || string(got) != want {
			t.Errorf("%s: %q %v", p, got, err)
		}
	}
	if got, err := os.ReadFile(filepath.Join(dir, "stand/unix")); err != nil || !bytes.Equal(got, files[3]) {
		t.Errorf("/stand/unix: %v", err)
	}
}

func TestFalconRefusals(t *testing.T) {
	files := fixtureMedia()[:4]
	s := falconSelection()
	r, err := NewRecipe(s, digests(t, s, files))
	if err != nil {
		t.Fatal(err)
	}
	if err = Quadra(context.Background(), r, media(files), nil, &sink{}); err == nil {
		t.Error("Quadra built a Falcon recipe")
	}
	s.Settings.RootMiB = 62
	if _, err = NewRecipe(s, nil); err == nil {
		t.Error("root of 62 MiB accepted")
	}
	s = falconSelection()
	s.Settings.RootMiB, s.Settings.SwapMiB = 2048, 2048
	r, _ = NewRecipe(s, digests(t, s, files))
	if _, _, err = FalconSizes(s, 512); err == nil {
		t.Error("oversized root and swap accepted")
	}
}
