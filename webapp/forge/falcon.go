package forge

import (
	"context"
	"fmt"
	"io"

	"amigaux.org/imagebuilder/atari"
	"amigaux.org/imagebuilder/recipes"
	"amigaux.org/imagebuilder/rootfs"
	"amigaux.org/imagebuilder/ufs"
)

// Scratch holds a filesystem while it is built.
type Scratch interface {
	io.ReaderAt
	io.WriterAt
}

// FalconCmdLine is the kernel command line of a forged Falcon disk.
const FalconCmdLine = "root=c0d0s1"

// FalconSizes returns the root scratch size and the /home size in MiB the
// recipe's disk needs for a kernel of kernelSize bytes.
func FalconSizes(s Selection, kernelSize int64) (rootMiB, homeMiB int, err error) {
	_, home, err := atari.Sizes(s.Settings.DiskMiB, int(kernelSize), s.Settings.RootMiB, s.Settings.SwapMiB)
	return s.Settings.RootMiB, home, err
}

// Falcon builds a Falcon disk image from verified media: the AMIX tape
// segments, then the kernel, then packages. root and home are zeroed scratch
// of FalconSizes; the disk streams to out.
func Falcon(ctx context.Context, r Recipe, media []Media, root, home Scratch, out io.Writer) error {
	s, err := r.Selection()
	if err != nil {
		return err
	}
	if p, _ := Lookup(s.Preset); p.Recipe != "falcon-console" {
		return fmt.Errorf("the %s preset is not a Falcon build", p.Label)
	}
	if err = s.Runnable(); err != nil {
		return err
	}
	if len(r.Lock.Bindings) != 0 || len(r.Lock.Artifacts) != 0 {
		return fmt.Errorf("catalog packages cannot be installed by this preset yet")
	}
	if err = Verify(ctx, r, s, media); err != nil {
		return err
	}
	text, err := Encode(r)
	if err != nil {
		return err
	}
	n := len(TapeSegments)
	kernel := media[n]
	if kernel.Size < 52 || kernel.Size > 32<<20 {
		return fmt.Errorf("kernel must be at most 32 MiB")
	}
	elf := make([]byte, kernel.Size)
	if m, err := kernel.Reader.ReadAt(elf, 0); m != len(elf) {
		return fmt.Errorf("read kernel: %v", err)
	}
	rootMiB, homeMiB, err := FalconSizes(s, kernel.Size)
	if err != nil {
		return err
	}
	src := make([]rootfs.Source, n)
	for i := range src {
		src[i] = rootfs.Source{Reader: media[i].Reader, Size: media[i].Size}
	}
	entries, err := rootfs.ScanLayers(ctx, src, rootfs.Options{})
	if err != nil {
		return err
	}
	if entries, err = Root(ctx, text, entries, kernel, media[n+1:]); err != nil {
		return err
	}
	if _, err = ufs.Build(ctx, root, ufs.Options{SizeMiB: rootMiB, Timestamp: QuadraTimestamp}, entries); err != nil {
		return err
	}
	if _, err = ufs.Build(ctx, home, ufs.Options{SizeMiB: homeMiB, Timestamp: QuadraTimestamp, MountPoint: "/home"}, recipes.AtariHome()); err != nil {
		return err
	}
	return atari.Assemble(ctx, out, atari.Params{DiskMiB: s.Settings.DiskMiB, SwapMiB: s.Settings.SwapMiB, Kernel: elf, CmdLine: FalconCmdLine,
		Root: root, RootMiB: rootMiB, Home: home, HomeMiB: homeMiB})
}
