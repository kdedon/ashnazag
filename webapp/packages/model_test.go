package packages

import (
	"encoding/json"
	"strings"
	"testing"
)

func fixture(id, version string) Recipe {
	return Recipe{FormatVersion: 1, ID: id, UpstreamVersion: version, RecipeRevision: 1, Variant: "m68k", Status: "qualified", Target: Target{Family: "amiga", EnvironmentProfiles: []string{"amiga-3.2"}}, Tier: "environment", Sources: []Source{}, Dependencies: []Dependency{}, Operations: []Operation{{Type: "file", Path: id, Data: "payload", Mode: 0644}}, State: State{Scope: "environment", WritablePaths: []string{}, PreserveOnUpgrade: true}, Verification: Verification{RequiredPaths: []string{id}, RuntimeQualified: true}, Metadata: Metadata{Format: "amiga", Entries: []MetadataEntry{}}}
}
func selection(id, version string) Selection {
	return Selection{ID: id, Version: version, Variant: "m68k", Tier: "environment"}
}
func request(env string, ss ...Selection) Request {
	return Request{EnvironmentID: env, Family: "amiga", Profile: "amiga-3.2", Packages: ss}
}
func TestPlanned(t *testing.T) {
	r, e := IBrowse()
	if e != nil {
		t.Fatal(e)
	}
	if RequireExecutable(r) == nil {
		t.Fatal("planned executed")
	}
	if _, e := Resolve([]Recipe{r}, []Request{request("a", Selection{r.ID, r.UpstreamVersion, r.Variant, r.Tier})}); e == nil {
		t.Fatal("planned resolved")
	}
}
func TestClosureIsPerEnvironment(t *testing.T) {
	a := fixture("app", "1")
	a.Dependencies = []Dependency{selection("lib", "1")}
	b := fixture("app", "2")
	b.Dependencies = []Dependency{selection("lib", "2")}
	l, e := Resolve([]Recipe{a, b, fixture("lib", "1"), fixture("lib", "2")}, []Request{request("first", selection("app", "1")), request("second", selection("app", "2"))})
	if e != nil {
		t.Fatal(e)
	}
	if len(l.Bindings) != 4 {
		t.Fatal(l)
	}
	if e := ValidateLock(l); e != nil {
		t.Fatal(e)
	}
	if l.Bindings[0].Recipe.ID != "lib" {
		t.Fatal("dependencies must precede dependents")
	}
	l.Bindings[0].Recipe.Operations[0].Data = "tampered"
	if ValidateLock(l) == nil {
		t.Fatal("tamper accepted")
	}
}
func TestResolverFailures(t *testing.T) {
	base := fixture("app", "1")
	lib := fixture("lib", "1")
	cases := []struct {
		name    string
		catalog []Recipe
		req     []Request
	}{
		{"conflict", []Recipe{base, fixture("app", "2")}, []Request{request("a", selection("app", "1"), selection("app", "2"))}},
		{"missing", []Recipe{base}, []Request{request("a", selection("app", "2"))}},
		{"duplicate env", []Recipe{base}, []Request{request("a"), request("a")}},
		{"reserved env", []Recipe{base}, []Request{request("Shared")}},
		{"private shared", []Recipe{base}, []Request{request("a", Selection{"app", "1", "m68k", "system"})}},
	}
	base.Dependencies = []Dependency{selection("lib", "1")}
	lib.Dependencies = []Dependency{selection("app", "1")}
	cases = append(cases, struct {
		name    string
		catalog []Recipe
		req     []Request
	}{"cycle", []Recipe{base, lib}, []Request{request("a", selection("app", "1"))}})
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if _, e := Resolve(c.catalog, c.req); e == nil {
				t.Fatal("accepted")
			}
		})
	}
}
func TestTierOverridesAndSharedConflict(t *testing.T) {
	a, b := fixture("app", "1"), fixture("app", "2")
	a.Shareable = true
	b.Shareable = true
	l, e := Resolve([]Recipe{a, b}, []Request{request("a", Selection{"app", "1", "m68k", "system"}, selection("app", "2"))})
	if e != nil {
		t.Fatal(e)
	}
	if len(l.Warnings) != 1 {
		t.Fatal(l)
	}
	if _, e := Resolve([]Recipe{a, b}, []Request{request("a", Selection{"app", "1", "m68k", "system"}), request("b", Selection{"app", "2", "m68k", "system"})}); e == nil {
		t.Fatal("shared version conflict accepted")
	}
}
func TestCompatibilityAndMetadata(t *testing.T) {
	r := fixture("app", "1")
	r.Target.CPUs = []string{"68040"}
	if _, e := Resolve([]Recipe{r}, []Request{request("a", selection("app", "1"))}); e == nil {
		t.Fatal("CPU ignored")
	}
	r.Target.CPUs = nil
	r.Target.Capabilities = []string{"network"}
	if _, e := Resolve([]Recipe{r}, []Request{request("a", selection("app", "1"))}); e == nil {
		t.Fatal("capability ignored")
	}
	r.Target.Capabilities = nil
	r.Metadata.Entries = []MetadataEntry{{Path: "app", Protection: 42, Comment: "private preferences"}}
	b, _ := json.Marshal(r)
	out, e := DecodeRecipe(b)
	if e != nil || out.Metadata.Entries[0].Protection != 42 {
		t.Fatal(e)
	}
	r.Metadata.Format = "appledouble"
	if ValidateRecipe(r) == nil {
		t.Fatal("wrong metadata accepted")
	}
	r = fixture("app", "1")
	r.Target.Family = "tos"
	r.Metadata.Format = "dos83"
	r.Operations[0].Path = "TOOLONGNAME.EXE"
	if ValidateRecipe(r) == nil {
		t.Fatal("8.3 ignored")
	}
	r.Operations = []Operation{{Type: "file", Path: "APP.EXE"}, {Type: "file", Path: "app.exe"}}
	if ValidateRecipe(r) == nil {
		t.Fatal("case collision ignored")
	}
}
func TestUnsafePathsAndUnknownFields(t *testing.T) {
	for _, p := range []string{"../escape", "/etc/passwd", "a/../b", "a\\b"} {
		r := fixture("app", "1")
		r.Operations[0].Path = p
		if ValidateRecipe(r) == nil {
			t.Fatal(p)
		}
	}
	r := fixture("app", "1")
	b, _ := json.Marshal(r)
	b = []byte(strings.Replace(string(b), `"formatVersion":1`, `"formatVersion":1,"shell":"echo hi"`, 1))
	if _, e := DecodeRecipe(b); e == nil {
		t.Fatal("unknown property accepted")
	}
	if _, e := DecodeRecipe([]byte(`{}`)); e == nil {
		t.Fatal("missing fields accepted")
	}
}
func TestArtifactsDeduplicate(t *testing.T) {
	a, b := fixture("app", "1"), fixture("other", "1")
	s := Source{"media", "lha", 12, strings.Repeat("a", 64), "user", "fixture", "user-supplied"}
	a.Sources = []Source{s}
	b.Sources = []Source{s}
	l, e := Resolve([]Recipe{a, b}, []Request{request("a", selection("app", "1")), request("b", selection("other", "1"))})
	if e != nil {
		t.Fatal(e)
	}
	if len(l.Artifacts) != 1 {
		t.Fatal(l)
	}
}
func TestSchemaCoverage(t *testing.T) {
	allowed := map[string]bool{}
	for _, k := range []string{"$schema", "$id", "$defs", "$ref", "type", "properties", "required", "additionalProperties", "items", "uniqueItems", "enum", "const", "minimum", "maximum", "minLength", "maxLength", "pattern"} {
		allowed[k] = true
	}
	var walk func(map[string]any)
	walk = func(s map[string]any) {
		for k, v := range s {
			if !allowed[k] {
				t.Fatalf("unimplemented schema keyword %s", k)
			}
			switch k {
			case "properties", "$defs":
				for _, x := range v.(map[string]any) {
					walk(x.(map[string]any))
				}
			case "items":
				walk(v.(map[string]any))
			}
		}
	}
	for _, kind := range []string{"recipe", "lock", "receipt"} {
		b, e := Schema(kind)
		if e != nil {
			t.Fatal(e)
		}
		var s map[string]any
		json.Unmarshal(b, &s)
		walk(s)
	}
}

