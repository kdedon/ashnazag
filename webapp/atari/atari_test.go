package atari

import (
	"bytes"
	"context"
	"os"
	"testing"
)

// zeros reads as an empty filesystem.
type zeros struct{}

func (zeros) ReadAt(b []byte, off int64) (int, error) { clear(b); return len(b), nil }

// head keeps the first bytes written and counts the rest.
type head struct {
	buf []byte
	n   int64
}

func (h *head) Write(b []byte) (int, error) {
	if room := 81*512 - len(h.buf); room > 0 {
		h.buf = append(h.buf, b[:min(room, len(b))]...)
	}
	h.n += int64(len(b))
	return len(b), nil
}

func fixture(t *testing.T, name string) []byte {
	b, err := os.ReadFile("testdata/" + name)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// The fixtures are the root sector and boot partition head that mkdisk.sh
// and instboot.py produce for a 512 MiB disk with a 128 MiB root, 64 MiB
// swap and the test kernel.
func TestLayoutMatchesMkdisk(t *testing.T) {
	kernel := fixture(t, "kernel.elf")
	axb, home, err := Sizes(512, len(kernel), 128, 64)
	if err != nil || axb != 4 || home != 312 {
		t.Fatalf("sizes %d %d %v", axb, home, err)
	}
	if got := RootSector(512, axb, 128, 64, home); !bytes.Equal(got, fixture(t, "rootsec.bin")) {
		t.Fatalf("root sector differs:\n%x\n%x", got[0x1C0:], fixture(t, "rootsec.bin")[0x1C0:])
	}
	boot, err := Boot(kernel, "root=c0d0s1")
	if err != nil || !bytes.Equal(boot, fixture(t, "axb.bin")) {
		t.Fatalf("boot partition differs: %v", err)
	}
	var h head
	err = Assemble(context.Background(), &h, Params{DiskMiB: 512, SwapMiB: 64, Kernel: kernel, CmdLine: "root=c0d0s1",
		Root: zeros{}, RootMiB: 128, Home: zeros{}, HomeMiB: home})
	if err != nil {
		t.Fatal(err)
	}
	want := append(append(fixture(t, "rootsec.bin"), make([]byte, 63*512)...), fixture(t, "axb.bin")...)
	if h.n != 512<<20 || !bytes.Equal(h.buf, want) {
		t.Fatalf("disk is %d bytes; head differs: %v", h.n, !bytes.Equal(h.buf, want))
	}
}

func TestChecksums(t *testing.T) {
	rs := RootSector(512, 4, 128, 64, 312)
	if wordSum(rs) != 0x1234 {
		t.Fatal("root sector word sum")
	}
	x86 := make([]byte, 56)
	copy(x86, "\x7fELF\x01\x01\x01")
	x86[18] = 3
	for _, c := range []struct {
		name string
		k    []byte
	}{{"short", []byte("\x7fELF")}, {"x86", x86}} {
		if _, err := Boot(c.k, ""); err == nil {
			t.Errorf("%s kernel accepted", c.name)
		}
	}
	if _, _, err := Sizes(64, 1<<20, 32, 32); err == nil {
		t.Error("tiny disk accepted")
	}
}
