// Package imagebuilder assembles native Quadra console images from local media.
package imagebuilder

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"time"

	"amigaux.org/imagebuilder/planner"
)

type Tools struct {
	HFSUtils  string `json:"hfsutils"`
	MKBB      string `json:"mkbb"`
	BootBlock string `json:"bootblk"`
}

type Request struct {
	Selection      planner.Selection `json:"selection"`
	SourceRepo     string            `json:"sourceRepo"`
	TapeDir        string            `json:"tapeDir"`
	KernelELF      string            `json:"kernelELF"`
	BootDonor      string            `json:"bootDonor"`
	Output         string            `json:"output"`
	RootMiB        int               `json:"rootMiB"`
	SwapMiB        int               `json:"swapMiB"`
	Tools          Tools             `json:"tools"`
	ExpectedSHA256 map[string]string `json:"expectedSHA256,omitempty"`
}

type Receipt struct {
	Recipe    string            `json:"recipe"`
	Selection planner.Selection `json:"selection"`
	Output    string            `json:"output"`
	Bytes     int64             `json:"bytes"`
	SHA256    string            `json:"sha256"`
	Inputs    map[string]string `json:"inputs"`
	RootMiB   int               `json:"rootMiB"`
	SwapMiB   int               `json:"swapMiB"`
	Warnings  []string          `json:"warnings"`
}

type Event struct {
	Stage   string `json:"stage"`
	Message string `json:"message"`
}
type Builder struct {
	Log      io.Writer
	Progress func(Event)
}

func (b Builder) event(stage, message string) {
	if b.Progress != nil {
		b.Progress(Event{stage, message})
	}
}

// ValidateSelection rejects options the fixed console recipe cannot install.
func ValidateSelection(s planner.Selection) (planner.Selection, error) {
	m, err := planner.Plan(s)
	if err != nil {
		return s, err
	}
	s = m.Selection
	if s.Machine != "q800" || len(s.Packages) != 0 || len(s.Containers) != 0 || len(s.ContainerInstances) != 0 || s.Desktop != "none" || s.Boot.Login != "console" || s.Boot.DefaultSession != "console" || s.Boot.Animation {
		return s, fmt.Errorf("native recipe supports only q800, console login, no packages, guests, desktop or animation")
	}
	want := []string{"adb", "framebuffer", "scc", "scsi53c96"}
	got := append([]string(nil), s.Devices...)
	sort.Strings(got)
	if !reflect.DeepEqual(want, got) {
		return s, fmt.Errorf("native recipe requires exactly the default Quadra devices: %s", strings.Join(want, ", "))
	}
	if len(s.BaseMedia) != 0 {
		return s, fmt.Errorf("use tapeDir and bootDonor for native media; selection.baseMedia is unsupported")
	}
	return s, nil
}

func regular(path string) error {
	st, err := os.Stat(path)
	if err != nil {
		return err
	}
	if !st.Mode().IsRegular() || st.Size() == 0 {
		return fmt.Errorf("expected nonempty regular file: %s", path)
	}
	return nil
}

func checkMedia(r Request) error {
	read := func(path string, n int) ([]byte, error) {
		f, err := os.Open(path)
		if err != nil {
			return nil, err
		}
		defer f.Close()
		data := make([]byte, n)
		_, err = io.ReadFull(f, data)
		return data, err
	}
	kernel, err := read(r.KernelELF, 20)
	if err != nil {
		return fmt.Errorf("kernel: %w", err)
	}
	if string(kernel[:7]) != "\x7fELF\x01\x02\x01" || kernel[16] != 0 || kernel[17] != 2 || kernel[18] != 0 || kernel[19] != 4 {
		return fmt.Errorf("kernel must be a linked big-endian ELF32 m68k executable")
	}
	donor, err := read(r.BootDonor, 4)
	if err != nil {
		return fmt.Errorf("boot donor: %w", err)
	}
	if string(donor) != "ER\x02\x00" {
		return fmt.Errorf("boot donor must have an Apple driver descriptor with 512-byte sectors")
	}
	for _, seg := range []string{"02", "03", "10"} {
		data, err := read(filepath.Join(r.TapeDir, seg), 6)
		if err != nil {
			return fmt.Errorf("tape segment %s: %w", seg, err)
		}
		if string(data) != "070701" && string(data) != "070702" {
			return fmt.Errorf("tape segment %s must be an extracted SVR4 cpio archive", seg)
		}
	}
	return nil
}

