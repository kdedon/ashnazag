package imagebuilder

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"amigaux.org/imagebuilder/planner"
)

func selection() planner.Selection {
	return planner.Selection{Machine: "q800", Devices: []string{"scsi53c96", "scc", "adb", "framebuffer"}}
}

func TestUnsupportedSelections(t *testing.T) {
	cases := map[string]func(*planner.Selection){
		"device deselection": func(s *planner.Selection) { s.Devices = s.Devices[:2] },
		"optional device":    func(s *planner.Selection) { s.Devices = append(s.Devices, "sonic") },
		"desktop":            func(s *planner.Selection) { s.Packages = []string{"x11"}; s.Desktop = "twm" },
		"animation":          func(s *planner.Selection) { s.Boot.Animation = true },
		"guest":              func(s *planner.Selection) { s.Containers = []string{"macenv"} },
		"unused media":       func(s *planner.Selection) { s.BaseMedia = map[string][]string{"amix": {"tape"}} },
	}
	for name, change := range cases {
		t.Run(name, func(t *testing.T) {
			s := selection()
			change(&s)
			if _, err := ValidateSelection(s); err == nil {
				t.Fatal("unsupported choice accepted")
			}
		})
	}
	if _, err := ValidateSelection(selection()); err != nil {
		t.Fatal(err)
	}
}

func TestHashCancellation(t *testing.T) {
	p := filepath.Join(t.TempDir(), "input")
	if err := os.WriteFile(p, []byte("data"), 0600); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := hash(ctx, p); !errors.Is(err, context.Canceled) {
		t.Fatalf("got %v", err)
	}
}

func TestRefusesExistingOutput(t *testing.T) {
	dir := t.TempDir()
	out := filepath.Join(dir, "disk.img")
	if err := os.WriteFile(out, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	r := Request{Selection: selection(), SourceRepo: dir, TapeDir: dir, KernelELF: "missing", BootDonor: "missing", Output: out, Tools: Tools{HFSUtils: dir, MKBB: "missing", BootBlock: "missing"}}
	if _, err := (Builder{}).Build(context.Background(), r); err == nil || !strings.Contains(err.Error(), "output already exists") {
		t.Fatalf("got %v", err)
	}
	got, _ := os.ReadFile(out)
	if string(got) != "keep" {
		t.Fatal("output modified")
	}
}

func TestRejectsDiskBounds(t *testing.T) {
	for _, r := range []Request{{Selection: selection(), RootMiB: 65}, {Selection: selection(), RootMiB: 4096}, {Selection: selection(), SwapMiB: -1}} {
		if _, err := (Builder{}).Build(context.Background(), r); err == nil || !strings.Contains(err.Error(), "rootMiB") {
			t.Fatalf("got %v", err)
		}
	}
}

func fixture(t *testing.T, script string) Request {
	t.Helper()
	for _, name := range []string{"sh", "python3", "cc", "cpio", "nice", "dd", "cmp", "awk", "sed", "grep", "sha256sum", "tail", "tee", "cp", "mkdir", "rm", "mv", "du", "cut", "wc"} {
		if _, err := exec.LookPath(name); err != nil {
			t.Skipf("missing host tool %s", name)
		}
	}
	dir := t.TempDir()
	write := func(path string, data []byte) {
		t.Helper()
		if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, 0700); err != nil {
			t.Fatal(err)
		}
	}
	r := Request{Selection: selection(), SourceRepo: filepath.Join(dir, "repo"), TapeDir: filepath.Join(dir, "tape"), KernelELF: filepath.Join(dir, "kernel.elf"), BootDonor: filepath.Join(dir, "donor.img"), Output: filepath.Join(dir, "result.img"), Tools: Tools{HFSUtils: filepath.Join(dir, "tools"), MKBB: filepath.Join(dir, "mkbb"), BootBlock: filepath.Join(dir, "bootblk")}}
	kernel := make([]byte, 24)
	copy(kernel, []byte("\x7fELF\x01\x02\x01"))
	kernel[17] = 2
	kernel[19] = 4
	write(r.KernelELF, kernel)
	write(r.BootDonor, []byte("ER\x02\x00"))
	write(r.Tools.MKBB, []byte("#!/bin/sh\n"))
	write(r.Tools.BootBlock, []byte("block"))
	for _, s := range []string{"02", "03", "10"} {
		write(filepath.Join(r.TapeDir, s), []byte("070701archive"))
	}
	for _, s := range []string{"hformat", "hfsck", "hmount", "hcopy", "humount", "hls"} {
		write(filepath.Join(r.Tools.HFSUtils, s), []byte("#!/bin/sh\n"))
	}
	for _, p := range []string{"kernel/mac/diskroot/mkdiskroot.sh", "kernel/mac/diskroot/mkufs.py", "kernel/mac/diskroot/ufscheck.py", "kernel/mac/diskroot/fstree.py", "kernel/mac/diskroot/addparts.py", "images/mkboot.sh", "images/auxsash.py", "kernel/mac/scsi/test/apmtest.c", "kernel/mac/scsi/apm.c", "kernel/mac/scsi/apm.h"} {
		write(filepath.Join(r.SourceRepo, p), []byte("fixture"))
	}
	write(filepath.Join(r.SourceRepo, "kernel/mac/diskroot/mkdiskimage.sh"), []byte(script))
	write(filepath.Join(r.SourceRepo, "kernel/mac/diskroot/root.manifest"), []byte("f /etc/nodename 644 0 3 etc/nodename\nf /usr/bin/dstest 755 2 2 ../../build/mac/display/dstest\n"))
	write(filepath.Join(r.SourceRepo, "kernel/mac/diskroot/etc/nodename"), []byte("ash-nazag\n"))
	write(filepath.Join(r.SourceRepo, "kernel/mac/diskroot/etc/unused-backup"), nil)
	return r
}

