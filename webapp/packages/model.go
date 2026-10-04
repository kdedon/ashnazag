// Package packages validates portable installation recipes and resolves exact releases.
package packages

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"path"
	"reflect"
	"regexp"
	"sort"
	"strings"
	"unicode/utf8"
)

type Target struct {
	Family              string   `json:"family"`
	EnvironmentProfiles []string `json:"environmentProfiles"`
	CPUs                []string `json:"cpus,omitempty"`
	Capabilities        []string `json:"capabilities,omitempty"`
}
type Source struct {
	ID             string `json:"id"`
	Format         string `json:"format"`
	Size           int64  `json:"size"`
	SHA256         string `json:"sha256"`
	Location       string `json:"location"`
	Provenance     string `json:"provenance"`
	Redistribution string `json:"redistribution"`
}
type Dependency struct {
	ID      string `json:"id"`
	Version string `json:"version"`
	Variant string `json:"variant"`
	Tier    string `json:"tier"`
}
type Selection = Dependency
type Operation struct {
	Type   string `json:"type"`
	Source string `json:"source,omitempty"`
	Path   string `json:"path"`
	Target string `json:"target,omitempty"`
	Mode   uint32 `json:"mode,omitempty"`
	Data   string `json:"data,omitempty"`
	SHA256 string `json:"sha256,omitempty"`
}
type State struct {
	Scope             string   `json:"scope"`
	WritablePaths     []string `json:"writablePaths"`
	PreserveOnUpgrade bool     `json:"preserveOnUpgrade"`
}
type Resources struct {
	InstalledBytes int64 `json:"installedBytes"`
	TemporaryBytes int64 `json:"temporaryBytes"`
	MemoryBytes    int64 `json:"memoryBytes"`
}
type Verification struct {
	RequiredPaths    []string `json:"requiredPaths"`
	RuntimeQualified bool     `json:"runtimeQualified"`
}
type MetadataEntry struct {
	Path       string `json:"path"`
	Protection uint32 `json:"protection,omitempty"`
	Comment    string `json:"comment,omitempty"`
}
type Metadata struct {
	Format  string          `json:"format"`
	Entries []MetadataEntry `json:"entries"`
}
type Recipe struct {
	FormatVersion   int          `json:"formatVersion"`
	ID              string       `json:"id"`
	UpstreamVersion string       `json:"upstreamVersion"`
	RecipeRevision  int          `json:"recipeRevision"`
	Variant         string       `json:"variant"`
	Status          string       `json:"status"`
	Target          Target       `json:"target"`
	Tier            string       `json:"tier"`
	Shareable       bool         `json:"shareable"`
	Sources         []Source     `json:"sources"`
	Dependencies    []Dependency `json:"dependencies"`
	Operations      []Operation  `json:"operations"`
	State           State        `json:"state"`
	Resources       Resources    `json:"resources"`
	Verification    Verification `json:"verification"`
	Metadata        Metadata     `json:"metadata"`
}
type Request struct {
	EnvironmentID string      `json:"environmentID"`
	Profile       string      `json:"profile"`
	Family        string      `json:"family"`
	CPU           string      `json:"cpu"`
	Capabilities  []string    `json:"capabilities"`
	Packages      []Selection `json:"packages"`
}
type Binding struct {
	EnvironmentID string `json:"environmentID"`
	Tier          string `json:"tier"`
	RecipeSHA256  string `json:"recipeSHA256"`
	Recipe        Recipe `json:"recipe"`
}
type Lock struct {
	FormatVersion       int       `json:"formatVersion"`
	LayoutPolicyVersion int       `json:"layoutPolicyVersion"`
	Bindings            []Binding `json:"bindings"`
	Artifacts           []Source  `json:"artifacts"`
	Warnings            []string  `json:"warnings"`
}
type InstalledFile struct {
	Path      string   `json:"path"`
	SHA256    string   `json:"sha256"`
	Preserved bool     `json:"preserved"`
	Metadata  Metadata `json:"metadata"`
}
type Receipt struct {
	FormatVersion int             `json:"formatVersion"`
	EnvironmentID string          `json:"environmentID"`
	PackageID     string          `json:"packageID"`
	Version       string          `json:"version"`
	Tier          string          `json:"tier"`
	RecipeSHA256  string          `json:"recipeSHA256"`
	Files         []InstalledFile `json:"files"`
	Verified      bool            `json:"verified"`
}

