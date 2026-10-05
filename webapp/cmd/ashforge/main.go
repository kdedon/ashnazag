package main

import (
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"

	"amigaux.org/imagebuilder/amixtape"
	"amigaux.org/imagebuilder/forge"
)

const usage = `usage:
  ashforge presets
  ashforge check -recipe recipe.json
  ashforge tape part...
  ashforge build (-preset quadra800 | -recipe recipe.json) [-root MiB] [-swap MiB]
                 [-package kind:family:id=file ...] -tape part [-tape part ...]
                 -kernel unix.elf -donor disk.img -output new.img [-export recipe.json]

A tape part is an archive (.tar.bz2, .tar.gz, .tar, .zip), a SIMH .tap image,
a segment file, a raw image or a directory of these.`

type list []string

func (l *list) String() string         { return strings.Join(*l, ",") }
func (l *list) Set(value string) error { *l = append(*l, value); return nil }

func readRecipe(name string) (forge.Recipe, error) {
	f, err := os.Open(name)
	if err != nil {
		return forge.Recipe{}, err
	}
	defer f.Close()
	b, err := io.ReadAll(io.LimitReader(f, 1<<20+1))
	if err != nil {
		return forge.Recipe{}, err
	}
	return forge.Decode(b)
}

func printJSON(v any) error {
	e := json.NewEncoder(os.Stdout)
	e.SetIndent("", "  ")
	return e.Encode(v)
}

func open(name string) (*os.File, forge.Media, error) {
	f, err := os.Open(name)
	if err != nil {
		return nil, forge.Media{}, err
	}
	st, err := f.Stat()
	if err == nil && !st.Mode().IsRegular() {
		err = fmt.Errorf("%s is not a regular file", name)
	}
	if err != nil {
		f.Close()
		return nil, forge.Media{}, err
	}
	return f, forge.Media{Reader: f, Size: st.Size()}, nil
}

// tapeParts opens the files of the named parts, expanding directories.
func tapeParts(names []string) ([]amixtape.Part, func(), error) {
	var parts []amixtape.Part
	var files []*os.File
	closeAll := func() {
		for _, f := range files {
			f.Close()
		}
	}
	for _, name := range names {
		paths := []string{name}
		if st, err := os.Stat(name); err == nil && st.IsDir() {
			entries, err := os.ReadDir(name)
			if err != nil {
				closeAll()
				return nil, nil, err
			}
			paths = paths[:0]
			for _, e := range entries {
				if e.Type().IsRegular() {
					paths = append(paths, filepath.Join(name, e.Name()))
				}
			}
		}
		for _, p := range paths {
			f, m, err := open(p)
			if err != nil {
				closeAll()
				return nil, nil, err
			}
			files = append(files, f)
			parts = append(parts, amixtape.Part{Name: p, Reader: m.Reader, Size: m.Size})
		}
	}
	return parts, closeAll, nil
}

// tapeMedia returns the segments a build layers and the tape input. dir
// names segments already split, taken as they are.
func tapeMedia(ctx context.Context, names []string, dir string) ([]forge.Media, forge.Input, error) {
	var segs []forge.Media
	var digests []amixtape.Digest
	if dir != "" {
		for _, id := range forge.TapeSegments {
			b, err := os.ReadFile(filepath.Join(dir, id))
			if err != nil {
				return nil, forge.Input{}, err
			}
			in, err := forge.Digest(ctx, "", bytes.NewReader(b), int64(len(b)))
			if err != nil {
				return nil, forge.Input{}, err
			}
			segs = append(segs, forge.Media{Reader: bytes.NewReader(b), Size: int64(len(b))})
			digests = append(digests, amixtape.Digest{Size: in.Size, SHA256: in.SHA256})
		}
	} else {
		parts, closeAll, err := tapeParts(names)
		if err != nil {
			return nil, forge.Input{}, err
		}
		defer closeAll()
		res, err := amixtape.Scan(ctx, amixtape.Table, parts, forge.TapeSegments, nil)
		if err != nil {
			return nil, forge.Input{}, err
		}
		if err = res.Need(forge.TapeSegments); err != nil {
			return nil, forge.Input{}, err
		}
		for _, id := range forge.TapeSegments {
			segs = append(segs, forge.Media{Reader: bytes.NewReader(res.Data[id]), Size: int64(len(res.Data[id]))})
		}
		digests = res.Parts
	}
	in, err := forge.TapeInput(ctx, segs, digests)
	return segs, in, err
}

func checkTape(ctx context.Context, names []string) error {
	if len(names) == 0 {
		return fmt.Errorf("%s", usage)
	}
	parts, closeAll, err := tapeParts(names)
	if err != nil {
		return err
	}
	defer closeAll()
	res, err := amixtape.Scan(ctx, amixtape.Table, parts, nil, nil)
	if err != nil {
		return err
	}
	for _, s := range amixtape.Table {
		if where, ok := res.Found[s.ID]; ok {
			fmt.Printf("%s  %s\n", s.ID, where)
		}
	}
	var ids []string
	for _, s := range amixtape.Table {
		ids = append(ids, s.ID)
	}
	return res.Need(ids)
}

