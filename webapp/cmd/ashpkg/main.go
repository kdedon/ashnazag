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
	"syscall"
	"unicode/utf8"

	"amigaux.org/imagebuilder/packagebuild"
	"amigaux.org/imagebuilder/packages"
	"amigaux.org/imagebuilder/sources"
	"amigaux.org/imagebuilder/svr4"
)

type resolveRequest struct {
	Catalog  []json.RawMessage  `json:"catalog"`
	Requests []packages.Request `json:"requests"`
}
type buildRequest struct {
	Lock          string            `json:"lock"`
	EnvironmentID string            `json:"environmentID"`
	PackageID     string            `json:"packageID"`
	Tier          string            `json:"tier"`
	Inputs        map[string]string `json:"inputs"`
	Info          svr4.Info         `json:"info"`
	Output        string            `json:"output"`
	Receipt       string            `json:"receipt"`
	Limits        sources.Limits    `json:"limits"`
}

func strict(data []byte, v any) error {
	if !utf8.Valid(data) {
		return fmt.Errorf("JSON must be valid UTF-8")
	}
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err := d.Decode(v); err != nil {
		return err
	}
	var extra any
	if d.Decode(&extra) != io.EOF {
		return fmt.Errorf("expected one JSON value")
	}
	return nil
}
func readJSON(path string) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	b, err := io.ReadAll(io.LimitReader(f, (32<<20)+1))
	if err != nil {
		return nil, err
	}
	if len(b) > 32<<20 {
		return nil, fmt.Errorf("JSON exceeds 32 MiB")
	}
	return b, nil
}
func relative(base, p string) (string, error) {
	if p == "" {
		return "", fmt.Errorf("missing path")
	}
	if filepath.IsAbs(p) {
		return filepath.Clean(p), nil
	}
	return filepath.Join(base, p), nil
}
func jsonBytes(v any) ([]byte, error) {
	b, err := json.MarshalIndent(v, "", "  ")
	return append(b, '\n'), err
}
func publish(paths []string, data [][]byte) error {
	temps := make([]string, len(paths))
	seen := map[string]bool{}
	defer func() {
		for _, p := range temps {
			if p != "" {
				os.Remove(p)
			}
		}
	}()
	for i, p := range paths {
		if seen[p] {
			return fmt.Errorf("output paths must differ")
		}
		seen[p] = true
		if _, err := os.Lstat(p); err == nil {
			return fmt.Errorf("output exists: %s", p)
		} else if !os.IsNotExist(err) {
			return err
		}
		f, err := os.CreateTemp(filepath.Dir(p), ".ashpkg-*")
		if err != nil {
			return err
		}
		temps[i] = f.Name()
		if _, err = f.Write(data[i]); err != nil {
			f.Close()
			return err
		}
		if err = f.Sync(); err != nil {
			f.Close()
			return err
		}
		if err = f.Close(); err != nil {
			return err
		}
	}
	published := []string{}
	for i, p := range paths {
		if err := os.Link(temps[i], p); err != nil {
			for _, old := range published {
				os.Remove(old)
			}
			return err
		}
		published = append(published, p)
	}
	return nil
}
func run(ctx context.Context, args []string, stdout io.Writer) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: ashpkg resolve|build -request request.json [-output lock.json]")
	}
	flags := flag.NewFlagSet("ashpkg "+args[0], flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	requestPath := flags.String("request", "", "JSON request")
	output := flags.String("output", "", "resolved lock output")
	if err := flags.Parse(args[1:]); err != nil {
		return err
	}
	if *requestPath == "" || flags.NArg() != 0 {
		return fmt.Errorf("expected -request")
	}
	absolute, err := filepath.Abs(*requestPath)
	if err != nil {
		return err
	}
	base := filepath.Dir(absolute)
	raw, err := readJSON(absolute)
	if err != nil {
		return err
	}
	switch args[0] {
	case "resolve":
		var req resolveRequest
		if err := strict(raw, &req); err != nil {
			return err
		}
		if req.Catalog == nil || req.Requests == nil {
			return fmt.Errorf("catalog and requests required")
		}
		catalog := []packages.Recipe{}
		for _, b := range req.Catalog {
			r, err := packages.DecodeRecipe(b)
			if err != nil {
				return err
			}
			catalog = append(catalog, r)
		}
		lock, err := packages.Resolve(catalog, req.Requests)
		if err != nil {
			return err
		}
		b, err := jsonBytes(lock)
		if err != nil {
			return err
		}
		if *output == "" {
			_, err = stdout.Write(b)
			return err
		}
		out, err := filepath.Abs(*output)
		if err != nil {
			return err
		}
		return publish([]string{out}, [][]byte{b})
	case "build":
		if *output != "" {
			return fmt.Errorf("build outputs belong in request")
		}
		var req buildRequest
		if err := strict(raw, &req); err != nil {
			return err
		}
		lockPath, err := relative(base, req.Lock)
		if err != nil {
			return err
		}
		b, err := readJSON(lockPath)
		if err != nil {
			return err
		}
		lock, err := packages.DecodeLock(b)
		if err != nil {
			return err
		}
		var selected *packages.Binding
		for i := range lock.Bindings {
			x := &lock.Bindings[i]
			if x.EnvironmentID == req.EnvironmentID && x.Recipe.ID == req.PackageID && x.Tier == req.Tier {
				selected = x
				break
			}
		}
		if selected == nil {
			return fmt.Errorf("binding not found")
		}
		inputs := map[string]packagebuild.Input{}
		files := []*os.File{}
		defer func() {
			for _, f := range files {
				f.Close()
			}
		}()
		for digest, p := range req.Inputs {
			p, err = relative(base, p)
			if err != nil {
				return err
			}
			f, err := os.Open(p)
			if err != nil {
				return err
			}
			files = append(files, f)
			stat, err := f.Stat()
			if err != nil {
				return err
			}
			if !stat.Mode().IsRegular() {
				return fmt.Errorf("source is not a regular file")
			}
			inputs[digest] = packagebuild.Input{Reader: f, Size: stat.Size()}
		}
		result, err := packagebuild.Build(ctx, *selected, inputs, req.Info, req.Limits)
		if err != nil {
			return err
		}
		out, err := relative(base, req.Output)
		if err != nil {
			return err
		}
		receipt, err := relative(base, req.Receipt)
		if err != nil {
			return err
		}
		data, err := jsonBytes(result.Receipt)
		if err != nil {
			return err
		}
		if err := ctx.Err(); err != nil {
			return err
		}
		if err = publish([]string{out, receipt}, [][]byte{result.Package, data}); err != nil {
			return err
		}
		return json.NewEncoder(stdout).Encode(map[string]any{"package": out, "receipt": receipt, "bytes": len(result.Package)})
	default:
		return fmt.Errorf("unknown action %q", args[0])
	}
}
func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, os.Args[1:], os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, "ashpkg:", err)
		os.Exit(1)
	}
}
