package packagebuild

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"strings"
	"testing"

	"amigaux.org/imagebuilder/packages"
	"amigaux.org/imagebuilder/sources"
	"amigaux.org/imagebuilder/svr4"
	"amigaux.org/imagebuilder/ufs"
)

func fixture() (packages.Binding, svr4.Info) {
	r := packages.Recipe{FormatVersion: 1, ID: "amiga.test", UpstreamVersion: "1", RecipeRevision: 1, Variant: "m68k", Status: "qualified", Target: packages.Target{Family: "amiga", EnvironmentProfiles: []string{"amiga-3.2"}}, Tier: "environment", Sources: []packages.Source{}, Dependencies: []packages.Dependency{}, Operations: []packages.Operation{{Type: "directory", Path: "Apps", Mode: 0755}, {Type: "file", Path: "Apps/readme", Mode: 0644, Data: "old"}, {Type: "patch", Path: "Apps/readme", SHA256: digest([]byte("old")), Data: "hello\n"}, {Type: "hardlink", Path: "Apps/copy", Target: "Apps/readme"}, {Type: "symlink", Path: "alias", Target: "Apps/readme"}}, State: packages.State{Scope: "environment", WritablePaths: []string{"Prefs"}, PreserveOnUpgrade: true}, Verification: packages.Verification{RequiredPaths: []string{"Apps/readme"}, RuntimeQualified: true}, Metadata: packages.Metadata{Format: "amiga", Entries: []packages.MetadataEntry{}}}
	return packages.Binding{EnvironmentID: "test", Tier: "environment", RecipeSHA256: packages.HashRecipe(r), Recipe: r}, svr4.Info{Package: "ASHtest", Name: "Test application", Version: "1", Architecture: "m68k", BaseDir: "/amiga/apps/test", Timestamp: 42}
}
func TestBuild(t *testing.T) {
	b, info := fixture()
	out, err := Build(context.Background(), b, nil, info, sources.Limits{})
	if err != nil {
		t.Fatal(err)
	}
	_, es, err := svr4.Import(context.Background(), bytes.NewReader(out.Package), int64(len(out.Package)), svr4.Options{})
	if err != nil || len(es) != 4 {
		t.Fatal(err, len(es))
	}
	if err := packages.ValidateReceipt(out.Receipt); err != nil {
		t.Fatal(err)
	}
	h := sha256.Sum256(out.Package)
	if got := hex.EncodeToString(h[:]); got != "a03eefafe7d228b1482073bb88cb02926a3c8e785b179f9655124d94366fb56c" {
		t.Fatalf("package byte regression: %s", got)
	}
	out2, err := Build(context.Background(), b, nil, info, sources.Limits{})
	if err != nil || !bytes.Equal(out.Package, out2.Package) {
		t.Fatal("non-deterministic", err)
	}
}
func TestSourceAndExtraction(t *testing.T) {
	_, info := fixture()
	var buf bytes.Buffer
	if err := svr4.Emit(context.Background(), &buf, info, []ufs.Entry{{Path: "/readme", Kind: 'f', Mode: 0644, Data: strings.NewReader("hello"), Size: 5}}); err != nil {
		t.Fatal(err)
	}
	raw := buf.Bytes()
	b, _ := fixture()
	b.Recipe.Sources = []packages.Source{{ID: "source", Format: "svr4", Size: int64(len(raw)), SHA256: digest(raw), Location: "user", Provenance: "fixture", Redistribution: "user-supplied"}}
	b.Recipe.Operations = []packages.Operation{{Type: "extract", Source: "source", Path: "Apps"}}
	b.RecipeSHA256 = packages.HashRecipe(b.Recipe)
	inputs := map[string]Input{digest(raw): {Reader: bytes.NewReader(raw), Size: int64(len(raw))}}
	out, err := Build(context.Background(), b, inputs, info, sources.Limits{})
	if err != nil {
		t.Fatal(err)
	}
	if len(out.Entries) != 2 {
		t.Fatal(len(out.Entries))
	}
	inputs[digest(raw)] = Input{Reader: bytes.NewReader(append([]byte{1}, raw[1:]...)), Size: int64(len(raw))}
	if _, err := Build(context.Background(), b, inputs, info, sources.Limits{}); err == nil {
		t.Fatal("bad hash accepted")
	}
}
func TestRefusals(t *testing.T) {
	cases := []struct {
		name   string
		change func(*packages.Binding)
	}{{"planned", func(b *packages.Binding) { b.Recipe.Status = "planned" }}, {"patch precondition", func(b *packages.Binding) { b.Recipe.Operations[2].SHA256 = strings.Repeat("0", 64) }}, {"metadata", func(b *packages.Binding) {
		b.Recipe.Metadata.Entries = []packages.MetadataEntry{{Path: "Apps/readme", Comment: "retain me"}}
	}}, {"required file", func(b *packages.Binding) { b.Recipe.Verification.RequiredPaths = []string{"missing"} }}, {"shared state", func(b *packages.Binding) {
		b.Recipe.Shareable = true
		b.Tier = "system"
		b.Recipe.State.WritablePaths = []string{"Apps/readme"}
	}}, {"symlink parent", func(b *packages.Binding) {
		b.Recipe.Operations = append(b.Recipe.Operations, packages.Operation{Type: "file", Path: "alias/escape", Data: "bad"})
	}}}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			b, info := fixture()
			c.change(&b)
			b.RecipeSHA256 = packages.HashRecipe(b.Recipe)
			out, err := Build(context.Background(), b, nil, info, sources.Limits{})
			if err == nil || len(out.Package) > 0 {
				t.Fatal("failure published package", err)
			}
		})
	}
}
func TestLimitsAndCancellation(t *testing.T) {
	b, info := fixture()
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := Build(ctx, b, nil, info, sources.Limits{}); err == nil {
		t.Fatal("ignored cancellation")
	}
	if _, err := Build(context.Background(), b, nil, info, sources.Limits{ExpandedBytes: 2}); err == nil {
		t.Fatal("ignored byte limit")
	}
	if _, err := Build(context.Background(), b, nil, info, sources.Limits{Entries: 1}); err == nil {
		t.Fatal("ignored entry limit")
	}
}

func TestRejectsUnrepresentableNames(t *testing.T) {
	_, info := fixture()
	var buf bytes.Buffer
	if err := svr4.Emit(context.Background(), &buf, info, []ufs.Entry{{Path: "/caf\x8e", Kind: 'f', Mode: 0644, Data: strings.NewReader("x"), Size: 1}}); err != nil {
		t.Fatal(err)
	}
	raw := buf.Bytes()
	b, _ := fixture()
	b.Recipe.Sources = []packages.Source{{ID: "source", Format: "svr4", Size: int64(len(raw)), SHA256: digest(raw), Location: "user", Provenance: "fixture", Redistribution: "user-supplied"}}
	b.Recipe.Operations = []packages.Operation{{Type: "extract", Source: "source", Path: "Apps"}}
	b.Recipe.Verification.RequiredPaths = []string{}
	b.RecipeSHA256 = packages.HashRecipe(b.Recipe)
	out, err := Build(context.Background(), b, map[string]Input{digest(raw): {Reader: bytes.NewReader(raw), Size: int64(len(raw))}}, info, sources.Limits{})
	if err == nil || len(out.Package) != 0 {
		t.Fatal("lossy name accepted")
	}
}