func hash(ctx context.Context, path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	buf := make([]byte, 1024*1024)
	for {
		if err := ctx.Err(); err != nil {
			return "", err
		}
		n, err := f.Read(buf)
		if n > 0 {
			_, _ = h.Write(buf[:n])
		}
		if err == io.EOF {
			break
		}
		if err != nil {
			return "", err
		}
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func copyFile(src, dst string) error {
	f, err := os.Open(src)
	if err != nil {
		return err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return err
	}
	if !st.Mode().IsRegular() {
		return fmt.Errorf("nonregular source: %s", src)
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0700); err != nil {
		return err
	}
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, st.Mode().Perm())
	if err != nil {
		return err
	}
	_, err = io.Copy(out, f)
	closeErr := out.Close()
	if err != nil {
		return err
	}
	return closeErr
}

func (b Builder) Build(ctx context.Context, r Request) (Receipt, error) {
	receipt := Receipt{Recipe: "q800-console-v1", Inputs: map[string]string{}, Warnings: []string{"Experimental image: boot on hardware is not certified.", "The supplied kernel remains unchanged, including statically linked drivers.", "Memory selection describes hardware; it does not reconfigure the supplied kernel.", "The optional display diagnostic dstest is omitted."}}
	b.event("preflight", "Checking selection, media and tools")
	var err error
	if r.Selection, err = ValidateSelection(r.Selection); err != nil {
		return receipt, err
	}
	if r.RootMiB == 0 {
		r.RootMiB = 64
	}
	if r.SwapMiB == 0 {
		r.SwapMiB = 64
	}
	if r.RootMiB < 64 || r.RootMiB > 2048 || r.RootMiB%4 != 0 || r.SwapMiB < 4 || r.SwapMiB > 2048 {
		return receipt, fmt.Errorf("rootMiB must be 64–2048 and divisible by 4; swapMiB must be 4–2048")
	}
	paths := []*string{&r.SourceRepo, &r.TapeDir, &r.KernelELF, &r.BootDonor, &r.Output, &r.Tools.HFSUtils, &r.Tools.MKBB, &r.Tools.BootBlock}
	for _, p := range paths {
		if *p == "" {
			return receipt, fmt.Errorf("sourceRepo, tapeDir, kernelELF, bootDonor, output and all tool paths are required")
		}
		*p, err = filepath.Abs(*p)
		if err != nil {
			return receipt, err
		}
	}
	if _, err := os.Lstat(r.Output); err == nil || !os.IsNotExist(err) {
		return receipt, fmt.Errorf("output already exists or cannot be checked: %s", r.Output)
	}
	if _, err := os.Stat(filepath.Dir(r.Output)); err != nil {
		return receipt, fmt.Errorf("output directory: %w", err)
	}
	sources := []string{r.KernelELF, r.BootDonor, r.Tools.MKBB, r.Tools.BootBlock}
	for _, seg := range []string{"02", "03", "10"} {
		sources = append(sources, filepath.Join(r.TapeDir, seg))
	}
	executables := []string{r.Tools.MKBB}
	for _, name := range []string{"hformat", "hfsck", "hmount", "hcopy", "humount", "hls"} {
		path := filepath.Join(r.Tools.HFSUtils, name)
		sources = append(sources, path)
		executables = append(executables, path)
	}
	for _, path := range []string{"kernel/mac/diskroot/mkdiskimage.sh", "kernel/mac/diskroot/mkdiskroot.sh", "kernel/mac/diskroot/root.manifest", "kernel/mac/diskroot/mkufs.py", "kernel/mac/diskroot/ufscheck.py", "kernel/mac/diskroot/fstree.py", "kernel/mac/diskroot/addparts.py", "images/mkboot.sh", "images/auxsash.py", "kernel/mac/scsi/test/apmtest.c", "kernel/mac/scsi/apm.c", "kernel/mac/scsi/apm.h"} {
		sources = append(sources, filepath.Join(r.SourceRepo, path))
	}
	etc := filepath.Join(r.SourceRepo, "kernel/mac/diskroot/etc")
	data, err := os.ReadFile(filepath.Join(r.SourceRepo, "kernel/mac/diskroot/root.manifest"))
	if err != nil {
		return receipt, err
	}
	filtered := []string{}
	for _, line := range strings.Split(string(data), "\n") {
		fields := strings.Fields(line)
		if len(fields) > 1 && fields[0] == "f" && fields[1] == "/usr/bin/dstest" {
			continue
		}
		filtered = append(filtered, line)
		if len(fields) == 6 && fields[0] == "f" && strings.HasPrefix(fields[5], "etc/") {
			path := filepath.Join(filepath.Dir(etc), filepath.FromSlash(fields[5]))
			real, err := filepath.EvalSymlinks(path)
			if err != nil {
				return receipt, err
			}
			realEtc, err := filepath.EvalSymlinks(etc)
			if err != nil {
				return receipt, err
			}
			if !strings.HasPrefix(real, realEtc+string(filepath.Separator)) {
				return receipt, fmt.Errorf("manifest source escapes etc directory: %s", fields[5])
			}
			sources = append(sources, path)
		}
	}
	for _, name := range []string{"sh", "python3", "cc", "cpio", "nice", "dd", "cmp", "awk", "sed", "grep", "sha256sum", "tail", "tee", "cp", "mkdir", "rm", "mv", "du", "cut", "wc"} {
		path, err := exec.LookPath(name)
		if err != nil {
			return receipt, fmt.Errorf("required host tool %s: %w", name, err)
		}
		sources = append(sources, path)
	}
	b.event("hash-inputs", "Recording input SHA-256 hashes")
	for _, path := range sources {
		if err := regular(path); err != nil {
			return receipt, err
		}
		sum, err := hash(ctx, path)
		if err != nil {
			return receipt, err
		}
		receipt.Inputs[path] = sum
	}
	for path, expected := range r.ExpectedSHA256 {
		abs, err := filepath.Abs(path)
		if err != nil {
			return receipt, err
		}
		actual, ok := receipt.Inputs[abs]
		if !ok || !strings.EqualFold(actual, expected) {
			return receipt, fmt.Errorf("SHA-256 mismatch or unused input: %s", path)
		}
	}
	if err := checkMedia(r); err != nil {
		return receipt, err
	}
	for _, path := range executables {
		st, err := os.Stat(path)
		if err != nil || st.Mode()&0111 == 0 {
			return receipt, fmt.Errorf("tool is not executable: %s", path)
		}
	}
	work, err := os.MkdirTemp(filepath.Dir(r.Output), ".ashbuild-")
	if err != nil {
		return receipt, err
	}
	defer os.RemoveAll(work)
	for _, path := range sources {
		if strings.HasPrefix(path, etc+string(filepath.Separator)) {
			rel, _ := filepath.Rel(etc, path)
			if err := copyFile(path, filepath.Join(work, "etc", rel)); err != nil {
				return receipt, err
			}
		}
	}
	manifest := filepath.Join(work, "console.manifest")
	if err := os.WriteFile(manifest, []byte(strings.Join(filtered, "\n")), 0600); err != nil {
		return receipt, err
	}
	imagePath := filepath.Join(work, "disk.img")
	env := []string{"PATH=" + os.Getenv("PATH"), "LC_ALL=C", "TZ=UTC", "PYTHONDONTWRITEBYTECODE=1", "AMIX_TAPE=" + r.TapeDir, "BOOT_SOURCE=" + r.BootDonor, "HFSUTILS=" + r.Tools.HFSUtils, "MKBB=" + r.Tools.MKBB, "BOOTBLK=" + r.Tools.BootBlock, "DISKROOT_BUILD=" + filepath.Join(work, "build"), "DISKROOT_SRCDIR=" + work, "DISKROOT_MANIFEST=" + manifest, "ROOTFS=ufs", "ROOTMB=" + strconv.Itoa(r.RootMiB), "SWAPMB=" + strconv.Itoa(r.SwapMiB), "CMDLINE=root=c0d0s1"}
	b.event("assemble", "Building root, boot volume and partition map")
	cmd := exec.CommandContext(ctx, "sh", filepath.Join(r.SourceRepo, "kernel/mac/diskroot/mkdiskimage.sh"), r.KernelELF, imagePath)
	cmd.Dir = work
	cmd.Env = env
	cmd.Stdout = b.Log
	cmd.Stderr = b.Log
	cmd.WaitDelay = 2 * time.Second
	configureProcess(cmd)
	if err := cmd.Run(); err != nil {
		return receipt, fmt.Errorf("image assembly: %w", err)
	}
	if err := regular(imagePath); err != nil {
		return receipt, err
	}
	b.event("verify-inputs", "Checking inputs remained unchanged")
	for path, before := range receipt.Inputs {
		after, err := hash(ctx, path)
		if err != nil {
			return receipt, err
		}
		if before != after {
			return receipt, fmt.Errorf("input changed during build: %s", path)
		}
	}
	receipt.SHA256, err = hash(ctx, imagePath)
	if err != nil {
		return receipt, err
	}
	st, err := os.Stat(imagePath)
	if err != nil {
		return receipt, err
	}
	receipt.Bytes = st.Size()
	if err := ctx.Err(); err != nil {
		return receipt, err
	}
	// A hard link publishes atomically without replacing an existing output.
	if err := os.Link(imagePath, r.Output); err != nil {
		return receipt, fmt.Errorf("publish image: %w", err)
	}
	receipt.Selection = r.Selection
	receipt.Output = r.Output
	receipt.RootMiB = r.RootMiB
	receipt.SwapMiB = r.SwapMiB
	b.event("complete", "Disk image generated")
	return receipt, nil
}
