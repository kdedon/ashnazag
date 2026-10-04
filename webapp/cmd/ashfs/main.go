package main

import (
	"amigaux.org/imagebuilder/provision"
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"

	"amigaux.org/imagebuilder/recipes"
	"amigaux.org/imagebuilder/rootfs"
	"amigaux.org/imagebuilder/ufs"
)

type archives []string

func (a *archives) String() string         { return fmt.Sprint([]string(*a)) }
func (a *archives) Set(value string) error { *a = append(*a, value); return nil }

func run() error {
	var archive archives
	flag.Var(&archive, "archive", "SVR4 cpio archive; repeat in layer order")
	provisionPath := flag.String("provision", "", "JSON list of system packages with kind, family, id, sha256, size and file")
	recipe := flag.String("recipe", "layers", "layers or quadra-console")
	kernelPath := flag.String("kernel", "", "prebuilt Quadra kernel ELF")
	output := flag.String("output", "", "new UFS image path")
	size := flag.Int("size", 64, "filesystem size in MiB (multiple of 4)")
	flag.Parse()
	if len(archive) == 0 || *output == "" || flag.NArg() != 0 {
		return fmt.Errorf("usage: ashfs -archive files.cpio [-archive next.cpio] -output root.img -size 64 [-recipe quadra-console -kernel unix.elf]")
	}
	if *size < 4 || *size > 2048 || *size%4 != 0 {
		return fmt.Errorf("size must be 4–2048 MiB in multiples of 4")
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if *recipe != "layers" && *recipe != "quadra-console" {
		return fmt.Errorf("unknown recipe %q", *recipe)
	}
	if *recipe == "quadra-console" && (len(archive) != 3 || *kernelPath == "") {
		return fmt.Errorf("quadra-console requires archives 02, 03 and 10 in order, and -kernel")
	}
	if *recipe == "layers" && *kernelPath != "" {
		return fmt.Errorf("-kernel requires quadra-console recipe")
	}
	var sources []rootfs.Source
	for _, name := range archive {
		src, err := os.Open(name)
		if err != nil {
			return err
		}
		defer src.Close()
		stat, err := src.Stat()
		if err != nil {
			return err
		}
		sources = append(sources, rootfs.Source{Reader: src, Size: stat.Size()})
	}
	entries, err := rootfs.ScanLayers(ctx, sources, rootfs.Options{})
	if err != nil {
		return err
	}
	var stamp uint32
	if *recipe == "quadra-console" {
		kernel, err := os.Open(*kernelPath)
		if err != nil {
			return err
		}
		defer kernel.Close()
		stat, err := kernel.Stat()
		if err != nil {
			return err
		}
		entries, err = recipes.QuadraRoot(entries, kernel, stat.Size())
		if err != nil {
			return err
		}
		stamp = 723000000
	}
	if *provisionPath != "" {
		requestFile, err := os.Open(*provisionPath)
		if err != nil {
			return err
		}
		defer requestFile.Close()
		data, err := io.ReadAll(io.LimitReader(requestFile, (1<<20)+1))
		if err != nil {
			return err
		}
		if len(data) > 1<<20 {
			return fmt.Errorf("provision request exceeds 1 MiB")
		}
		var artifacts []struct {
			provision.Artifact
			File string `json:"file"`
		}
		decoder := json.NewDecoder(bytes.NewReader(data))
		decoder.DisallowUnknownFields()
		if err = decoder.Decode(&artifacts); err != nil {
			return err
		}
		if decoder.Decode(new(any)) != io.EOF {
			return fmt.Errorf("trailing provision JSON")
		}
		if len(artifacts) > 64 {
			return fmt.Errorf("choose at most 64 provisioning packages")
		}
		var metadata []provision.Artifact
		var readers []io.ReaderAt
		for _, a := range artifacts {
			name := a.File
			if !filepath.IsAbs(name) {
				name = filepath.Join(filepath.Dir(*provisionPath), name)
			}
			f, err := os.Open(name)
			if err != nil {
				return err
			}
			defer f.Close()
			st, err := f.Stat()
			if err != nil {
				return err
			}
			if !st.Mode().IsRegular() {
				return fmt.Errorf("provisioning input must be a regular file")
			}
			if st.Size() != a.Size {
				return fmt.Errorf("provisioning package size mismatch")
			}
			metadata = append(metadata, a.Artifact)
			readers = append(readers, f)
		}
		entries, err = provision.Stage(ctx, entries, metadata, readers)
		if err != nil {
			return err
		}
	}
	dst, err := os.OpenFile(*output, os.O_RDWR|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	ok := false
	defer func() {
		dst.Close()
		if !ok {
			os.Remove(*output)
		}
	}()
	if err = dst.Truncate(int64(*size) * 1048576); err != nil {
		return err
	}
	report, err := ufs.Build(ctx, dst, ufs.Options{SizeMiB: *size, Timestamp: stamp}, entries)
	if err != nil {
		return err
	}
	if err = dst.Sync(); err != nil {
		return err
	}
	if err = dst.Close(); err != nil {
		return err
	}
	ok = true
	fmt.Printf("Created %s (%d MiB): %+v\n", *output, *size, report)
	return nil
}
func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "ashfs:", err)
		os.Exit(1)
	}
}
