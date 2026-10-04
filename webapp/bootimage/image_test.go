package bootimage

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func fixtureELF() []byte {
	e := make([]byte, 1024)
	copy(e, "\x7fELF")
	e[4] = 1
	e[5] = 2
	be.PutUint16(e[16:], 2)
	be.PutUint16(e[18:], 4)
	be.PutUint32(e[24:], 0x1000)
	be.PutUint32(e[28:], 52)
	be.PutUint16(e[42:], 32)
	be.PutUint16(e[44:], 1)
	p := e[52:]
	be.PutUint32(p, 1)
	be.PutUint32(p[4:], 512)
	be.PutUint32(p[8:], 0x1000)
	be.PutUint32(p[12:], 0x1000)
	be.PutUint32(p[16:], 512)
	be.PutUint32(p[20:], 1024)
	for i := 512; i < len(e); i++ {
		e[i] = byte(i)
	}
	return e
}
func fixtureDonor() []byte {
	b := make([]byte, 128*512)
	copy(b, "ER")
	be.PutUint16(b[2:], 512)
	be.PutUint32(b[4:], 128)
	be.PutUint16(b[16:], 1)
	be.PutUint32(b[18:], 64)
	be.PutUint16(b[22:], 1)
	for i, typ := range []string{"Apple_partition_map", "Apple_Driver", "Apple_HFS"} {
		p := b[(i+1)*512:]
		copy(p, "PM")
		be.PutUint32(p[4:], 3)
		be.PutUint32(p[8:], []uint32{1, 64, 65}[i])
		be.PutUint32(p[12:], []uint32{63, 1, 63}[i])
		copy(p[48:], typ)
	}
	return b
}
func TestBuild(t *testing.T) {
	d := fixtureDonor()
	e := fixtureELF()
	b, err := Build(context.Background(), bytes.NewReader(d), int64(len(d)), e, Options{CommandLine: "root=c0d0s1"})
	if err != nil {
		t.Fatal(err)
	}
	v := b[65*512:]
	if string(v[0x8c:0x90]) != "UxBB" || string(v[896:907]) != "root=c0d0s1" {
		t.Fatal("boot parameters")
	}
	k, err := Flatten(e)
	if err != nil {
		t.Fatal(err)
	}
	off := be.Uint32(v[0x90:])
	if !bytes.Equal(v[off:off+uint32(len(k.Data))], k.Data) {
		t.Fatal("kernel mismatch")
	}
	if len(b) > 32<<20 {
		t.Fatal("boot exceeds bound")
	}
}
func TestReject(t *testing.T) {
	d := fixtureDonor()
	e := fixtureELF()
	for _, mutate := range []func([]byte){func(e []byte) { e[5] = 1 }, func(e []byte) { be.PutUint32(e[52+20:], 1) }, func(e []byte) { be.PutUint32(e[52+4:], 0xffffffff) }, func(e []byte) { be.PutUint32(e[52+8:], 0xfffffff0) }} {
		bad := append([]byte(nil), e...)
		mutate(bad)
		if _, err := Build(context.Background(), bytes.NewReader(d), int64(len(d)), bad, Options{}); err == nil {
			t.Fatal("accepted bad ELF")
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := Build(ctx, bytes.NewReader(d), int64(len(d)), e, Options{}); err == nil {
		t.Fatal("ignored cancellation")
	}
}
func TestTemplate(t *testing.T) {
	b, err := os.ReadFile("../../kernel/mac/bootblk/build/bootblk.bin")
	if os.IsNotExist(err) {
		t.Skip("native boot blocks are not built")
	}
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(b, bootBlocks) {
		t.Fatal("boot blocks changed: regenerate bootblocks.go")
	}
}
func TestNativeFixture(t *testing.T) {
	if os.Getenv("ASH_BOOT_INTEGRATION") == "" {
		t.Skip("local media required")
	}
	d, err := os.Open("../../images/q800-test-small.img")
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	st, _ := d.Stat()
	e, err := os.ReadFile("../../kernel/build/unix-mac.elf")
	if err != nil {
		t.Fatal(err)
	}
	b, err := Build(context.Background(), d, st.Size(), e, Options{CommandLine: "root=c0d0s1"})
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	output := filepath.Join(dir, "boot.img")
	if err = os.WriteFile(output, b, 0600); err != nil {
		t.Fatal(err)
	}
	run := func(bin string, args ...string) {
		t.Helper()
		c := exec.Command(bin, args...)
		c.Env = append(os.Environ(), "HOME="+dir)
		if out, err := c.CombinedOutput(); err != nil {
			t.Fatalf("%s: %v\n%s", bin, err, out)
		}
	}
	run("../../toolchain/bin/hfsck", "-n", output, "1")
	run("../../kernel/mac/bootblk/build/mkbb", "check", output, "../../kernel/build/unix-mac.elf")
	run("../../toolchain/bin/hmount", output, "1")
	extracted := filepath.Join(dir, "unix.bin")
	run("../../toolchain/bin/hcopy", "-r", ":unix", extracted)
	run("../../toolchain/bin/humount")
	expected := filepath.Join(dir, "flat.bin")
	run("../../kernel/mac/bootblk/build/mkbb", "flat", "../../kernel/build/unix-mac.elf", expected)
	actual, _ := os.ReadFile(extracted)
	want, _ := os.ReadFile(expected)
	if !bytes.Equal(actual, want) {
		t.Fatal("HFS extraction differs from native flat kernel")
	}

}

func TestDonorValidation(t *testing.T) {
	e := fixtureELF()
	for _, mutate := range []func([]byte){
		func(d []byte) { copy(d, "XX") },
		func(d []byte) { be.PutUint32(d[516:], 64) },
		func(d []byte) { be.PutUint32(d[1024+8:], 65) },
		func(d []byte) { be.PutUint32(d[18:], 63) },
		func(d []byte) { be.PutUint32(d[1536+12:], 0xffffffff) },
	} {
		d := fixtureDonor()
		mutate(d)
		if _, err := Build(context.Background(), bytes.NewReader(d), int64(len(d)), e, Options{}); err == nil {
			t.Fatal("accepted invalid donor")
		}
	}
	d := fixtureDonor()
	for _, cmd := range []string{string(make([]byte, 128)), "root\x00evil"} {
		if _, err := Build(context.Background(), bytes.NewReader(d), int64(len(d)), e, Options{CommandLine: cmd}); err == nil {
			t.Fatal("accepted invalid command line")
		}
	}
}
