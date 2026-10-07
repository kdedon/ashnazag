// Package forge defines machine presets and exportable build recipes.
package forge

import (
	"bytes"
	"context"
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"reflect"
	"sort"
	"strings"

	"amigaux.org/imagebuilder/amixtape"
	"amigaux.org/imagebuilder/packages"
	"amigaux.org/imagebuilder/planner"
	"amigaux.org/imagebuilder/provision"
)

const (
	FormatVersion = 2
	Version       = "0.1.0"
	// RecipePath is where every built image carries its recipe.
	RecipePath = "/etc/forge/recipe.json"
)

var (
	//go:embed presets.json
	presetsJSON []byte
	//go:embed preset.schema.json
	presetSchema []byte
	//go:embed recipe.schema.json
	recipeSchema []byte
)

type Settings struct {
	Devices []string `json:"devices"`
	// OptionalDevices may be added to Devices.
	OptionalDevices []string `json:"optionalDevices,omitempty"`
	RootMiB int      `json:"rootMiB,omitempty"`
	SwapMiB int      `json:"swapMiB,omitempty"`
	DiskMiB int      `json:"diskMiB,omitempty"`
}

type Preset struct {
	ID          string `json:"id"`
	Revision    int    `json:"revision"`
	Label       string `json:"label"`
	Description string `json:"description"`
	Status      string `json:"status"`
	// Runnable is set once the machine boots from a forge-built image.
	Runnable bool     `json:"runnable"`
	Machine  string   `json:"machine"`
	Recipe   string   `json:"recipe,omitempty"`
	Settings Settings `json:"settings"`
}

type PresetRef struct {
	ID       string `json:"id"`
	Revision int    `json:"revision"`
}

// Package names a provisioning package; its bytes are an input.
type Package struct {
	Kind   string `json:"kind"`
	Family string `json:"family"`
	ID     string `json:"id"`
}

// Changes holds only what differs from the preset.
type Changes struct {
	Devices   *[]string `json:"devices,omitempty"`
	RootMiB   *int      `json:"rootMiB,omitempty"`
	SwapMiB   *int      `json:"swapMiB,omitempty"`
	Provision []Package `json:"provision,omitempty"`
}

