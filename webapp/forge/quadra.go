package forge

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"path"
	"sort"
	"strings"

	"amigaux.org/imagebuilder/bootimage"
	"amigaux.org/imagebuilder/diskimage"
	"amigaux.org/imagebuilder/provision"
	"amigaux.org/imagebuilder/recipes"
	"amigaux.org/imagebuilder/rootfs"
	"amigaux.org/imagebuilder/ufs"
)

// QuadraTimestamp fixes root file times so equal inputs give equal images.
const QuadraTimestamp = 723000000

// DefaultZone is the Falcon root's /etc/TIMEZONE value.
const DefaultZone = "CST6CDT"

// Media is one input. Builds take the tape's segments (TapeSegments), then
// one Media per later role.
type Media struct {
	Reader io.ReaderAt
	Size   int64
}

// Install adds the canonical recipe to a root tree.
func Install(entries []ufs.Entry, recipe []byte) ([]ufs.Entry, error) {
	if _, err := Decode(recipe); err != nil {
		return nil, err
	}
	out := make([]ufs.Entry, 0, len(entries)+2)
	for _, e := range entries {
		if e.Path == path.Dir(RecipePath) || strings.HasPrefix(e.Path, path.Dir(RecipePath)+"/") {
			return nil, fmt.Errorf("%s already exists in the root", e.Path)
		}
		out = append(out, e)
	}
	out = append(out, ufs.Entry{Path: path.Dir(RecipePath), Kind: 'd', Mode: 0755},
		ufs.Entry{Path: RecipePath, Kind: 'f', Mode: 0644, Size: int64(len(recipe)), Data: bytes.NewReader(recipe)})
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out, nil
}

// Verify hashes the media and compares them with the recipe.
func Verify(ctx context.Context, r Recipe, s Selection, media []Media) error {
	roles := Roles(s)
	n := len(TapeSegments)
	if len(media) != len(roles)-1+n {
		return fmt.Errorf("expected %d inputs, got %d", len(roles)-1+n, len(media))
	}
	tape, err := TapeInput(ctx, media[:n], nil)
	if err != nil {
		return err
	}
	got := []Input{tape}
	for i, m := range media[n:] {
		in, err := Digest(ctx, roles[i+1], m.Reader, m.Size)
		if err != nil {
			return err
		}
		got = append(got, in)
	}
	if bad := Mismatches(r.Inputs, got); len(bad) != 0 {
		return fmt.Errorf("inputs differ from the recipe: %s", strings.Join(bad, ", "))
	}
	return nil
}

// Quadra builds a complete Quadra disk image from verified media. root is
// scratch space for the root filesystem; the disk streams to out.
func Quadra(ctx context.Context, r Recipe, media []Media, root interface {
	io.ReaderAt
	io.WriterAt
}, out io.Writer) error {
	s, err := r.Selection()
	if err != nil {
		return err
	}
	if err = s.Runnable(); err != nil {
		return err
	}
	if p, _ := Lookup(s.Preset); p.Recipe != "quadra-console" {
		return fmt.Errorf("the %s preset is not a Quadra build", p.Label)
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
	kernel := media[3]
	if kernel.Size < 52 || kernel.Size > 32<<20 {
		return fmt.Errorf("kernel must be at most 32 MiB")
	}
	elf := make([]byte, kernel.Size)
	if n, err := kernel.Reader.ReadAt(elf, 0); n != len(elf) {
		return fmt.Errorf("read kernel: %v", err)
	}
	boot, err := bootimage.Build(ctx, media[4].Reader, media[4].Size, elf, bootimage.Options{CommandLine: "root=c0d0s1"})
	if err != nil {
		return err
	}
	entries, err := rootfs.ScanLayers(ctx, []rootfs.Source{{Reader: media[0].Reader, Size: media[0].Size}, {Reader: media[1].Reader, Size: media[1].Size}, {Reader: media[2].Reader, Size: media[2].Size}}, rootfs.Options{})
	if err != nil {
		return err
	}
	if entries, err = Root(ctx, text, entries, kernel, media[5:]); err != nil {
		return err
	}
	rootSize := int64(s.Settings.RootMiB) << 20
	if _, err = ufs.Build(ctx, root, ufs.Options{SizeMiB: s.Settings.RootMiB, Timestamp: QuadraTimestamp}, entries); err != nil {
		return err
	}
	_, err = diskimage.Assemble(ctx, out, bytes.NewReader(boot), int64(len(boot)), root, rootSize, diskimage.Options{SwapBytes: int64(s.Settings.SwapMiB) << 20})
	return err
}

// Root applies the console recipe, packages and the recipe file to the
// layered AMIX tree.
func Root(ctx context.Context, recipe []byte, entries []ufs.Entry, kernel Media, packages []Media) ([]ufs.Entry, error) {
	r, err := Decode(recipe)
	if err != nil {
		return nil, err
	}
	s, err := r.Selection()
	if err != nil {
		return nil, err
	}
	if err = s.Runnable(); err != nil {
		return nil, err
	}
	if len(r.Inputs) != len(Roles(s)) {
		return nil, fmt.Errorf("recipe lists no input digests")
	}
	if len(packages) != len(s.Provision) {
		return nil, fmt.Errorf("expected %d packages, got %d", len(s.Provision), len(packages))
	}
	entries, err = recipes.QuadraRoot(entries, kernel.Reader, kernel.Size)
	if err != nil {
		return nil, err
	}
	if p, _ := Lookup(s.Preset); p.Recipe == "falcon-console" {
		if entries, err = recipes.AtariRoot(entries, DefaultZone); err != nil {
			return nil, err
		}
	}
	if len(packages) != 0 {
		readers := make([]io.ReaderAt, len(packages))
		for i, p := range packages {
			readers[i] = p.Reader
		}
		if entries, err = provision.Stage(ctx, entries, r.Artifacts(s), readers); err != nil {
			return nil, err
		}
	}
	canonical, err := Encode(r.Portable())
	if err != nil {
		return nil, err
	}
	return Install(entries, canonical)
}