var safeID = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,63}$`)
var digest = regexp.MustCompile(`^[0-9a-f]{64}$`)
var dosName = regexp.MustCompile("^[A-Za-z0-9!#$%&'()@^_`{}~-]{1,8}(\\.[A-Za-z0-9!#$%&'()@^_`{}~-]{1,3})?$")

func relative(p string) bool {
	return p != "" && p != "." && !strings.HasPrefix(p, "/") && path.Clean(p) == p && p != ".." && !strings.HasPrefix(p, "../") && !strings.ContainsAny(p, "\\\x00\r\n\t")
}
func tier(t string) bool { return t == "environment" || t == "account" || t == "system" }
func contains(xs []string, s string) bool {
	for _, x := range xs {
		if x == s {
			return true
		}
	}
	return false
}
func HashRecipe(r Recipe) string {
	b, _ := json.Marshal(r)
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}
func RequireExecutable(r Recipe) error {
	if err := ValidateRecipe(r); err != nil {
		return err
	}
	if r.Status != "qualified" {
		return fmt.Errorf("%s: %s recipe cannot execute", r.ID, r.Status)
	}
	return nil
}
func DecodeRecipe(data []byte) (Recipe, error) {
	var r Recipe
	if err := ValidateJSON("recipe", data); err != nil {
		return r, err
	}
	if err := json.Unmarshal(data, &r); err != nil {
		return r, err
	}
	return r, ValidateRecipe(r)
}
func ValidateRecipe(r Recipe) error {
	b, err := recordJSON(r)
	if err != nil {
		return err
	}
	if err = ValidateJSON("recipe", b); err != nil {
		return err
	}
	fail := func(s string) error { return fmt.Errorf("%s: %s", r.ID, s) }
	if !r.Shareable && r.Tier != "environment" {
		return fail("non-shareable payload requires environment tier")
	}
	if r.State.Scope != "environment" {
		return fail("writable state must be environment-local")
	}
	if r.Status == "qualified" && (len(r.Target.EnvironmentProfiles) == 0 || !r.Verification.RuntimeQualified || len(r.Operations) == 0 || r.UpstreamVersion == "UNRESOLVED") {
		return fail("qualified recipe requires profiles, operations, exact version and runtime qualification")
	}
	formats := map[string]string{"mac": "appledouble", "amiga": "amiga", "tos": "dos83"}
	if r.Metadata.Format != "none" && r.Metadata.Format != formats[r.Target.Family] {
		return fail("metadata format does not match guest family")
	}
	ids := map[string]bool{}
	for _, s := range r.Sources {
		if ids[s.ID] {
			return fail("duplicate source id")
		}
		ids[s.ID] = true
		if !digest.MatchString(s.SHA256) || s.Size <= 0 {
			return fail("source requires positive size and SHA-256")
		}
	}
	paths := map[string]bool{}
	fold := map[string]string{}
	check := func(p string) error {
		if !relative(p) {
			return fail("unsafe relative path: " + p)
		}
		if r.Target.Family == "tos" {
			for _, part := range strings.Split(p, "/") {
				if !dosName.MatchString(part) {
					return fail("invalid TOS 8.3 path: " + p)
				}
			}
		}
		prefix := ""
		for _, part := range strings.Split(p, "/") {
			if prefix != "" {
				prefix += "/"
			}
			prefix += part
			k := strings.ToLower(prefix)
			if old, ok := fold[k]; ok && old != prefix {
				return fail("case-colliding paths: " + old + " and " + prefix)
			}
			fold[k] = prefix
		}
		return nil
	}
	for _, o := range r.Operations {
		if err := check(o.Path); err != nil {
			return err
		}
		if paths[o.Path] && o.Type != "patch" {
			return fail("conflicting operations: " + o.Path)
		}
		paths[o.Path] = true
		switch o.Type {
		case "extract":
			if !ids[o.Source] {
				return fail("extract references unknown source")
			}
		case "file", "directory":
		case "symlink", "hardlink":
			if !relative(o.Target) {
				return fail("unsafe link target")
			}
		case "patch":
			if !digest.MatchString(o.SHA256) {
				return fail("patch requires input SHA-256")
			}
		default:
			return fail("unknown operation")
		}
		if o.Type != "extract" && o.Source != "" {
			return fail("source only applies to extract")
		}
		if o.Type != "symlink" && o.Type != "hardlink" && o.Target != "" {
			return fail("target only applies to links")
		}
		if o.Type != "file" && o.Type != "patch" && o.Data != "" {
			return fail("data only applies to file and patch")
		}
	}
	kinds := map[string]string{}
	for _, o := range r.Operations {
		kinds[o.Path] = o.Type
	}
	for _, o := range r.Operations {
		for p := path.Dir(o.Path); p != "."; p = path.Dir(p) {
			if kind, ok := kinds[p]; ok && kind != "directory" && kind != "extract" {
				return fail("non-directory operation parent: " + p)
			}
		}
	}
	for _, p := range r.State.WritablePaths {
		if err := check(p); err != nil {
			return err
		}
	}
	for _, p := range r.Verification.RequiredPaths {
		if err := check(p); err != nil {
			return err
		}
	}
	for _, m := range r.Metadata.Entries {
		if err := check(m.Path); err != nil {
			return err
		}
		if r.Metadata.Format != "amiga" && (m.Protection != 0 || m.Comment != "") {
			return fail("Amiga metadata requires amiga format")
		}
	}
	deps := map[string]bool{}
	for _, d := range r.Dependencies {
		k := d.ID + "/" + d.Tier
		if deps[k] {
			return fail("duplicate dependency")
		}
		deps[k] = true
	}
	return nil
}

func Resolve(catalog []Recipe, requests []Request) (Lock, error) {
	out := Lock{FormatVersion: 1, LayoutPolicyVersion: 1, Bindings: []Binding{}, Artifacts: []Source{}, Warnings: []string{}}
	recipes := map[string]Recipe{}
	key := func(id, v, variant string) string { return id + "@" + v + "#" + variant }
	for _, r := range catalog {
		if err := ValidateRecipe(r); err != nil {
			return out, err
		}
		k := key(r.ID, r.UpstreamVersion, r.Variant)
		if _, ok := recipes[k]; ok {
			return out, fmt.Errorf("ambiguous release %s", k)
		}
		recipes[k] = r
	}
	seenEnv := map[string]bool{}
	shared := map[string]string{}
	artifacts := map[string]Source{}
	writes := writeClaims{}
	for _, req := range requests {
		if !safeID.MatchString(req.EnvironmentID) || strings.EqualFold(req.EnvironmentID, "Shared") || seenEnv[req.EnvironmentID] {
			return out, fmt.Errorf("invalid or duplicate environment id %q", req.EnvironmentID)
		}
		seenEnv[req.EnvironmentID] = true
		selected := map[string]string{}
		active := map[string]bool{}
		done := map[string]bool{}
		tiers := map[string][]string{}
		var visit func(Selection) error
		visit = func(s Selection) error {
			if !tier(s.Tier) {
				return fmt.Errorf("invalid tier %q", s.Tier)
			}
			k := key(s.ID, s.Version, s.Variant)
			r, ok := recipes[k]
			if !ok {
				return fmt.Errorf("missing exact release %s", k)
			}
			if err := RequireExecutable(r); err != nil {
				return err
			}
			if !r.Shareable && s.Tier != "environment" {
				return fmt.Errorf("%s is private to an environment", k)
			}
			if r.Target.Family != req.Family || !contains(r.Target.EnvironmentProfiles, req.Profile) || (len(r.Target.CPUs) > 0 && !contains(r.Target.CPUs, req.CPU)) {
				return fmt.Errorf("%s incompatible with environment %s", k, req.EnvironmentID)
			}
			for _, cap := range r.Target.Capabilities {
				if !contains(req.Capabilities, cap) {
					return fmt.Errorf("%s requires capability %s", k, cap)
				}
			}
			identity := s.ID + "/" + s.Tier
			if old, ok := selected[identity]; ok && old != k {
				return fmt.Errorf("conflicting versions of %s in %s", s.ID, req.EnvironmentID)
			}
			selected[identity] = k
			if s.Tier != "environment" {
				sk := req.Family + "/" + identity
				if old, ok := shared[sk]; ok && old != k {
					return fmt.Errorf("conflicting shared versions of %s", s.ID)
				}
				shared[sk] = k
			}
			if active[identity] {
				return fmt.Errorf("dependency cycle at %s", s.ID)
			}
			if done[identity] {
				return nil
			}
			active[identity] = true
			for _, d := range r.Dependencies {
				if err := visit(d); err != nil {
					return err
				}
			}
			for _, o := range r.Operations {
				if err := writes.add(writeScope(req.Family, s.Tier, req.EnvironmentID), k, o); err != nil {
					return err
				}
			}
			for _, src := range r.Sources {
				if old, ok := artifacts[src.SHA256]; ok && (old.Size != src.Size || old.Format != src.Format) {
					return fmt.Errorf("inconsistent artifact %s", src.SHA256)
				}
				artifacts[src.SHA256] = src
			}
			out.Bindings = append(out.Bindings, Binding{req.EnvironmentID, s.Tier, HashRecipe(r), r})
			tiers[s.ID] = append(tiers[s.ID], s.Tier)
			active[identity] = false
			done[identity] = true
			return nil
		}
		for _, s := range req.Packages {
			if err := visit(s); err != nil {
				return out, err
			}
		}
		for id, ts := range tiers {
			if len(ts) > 1 {
				sort.Strings(ts)
				out.Warnings = append(out.Warnings, fmt.Sprintf("%s: %s occurs in %s; environment overrides account overrides system", req.EnvironmentID, id, strings.Join(ts, ", ")))
			}
		}
	}
	for _, s := range artifacts {
		out.Artifacts = append(out.Artifacts, s)
	}
	sort.Slice(out.Artifacts, func(i, j int) bool { return out.Artifacts[i].SHA256 < out.Artifacts[j].SHA256 })
	sort.Strings(out.Warnings)
	return out, nil
}

func DecodeLock(data []byte) (Lock, error) {
	var l Lock
	if err := ValidateJSON("lock", data); err != nil {
		return l, err
	}
	if err := json.Unmarshal(data, &l); err != nil {
		return l, err
	}
	return l, ValidateLock(l)
}
func ValidateLock(l Lock) error {
	b, err := recordJSON(l)
	if err != nil {
		return err
	}
	if err := ValidateJSON("lock", b); err != nil {
		return err
	}
	bindings := map[string]Binding{}
	shared := map[string]string{}
	writes := writeClaims{}
	artifacts := map[string]Source{}
	for _, s := range l.Artifacts {
		if _, ok := artifacts[s.SHA256]; ok {
			return fmt.Errorf("duplicate locked artifact")
		}
		artifacts[s.SHA256] = s
	}
	for _, binding := range l.Bindings {
		if !safeID.MatchString(binding.EnvironmentID) || strings.EqualFold(binding.EnvironmentID, "Shared") {
			return fmt.Errorf("invalid locked environment")
		}
		if err := RequireExecutable(binding.Recipe); err != nil {
			return err
		}
		if HashRecipe(binding.Recipe) != binding.RecipeSHA256 {
			return fmt.Errorf("locked recipe hash mismatch")
		}
		if !binding.Recipe.Shareable && binding.Tier != "environment" {
			return fmt.Errorf("private locked payload in shared tier")
		}
		k := binding.EnvironmentID + "/" + binding.Tier + "/" + binding.Recipe.ID
		if _, ok := bindings[k]; ok {
			return fmt.Errorf("duplicate locked package")
		}
		bindings[k] = binding
		r := binding.Recipe
		if binding.Tier != "environment" {
			identity := r.Target.Family + "/" + binding.Tier + "/" + r.ID
			if old, ok := shared[identity]; ok && old != binding.RecipeSHA256 {
				return fmt.Errorf("conflicting shared locked releases")
			}
			shared[identity] = binding.RecipeSHA256
		}
		for _, o := range r.Operations {
			if err := writes.add(writeScope(r.Target.Family, binding.Tier, binding.EnvironmentID), binding.RecipeSHA256, o); err != nil {
				return err
			}
		}
		for _, source := range binding.Recipe.Sources {
			s, ok := artifacts[source.SHA256]
			if !ok || s.Size != source.Size || s.Format != source.Format {
				return fmt.Errorf("missing or inconsistent locked source")
			}
		}
	}
	for _, binding := range l.Bindings {
		for _, dep := range binding.Recipe.Dependencies {
			k := binding.EnvironmentID + "/" + dep.Tier + "/" + dep.ID
			d, ok := bindings[k]
			if !ok || d.Recipe.UpstreamVersion != dep.Version || d.Recipe.Variant != dep.Variant {
				return fmt.Errorf("missing locked dependency %s", dep.ID)
			}
		}
	}
	active := map[string]bool{}
	done := map[string]bool{}
	var visit func(string) error
	visit = func(k string) error {
		if active[k] {
			return fmt.Errorf("locked dependency cycle")
		}
		if done[k] {
			return nil
		}
		active[k] = true
		b := bindings[k]
		for _, d := range b.Recipe.Dependencies {
			if err := visit(b.EnvironmentID + "/" + d.Tier + "/" + d.ID); err != nil {
				return err
			}
		}
		active[k] = false
		done[k] = true
		return nil
	}
	for k := range bindings {
		if err := visit(k); err != nil {
			return err
		}
	}
	return nil
}
func DecodeReceipt(data []byte) (Receipt, error) {
	var r Receipt
	if err := ValidateJSON("receipt", data); err != nil {
		return r, err
	}
	if err := json.Unmarshal(data, &r); err != nil {
		return r, err
	}
	return r, ValidateReceipt(r)
}
func ValidateReceipt(r Receipt) error {
	b, err := recordJSON(r)
	if err != nil {
		return err
	}
	if err := ValidateJSON("receipt", b); err != nil {
		return err
	}
	if strings.EqualFold(r.EnvironmentID, "Shared") {
		return fmt.Errorf("reserved environment")
	}
	seen := map[string]bool{}
	for _, f := range r.Files {
		if !relative(f.Path) || seen[f.Path] {
			return fmt.Errorf("unsafe or duplicate installed path")
		}
		seen[f.Path] = true
		for _, m := range f.Metadata.Entries {
			if !relative(m.Path) {
				return fmt.Errorf("unsafe metadata path")
			}
		}
	}
	return nil
}

func recordJSON(v any) ([]byte, error) {
	var valid func(reflect.Value) bool
	valid = func(x reflect.Value) bool {
		switch x.Kind() {
		case reflect.String:
			return utf8.ValidString(x.String())
		case reflect.Struct:
			for i := 0; i < x.NumField(); i++ {
				if !valid(x.Field(i)) {
					return false
				}
			}
		case reflect.Slice, reflect.Array:
			for i := 0; i < x.Len(); i++ {
				if !valid(x.Index(i)) {
					return false
				}
			}
		}
		return true
	}
	if !valid(reflect.ValueOf(v)) {
		return nil, fmt.Errorf("record strings must be valid UTF-8")
	}
	return json.Marshal(v)
}

// Each shared tier has one namespace per guest family across environments.
func writeScope(family, tier, environment string) string {
	scope := family + "/" + tier
	if tier == "environment" {
		scope += "/" + environment
	}
	return scope
}

type writeNode struct {
	owner     string
	directory bool
	children  map[string]*writeNode
}
type writeClaims map[string]*writeNode

func (claims writeClaims) add(scope, owner string, o Operation) error {
	node := claims[scope]
	if node == nil {
		node = &writeNode{}
		claims[scope] = node
	}
	for _, part := range strings.Split(strings.ToLower(o.Path), "/") {
		if node.owner != "" && !node.directory {
			return fmt.Errorf("non-directory write ancestor of %s", o.Path)
		}
		if node.children == nil {
			node.children = map[string]*writeNode{}
		}
		child := node.children[part]
		if child == nil {
			child = &writeNode{}
			node.children[part] = child
		}
		node = child
	}
	if node.owner != "" && node.owner != owner {
		return fmt.Errorf("conflicting writes to %s", o.Path)
	}
	directory := o.Type == "directory" || o.Type == "extract"
	if !directory && len(node.children) > 0 {
		return fmt.Errorf("non-directory write shadows descendants of %s", o.Path)
	}
	node.owner = owner
	node.directory = directory
	return nil
}