func TestRejectsInvalidUTF8(t *testing.T) {
	r := fixture("app", "1")
	r.Operations[0].Data = "\xff"
	if ValidateRecipe(r) == nil {
		t.Fatal("lossy recipe data accepted")
	}
	if ValidateJSON("recipe", []byte("{\"id\":\"\xff\"}")) == nil {
		t.Fatal("invalid UTF-8 JSON accepted")
	}
}

func operationRecipe(id, p, kind string) Recipe {
	r := fixture(id, "1")
	r.Operations = []Operation{{Type: kind, Path: p, Mode: 0755}}
	r.Verification.RequiredPaths = []string{p}
	return r
}
func operationLock(a, b Recipe, envA, envB, tier string) Lock {
	return Lock{FormatVersion: 1, LayoutPolicyVersion: 1, Bindings: []Binding{{envA, tier, HashRecipe(a), a}, {envB, tier, HashRecipe(b), b}}, Artifacts: []Source{}, Warnings: []string{}}
}
func TestWriteAncestorConflicts(t *testing.T) {
	for _, parentKind := range []string{"file", "symlink", "hardlink"} {
		for _, reverse := range []bool{false, true} {
			a := operationRecipe("parent", "App", parentKind)
			if parentKind != "file" {
				a.Operations[0].Target = "target"
			}
			b := operationRecipe("child", "app/config", "file")
			if reverse {
				a, b = b, a
			}
			if _, e := Resolve([]Recipe{a, b}, []Request{request("a", selection(a.ID, "1"), selection(b.ID, "1"))}); e == nil {
				t.Fatalf("resolver accepted %s reverse=%v", parentKind, reverse)
			}
			if e := ValidateLock(operationLock(a, b, "a", "a", "environment")); e == nil {
				t.Fatalf("lock accepted %s reverse=%v", parentKind, reverse)
			}
		}
	}
}
func TestDirectoryParentAllowsChildren(t *testing.T) {
	a := operationRecipe("directory", "App", "directory")
	b := operationRecipe("child", "App/config", "file")
	c := operationRecipe("other", "App/data", "file")
	for _, recipes := range [][]Recipe{{a, b, c}, {b, c, a}} {
		ss := []Selection{}
		for _, r := range recipes {
			ss = append(ss, selection(r.ID, "1"))
		}
		lock, e := Resolve(recipes, []Request{request("a", ss...)})
		if e != nil {
			t.Fatal(e)
		}
		if e := ValidateLock(lock); e != nil {
			t.Fatal(e)
		}
	}
}
func TestWriteScopesAcrossEnvironments(t *testing.T) {
	a := operationRecipe("parent", "App", "file")
	b := operationRecipe("child", "app/config", "file")
	a.Shareable = true
	b.Shareable = true
	for _, tier := range []string{"environment", "account", "system"} {
		sa, sb := selection(a.ID, "1"), selection(b.ID, "1")
		sa.Tier = tier
		sb.Tier = tier
		_, err := Resolve([]Recipe{a, b}, []Request{request("a", sa), request("b", sb)})
		locked := ValidateLock(operationLock(a, b, "a", "b", tier))
		if tier == "environment" {
			if err != nil || locked != nil {
				t.Fatalf("private environments conflict: %v %v", err, locked)
			}
		} else if err == nil || locked == nil {
			t.Fatalf("shared %s ancestor accepted: %v %v", tier, err, locked)
		}
	}
	for _, tier := range []string{"account", "system"} {
		sa := selection(a.ID, "1")
		sa.Tier = tier
		lock, e := Resolve([]Recipe{a}, []Request{request("a", sa), request("b", sa)})
		if e != nil {
			t.Fatal(e)
		}
		if e := ValidateLock(lock); e != nil {
			t.Fatal(e)
		}
	}
}
func TestSharedFamilyNamespacesAreSeparate(t *testing.T) {
	a := operationRecipe("amigaapp", "APP", "file")
	b := operationRecipe("macapp", "APP/Config", "file")
	a.Shareable = true
	b.Shareable = true
	b.Target.Family = "mac"
	b.Metadata.Format = "appledouble"
	b.Target.EnvironmentProfiles = []string{"mac-8.1"}
	sa, sb := selection(a.ID, "1"), selection(b.ID, "1")
	sa.Tier = "system"
	sb.Tier = "system"
	reqB := Request{EnvironmentID: "mac", Family: "mac", Profile: "mac-8.1", Packages: []Selection{sb}}
	lock, e := Resolve([]Recipe{a, b}, []Request{request("amiga", sa), reqB})
	if e != nil {
		t.Fatal(e)
	}
	if e := ValidateLock(lock); e != nil {
		t.Fatal(e)
	}
}