func build(ctx context.Context, args []string) error {
	fs := flag.NewFlagSet("build", flag.ContinueOnError)
	preset := fs.String("preset", "", "preset id")
	recipePath := fs.String("recipe", "", "exported recipe to rebuild")
	root := fs.Int("root", 0, "root filesystem MiB")
	swap := fs.Int("swap", 0, "swap MiB")
	var tape list
	fs.Var(&tape, "tape", "AMIX 2.1 tape part; repeat for each part")
	tapes := fs.String("tapes", "", "directory of split segments 02, 03 and 10, not checked against the AMIX table")
	kernel := fs.String("kernel", "", "prebuilt Quadra kernel ELF")
	donor := fs.String("donor", "", "A/UX boot donor disk")
	output := fs.String("output", "", "new disk image")
	export := fs.String("export", "", "write the recipe here")
	var pkgs list
	fs.Var(&pkgs, "package", "provisioning package kind:family:id=file; repeat in order")
	if err := fs.Parse(args); err != nil || fs.NArg() != 0 || (*preset == "") == (*recipePath == "") || (len(tape) == 0) == (*tapes == "") || *kernel == "" || *donor == "" || *output == "" {
		return fmt.Errorf("%s", usage)
	}
	var imported *forge.Recipe
	var s forge.Selection
	if *recipePath != "" {
		r, err := readRecipe(*recipePath)
		if err != nil {
			return err
		}
		if r.ForgeVersion != forge.Version {
			fmt.Fprintf(os.Stderr, "ashforge: recipe from forge %s; this is %s, so the image may differ\n", r.ForgeVersion, forge.Version)
		}
		imported = &r
		if s, err = r.Selection(); err != nil {
			return err
		}
	} else {
		p, ok := forge.Lookup(*preset)
		if !ok {
			return fmt.Errorf("unknown preset %q", *preset)
		}
		s = forge.Selection{Preset: p.ID, Machine: p.Machine, Settings: p.Settings}
	}
	if *root != 0 {
		s.Settings.RootMiB = *root
	}
	if *swap != 0 {
		s.Settings.SwapMiB = *swap
	}
	if len(pkgs) != 0 {
		s.Provision = nil
	}
	files := []string{*kernel, *donor}
	for _, p := range pkgs {
		spec, file, ok := strings.Cut(p, "=")
		parts := strings.Split(spec, ":")
		if !ok || len(parts) != 3 {
			return fmt.Errorf("package must be kind:family:id=file")
		}
		s.Provision = append(s.Provision, forge.Package{Kind: parts[0], Family: parts[1], ID: parts[2]})
		files = append(files, file)
	}
	if len(files) != len(forge.Roles(s))-1 {
		return fmt.Errorf("supply each of the recipe's %d packages with -package", len(s.Provision))
	}
	media, tapeIn, err := tapeMedia(ctx, tape, *tapes)
	if err != nil {
		return err
	}
	inputs := []forge.Input{tapeIn}
	for i, name := range files {
		f, m, err := open(name)
		if err != nil {
			return err
		}
		defer f.Close()
		in, err := forge.Digest(ctx, forge.Roles(s)[i+1], m.Reader, m.Size)
		if err != nil {
			return err
		}
		media = append(media, m)
		inputs = append(inputs, in)
	}
	r, err := forge.NewRecipe(s, inputs)
	if err != nil {
		return err
	}
	if imported != nil {
		if bad := forge.Mismatches(imported.Inputs, inputs); len(bad) != 0 {
			return fmt.Errorf("inputs differ from the recipe: %s", strings.Join(bad, ", "))
		}
	}
	text, err := forge.Encode(r)
	if err != nil {
		return err
	}
	if err = s.Runnable(); err != nil {
		return err
	}
	dir := filepath.Dir(*output)
	scratch, err := os.CreateTemp(dir, ".ashforge-root-*")
	if err != nil {
		return err
	}
	defer os.Remove(scratch.Name())
	defer scratch.Close()
	if err = scratch.Truncate(int64(s.Settings.RootMiB) << 20); err != nil {
		return err
	}
	out, err := os.OpenFile(*output, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
	if err != nil {
		return err
	}
	ok := false
	defer func() {
		out.Close()
		if !ok {
			os.Remove(*output)
		}
	}()
	if err = forge.Quadra(ctx, r, media, scratch, out); err != nil {
		return err
	}
	if err = out.Sync(); err != nil {
		return err
	}
	if *export != "" {
		if err = os.WriteFile(*export, text, 0644); err != nil {
			return err
		}
	}
	ok = true
	fmt.Fprintf(os.Stderr, "Created %s\n", *output)
	return nil
}

func run() error {
	if len(os.Args) < 2 {
		return fmt.Errorf("%s", usage)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	switch os.Args[1] {
	case "presets":
		return printJSON(forge.Presets())
	case "check":
		fs := flag.NewFlagSet("check", flag.ContinueOnError)
		name := fs.String("recipe", "", "exported recipe")
		if err := fs.Parse(os.Args[2:]); err != nil || *name == "" || fs.NArg() != 0 {
			return fmt.Errorf("%s", usage)
		}
		r, err := readRecipe(*name)
		if err != nil {
			return err
		}
		s, err := r.Selection()
		if err != nil {
			return err
		}
		if err = s.Runnable(); err != nil {
			fmt.Fprintln(os.Stderr, "ashforge:", err)
		}
		return printJSON(s)
	case "build":
		return build(ctx, os.Args[2:])
	case "tape":
		return checkTape(ctx, os.Args[2:])
	}
	return fmt.Errorf("%s", usage)
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "ashforge:", err)
		os.Exit(1)
	}
}