type Input struct {
	Role   string `json:"role"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
	// Parts are the files the tape came in; they do not affect the image.
	Parts []amixtape.Digest `json:"parts,omitempty"`
}

type Recipe struct {
	FormatVersion int           `json:"formatVersion"`
	ForgeVersion  string        `json:"forgeVersion"`
	Preset        PresetRef     `json:"preset"`
	Changes       Changes       `json:"changes"`
	Lock          packages.Lock `json:"lock"`
	Inputs        []Input       `json:"inputs"`
}

// Selection is the editable state a recipe restores.
type Selection struct {
	Preset    string    `json:"preset"`
	Machine   string    `json:"machine"`
	Settings  Settings  `json:"settings"`
	Provision []Package `json:"provision,omitempty"`
}

var presets []Preset

func init() {
	if err := packages.ValidateSchema(presetSchema, presetsJSON); err != nil {
		panic("presets: " + err.Error())
	}
	var file struct {
		Presets []Preset `json:"presets"`
	}
	if err := json.Unmarshal(presetsJSON, &file); err != nil {
		panic(err)
	}
	presets = file.Presets
	for _, p := range presets {
		if err := checkDevices(p.Machine, append(append([]string{}, p.Settings.Devices...), p.Settings.OptionalDevices...)); err != nil {
			panic("preset " + p.ID + ": " + err.Error())
		}
		if p.Status == "available" && !p.Runnable {
			panic("preset " + p.ID + ": available presets must be runnable")
		}
		if (p.Status == "available") != (p.Recipe != "") {
			panic("preset " + p.ID + ": only available presets name a recipe")
		}
	}
}

func Presets() []Preset { return append([]Preset(nil), presets...) }

func Lookup(id string) (Preset, bool) {
	for _, p := range presets {
		if p.ID == id {
			return p, true
		}
	}
	return Preset{}, false
}

func checkDevices(machine string, devices []string) error {
	for _, m := range planner.GetCatalog().Machines {
		if m.ID != machine {
			continue
		}
		for _, d := range devices {
			if !contains(m.DeviceIDs, d) {
				return fmt.Errorf("device %q is not available on %s", d, m.Label)
			}
		}
		return nil
	}
	return fmt.Errorf("unknown machine %q", machine)
}

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}

func sorted(list []string) []string {
	out := append([]string{}, list...)
	sort.Strings(out)
	return out
}

// TapeSegments are the AMIX tape segments the root is layered from, in order.
var TapeSegments = []string{"02", "03", "10"}

// TapeRole is the AMIX tape input. Its size and digest cover the segments
// used, so any packaging of the same tape gives the same recipe and image.
const TapeRole = "amix-tape"

// Roles lists the inputs a selection consumes, in build order.
func Roles(s Selection) []string {
	roles := []string{TapeRole, "kernel"}
	if p, _ := Lookup(s.Preset); p.Recipe != "falcon-console" {
		roles = append(roles, "boot-donor")
	}
	for _, p := range s.Provision {
		roles = append(roles, "package:"+p.ID)
	}
	return roles
}

// NewRecipe records a selection and its input digests.
func NewRecipe(s Selection, inputs []Input) (Recipe, error) {
	p, ok := Lookup(s.Preset)
	if !ok {
		return Recipe{}, fmt.Errorf("unknown preset %q", s.Preset)
	}
	lock, err := packages.Resolve(nil, nil)
	if err != nil {
		return Recipe{}, err
	}
	r := Recipe{FormatVersion: FormatVersion, ForgeVersion: Version, Preset: PresetRef{p.ID, p.Revision}, Lock: lock, Inputs: append([]Input{}, inputs...)}
	if d := sorted(s.Settings.Devices); !reflect.DeepEqual(d, sorted(p.Settings.Devices)) {
		r.Changes.Devices = &d
	}
	if v := s.Settings.RootMiB; v != p.Settings.RootMiB {
		r.Changes.RootMiB = &v
	}
	if v := s.Settings.SwapMiB; v != p.Settings.SwapMiB {
		r.Changes.SwapMiB = &v
	}
	r.Changes.Provision = append([]Package(nil), s.Provision...)
	if _, err := r.Selection(); err != nil {
		return Recipe{}, err
	}
	return r, nil
}

// Selection applies the recipe's changes to its preset.
func (r Recipe) Selection() (Selection, error) {
	p, ok := Lookup(r.Preset.ID)
	if !ok {
		return Selection{}, fmt.Errorf("unknown preset %q", r.Preset.ID)
	}
	if r.Preset.Revision > p.Revision {
		return Selection{}, fmt.Errorf("preset %s revision %d is newer than this forge supports (%d)", p.ID, r.Preset.Revision, p.Revision)
	}
	s := Selection{Preset: p.ID, Machine: p.Machine, Settings: p.Settings, Provision: r.Changes.Provision}
	s.Settings.Devices = sorted(p.Settings.Devices)
	if r.Changes.Devices != nil {
		s.Settings.Devices = sorted(*r.Changes.Devices)
	}
	if r.Changes.RootMiB != nil {
		s.Settings.RootMiB = *r.Changes.RootMiB
	}
	if r.Changes.SwapMiB != nil {
		s.Settings.SwapMiB = *r.Changes.SwapMiB
	}
	if err := checkDevices(s.Machine, s.Settings.Devices); err != nil {
		return s, err
	}
	if p.Recipe != "" && (s.Settings.RootMiB < 64 || s.Settings.RootMiB > 2048 || s.Settings.RootMiB%4 != 0 || s.Settings.SwapMiB < 4 || s.Settings.SwapMiB > 2048) {
		return s, fmt.Errorf("root must be 64–2048 MiB in multiples of 4 and swap 4–2048 MiB")
	}
	ids := map[string]bool{}
	for _, pkg := range s.Provision {
		if ids[pkg.ID] {
			return s, fmt.Errorf("duplicate package %s", pkg.ID)
		}
		ids[pkg.ID] = true
	}
	roles := Roles(s)
	if len(r.Inputs) != 0 && len(r.Inputs) != len(roles) {
		return s, fmt.Errorf("recipe lists %d inputs; its selection needs %d", len(r.Inputs), len(roles))
	}
	for i, in := range r.Inputs {
		if len(in.Parts) != 0 && in.Role != TapeRole {
			return s, fmt.Errorf("input %d (%s) cannot have parts", i+1, in.Role)
		}
		if in.Role != roles[i] {
			return s, fmt.Errorf("input %d has role %q; expected %q", i+1, in.Role, roles[i])
		}
	}
	return s, nil
}

// Runnable reports why a selection cannot be built yet.
func (s Selection) Runnable() error {
	p, _ := Lookup(s.Preset)
	if p.Status != "available" {
		if p.Runnable {
			return fmt.Errorf("the %s preset preset is planned: it boots, but the forge cannot write its disk layout yet", p.Label)
		}
		return fmt.Errorf("the %s preset is planned and cannot build yet", p.Label)
	}
	var have []string
	for _, d := range s.Settings.Devices {
		if !contains(p.Settings.OptionalDevices, d) {
			have = append(have, d)
		}
	}
	if want := sorted(p.Settings.Devices); !reflect.DeepEqual(have, want) {
		return fmt.Errorf("the supplied kernel requires exactly the default devices: %s", strings.Join(want, ", "))
	}
	return nil
}

// Encode returns the canonical recipe bytes; equal recipes encode identically.
func Encode(r Recipe) ([]byte, error) {
	b, err := json.MarshalIndent(r, "", "  ")
	if err != nil {
		return nil, err
	}
	b = append(b, '\n')
	if _, err := Decode(b); err != nil {
		return nil, err
	}
	return b, nil
}

// Decode validates an exported recipe.
func Decode(data []byte) (Recipe, error) {
	var r Recipe
	if len(data) > 1<<20 {
		return r, fmt.Errorf("recipe exceeds 1 MiB")
	}
	var head struct {
		FormatVersion any `json:"formatVersion"`
	}
	if err := json.Unmarshal(data, &head); err != nil {
		return r, fmt.Errorf("not a forge recipe: %v", err)
	}
	v, ok := head.FormatVersion.(float64)
	if !ok || v < 1 || v != float64(int(v)) {
		return r, fmt.Errorf("not a forge recipe: missing formatVersion")
	}
	if v > FormatVersion {
		return r, fmt.Errorf("recipe format %d is newer than this forge supports (%d); use a newer forge", int(v), FormatVersion)
	}
	if err := packages.ValidateSchema(recipeSchema, data); err != nil {
		return r, fmt.Errorf("invalid recipe: %v", err)
	}
	var raw struct {
		Lock json.RawMessage `json:"lock"`
	}
	if err := json.Unmarshal(data, &raw); err != nil {
		return r, err
	}
	lock, err := packages.DecodeLock(raw.Lock)
	if err != nil {
		return r, fmt.Errorf("invalid recipe lock: %v", err)
	}
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err := d.Decode(&r); err != nil {
		return r, fmt.Errorf("invalid recipe: %v", err)
	}
	r.Lock = lock
	if r.FormatVersion == 1 {
		if err := upgrade(&r); err != nil {
			return r, fmt.Errorf("invalid recipe: %v", err)
		}
	}
	if _, err := r.Selection(); err != nil {
		return r, fmt.Errorf("invalid recipe: %v", err)
	}
	return r, nil
}

// upgrade replaces a format 1 recipe's segment inputs with the tape input.
func upgrade(r *Recipe) error {
	r.FormatVersion = FormatVersion
	if len(r.Inputs) == 0 {
		return nil
	}
	if len(r.Inputs) < len(TapeSegments) {
		return fmt.Errorf("recipe lists %d inputs", len(r.Inputs))
	}
	for i, id := range TapeSegments {
		if r.Inputs[i].Role != "amix-"+id {
			return fmt.Errorf("input %d has role %q; expected %q", i+1, r.Inputs[i].Role, "amix-"+id)
		}
	}
	r.Inputs = append([]Input{tapeInput(r.Inputs[:len(TapeSegments)])}, r.Inputs[len(TapeSegments):]...)
	return nil
}

// Digest hashes one input.
func Digest(ctx context.Context, role string, r io.ReaderAt, size int64) (Input, error) {
	h := sha256.New()
	buf := make([]byte, 1<<20)
	for off := int64(0); off < size; {
		if err := ctx.Err(); err != nil {
			return Input{}, err
		}
		n := int(min(int64(len(buf)), size-off))
		got, err := r.ReadAt(buf[:n], off)
		if got != n {
			if err == nil {
				err = io.ErrUnexpectedEOF
			}
			return Input{}, fmt.Errorf("read %s: %w", role, err)
		}
		h.Write(buf[:n])
		off += int64(n)
	}
	return Input{Role: role, Size: size, SHA256: hex.EncodeToString(h.Sum(nil))}, nil
}

// TapeInput describes the tape from its segments, in TapeSegments order,
// and the parts they came from.
func TapeInput(ctx context.Context, segments []Media, parts []amixtape.Digest) (Input, error) {
	if len(segments) != len(TapeSegments) {
		return Input{}, fmt.Errorf("expected AMIX tape segments %s", strings.Join(TapeSegments, ", "))
	}
	ins := make([]Input, len(segments))
	for i, m := range segments {
		in, err := Digest(ctx, "amix-"+TapeSegments[i], m.Reader, m.Size)
		if err != nil {
			return Input{}, err
		}
		ins[i] = in
	}
	in := tapeInput(ins)
	in.Parts = append([]amixtape.Digest(nil), parts...)
	return in, nil
}

// tapeInput sums the segment sizes and hashes "<id> <sha256>" lines, so
// segment digests from older recipes convert too.
func tapeInput(segments []Input) Input {
	h := sha256.New()
	in := Input{Role: TapeRole}
	for _, s := range segments {
		fmt.Fprintf(h, "%s %s\n", strings.TrimPrefix(s.Role, "amix-"), s.SHA256)
		in.Size += s.Size
	}
	in.SHA256 = hex.EncodeToString(h.Sum(nil))
	return in
}

// Portable drops the tape's part digests, as images carry the recipe.
func (r Recipe) Portable() Recipe {
	r.Inputs = append([]Input(nil), r.Inputs...)
	for i := range r.Inputs {
		r.Inputs[i].Parts = nil
	}
	return r
}

// Mismatches names the roles whose inputs differ between two recipes.
// Tape parts are not compared.
func Mismatches(want, got []Input) []string {
	var out []string
	same := func(a, b Input) bool { return a.Role == b.Role && a.Size == b.Size && a.SHA256 == b.SHA256 }
	for i, w := range want {
		if i >= len(got) || !same(got[i], w) {
			out = append(out, w.Role)
		}
	}
	for i := len(want); i < len(got); i++ {
		out = append(out, got[i].Role)
	}
	return out
}

// Artifacts pairs packages with their recorded inputs for staging.
func (r Recipe) Artifacts(s Selection) []provision.Artifact {
	var out []provision.Artifact
	if len(r.Inputs) != len(Roles(s)) {
		return nil
	}
	base := len(Roles(s)) - len(s.Provision)
	for i, p := range s.Provision {
		in := r.Inputs[base+i]
		out = append(out, provision.Artifact{Kind: p.Kind, Family: p.Family, ID: p.ID, SHA256: in.SHA256, Size: in.Size})
	}
	return out
}
