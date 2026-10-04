package main

import (
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

	"amigaux.org/imagebuilder/forge"
)

const usage = `usage:
  ashforge presets
  ashforge check -recipe recipe.json
  ashforge build (-preset quadra800 | -recipe recipe.json) [-root MiB] [-swap MiB]
                 [-package kind:family:id=file ...] -tapes dir -kernel unix.elf
                 -donor disk.img -output new.img [-export recipe.json]`

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

func build(ctx context.Context, args []string) error {
	fs := flag.NewFlagSet("build", flag.ContinueOnError)
	preset := fs.String("preset", "", "preset id")
	recipePath := fs.String("recipe", "", "exported recipe to rebuild")
	root := fs.Int("root", 0, "root filesystem MiB")
	swap := fs.Int("swap", 0, "swap MiB")
	tapes := fs.String("tapes", "", "directory holding AMIX tape segments 02, 03 and 10")
	kernel := fs.String("kernel", "", "prebuilt Quadra kernel ELF")
	donor := fs.String("donor", "", "A/UX boot donor disk")
	output := fs.String("output", "", "new disk image")
	export := fs.String("export", "", "write the recipe here")
	var pkgs list
	fs.Var(&pkgs, "package", "provisioning package kind:family:id=file; repeat in order")
	if err := fs.Parse(args); err != nil || fs.NArg() != 0 || (*preset == "") == (*recipePath == "") || *tapes == "" || *kernel == "" || *donor == "" || *output == "" {
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
	files := []string{filepath.Join(*tapes, "02"), filepath.Join(*tapes, "03"), filepath.Join(*tapes, "10"), *kernel, *donor}
	for _, p := range pkgs {
		spec, file, ok := strings.Cut(p, "=")
		parts := strings.Split(spec, ":")
		if !ok || len(parts) != 3 {
			return fmt.Errorf("package must be kind:family:id=file")
		}
		s.Provision = append(s.Provision, forge.Package{Kind: parts[0], Family: parts[1], ID: parts[2]})
		files = append(files, file)
	}
	if len(files) != len(forge.Roles(s)) {
		return fmt.Errorf("supply each of the recipe's %d packages with -package", len(s.Provision))
	}
	var media []forge.Media
	var inputs []forge.Input
	for i, name := range files {
		f, m, err := open(name)
		if err != nil {
			return err
		}
		defer f.Close()
		in, err := forge.Digest(ctx, forge.Roles(s)[i], m.Reader, m.Size)
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
	}
	return fmt.Errorf("%s", usage)
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "ashforge:", err)
		os.Exit(1)
	}
}