func TestBuildOrchestration(t *testing.T) {
	r := fixture(t, "#!/bin/sh\nset -eu\ntest -f \"$DISKROOT_SRCDIR/etc/nodename\"\n! grep -q dstest \"$DISKROOT_MANIFEST\"\ntest -z \"${SPARE-}\"\nprintf 'assembled image' > \"$2\"\n")
	t.Setenv("SPARE", "128")
	receipt, err := (Builder{}).Build(context.Background(), r)
	if err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(r.Output)
	if err != nil || string(data) != "assembled image" {
		t.Fatalf("output %q: %v", data, err)
	}
	if receipt.SHA256 == "" || receipt.Bytes != 15 || len(receipt.Inputs) < 20 {
		t.Fatalf("incomplete receipt: %+v", receipt)
	}
	for path := range receipt.Inputs {
		if strings.Contains(path, "unused-backup") {
			t.Fatal("unused backup included")
		}
	}
	work, _ := filepath.Glob(filepath.Join(filepath.Dir(r.Output), ".ashbuild-*"))
	if len(work) != 0 {
		t.Fatal("scratch retained")
	}
}

func TestInputHashMismatch(t *testing.T) {
	r := fixture(t, "#!/bin/sh\nexit 99\n")
	r.ExpectedSHA256 = map[string]string{r.KernelELF: strings.Repeat("0", 64)}
	if _, err := (Builder{}).Build(context.Background(), r); err == nil || !strings.Contains(err.Error(), "SHA-256 mismatch") {
		t.Fatalf("got %v", err)
	}
	if _, err := os.Stat(r.Output); !os.IsNotExist(err) {
		t.Fatal("output published")
	}
}

func TestBuildCancellation(t *testing.T) {
	r := fixture(t, "#!/bin/sh\nsleep 30 &\nwait\n")
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	b := Builder{Progress: func(e Event) {
		if e.Stage == "assemble" {
			time.AfterFunc(100*time.Millisecond, cancel)
		}
	}}
	start := time.Now()
	if _, err := b.Build(ctx, r); err == nil {
		t.Fatal("cancellation ignored")
	}
	if time.Since(start) > 5*time.Second {
		t.Fatal("cancellation did not stop children promptly")
	}
	if _, err := os.Stat(r.Output); !os.IsNotExist(err) {
		t.Fatal("output published")
	}
	work, _ := filepath.Glob(filepath.Join(filepath.Dir(r.Output), ".ashbuild-*"))
	if len(work) != 0 {
		t.Fatal("scratch retained")
	}
}
