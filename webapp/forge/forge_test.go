package forge

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"reflect"
	"strings"
	"testing"

	"amigaux.org/imagebuilder/amixtape"
	"amigaux.org/imagebuilder/provision"
	"amigaux.org/imagebuilder/recipes"
	"amigaux.org/imagebuilder/rootfs"
	"amigaux.org/imagebuilder/ufs"
)

var be = binary.BigEndian

func archive(files map[string][]byte, order ...string) []byte {
	var out bytes.Buffer
	pad := func(n int) { out.Write(make([]byte, (4-n%4)%4)) }
	for i, name := range append(order, "TRAILER!!!") {
		data := files[name]
		fields := []int{i + 1, 0o100644, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name) + 1, 0}
		out.WriteString("070701")
		for _, f := range fields {
			fmt.Fprintf(&out, "%08x", f)
		}
		out.WriteString(name + "\x00")
		pad(110 + len(name) + 1)
		out.Write(data)
		pad(len(data))
	}
	return out.Bytes()
}

// Mirrors the synthetic media of the browser worker test.
func fixtureMedia() [][]byte {
	swap := make([]byte, 0x1220)
	for _, p := range [][2]int{{0x1216, 0xe581}, {0x121e, 0xe581}, {0x1034, 0x780b}, {0x1044, 0x780b}, {0x1054, 0x780b}} {
		be.PutUint16(swap[p[0]:], uint16(p[1]))
	}
	files := map[string][]byte{
		"etc/profile":       []byte("TERM=amiga\n\tif sioc\n"),
		"usr/sbin/shutdown": []byte("#!/sbin/sh\nif /usr/amiga/bin/sioc && test x\n"),
		"usr/sbin/rc0":      []byte("#!/sbin/sh\n/sbin/umountall\n"),
		"usr/sbin/rc6":      []byte("#!/sbin/sh\n/sbin/umountall\n"),
		"etc/motd":          []byte("Amiga Version 2.1\n"),
		"etc/screendefs":    []byte("# screens\namiga\n"),
		"etc/group":         []byte("root::0:\ndisplay::99:\n"),
		"etc/vfstab":        []byte("/dev/dsk/c0d0s1 /dev/rdsk/c0d0s1 / s5 1 yes -\n"),
		"usr/sbin/swap":     swap,
	}
	core := archive(files, "etc/profile", "usr/sbin/shutdown", "usr/sbin/rc0", "usr/sbin/rc6", "etc/motd", "etc/screendefs", "etc/group", "etc/vfstab", "usr/sbin/swap")
	kernel := make([]byte, 512)
	copy(kernel, "\x7fELF\x01\x02\x01")
	be.PutUint16(kernel[16:], 2)
	be.PutUint16(kernel[18:], 4)
	be.PutUint32(kernel[20:], 1)
	be.PutUint32(kernel[24:], 0x100000)
	be.PutUint32(kernel[28:], 52)
	be.PutUint16(kernel[40:], 52)
	be.PutUint16(kernel[42:], 32)
	be.PutUint16(kernel[44:], 1)
	for i, v := range []uint32{1, 256, 0x100000, 0x100000, 32, 512, 5, 4} {
		be.PutUint32(kernel[52+4*i:], v)
	}
	for i := 256; i < 288; i++ {
		kernel[i] = 0x4e
	}
	donor := make([]byte, 128*512)
	copy(donor, "ER")
	be.PutUint16(donor[2:], 512)
	be.PutUint32(donor[4:], 128)
	be.PutUint16(donor[16:], 1)
	be.PutUint32(donor[18:], 64)
	be.PutUint16(donor[22:], 32)
	be.PutUint16(donor[24:], 1)
	for i, p := range []struct {
		name, typ     string
		start, blocks uint32
	}{{"Apple", "Apple_partition_map", 1, 63}, {"Macintosh", "Apple_Driver", 64, 32}, {"MacOS", "Apple_HFS", 96, 32}} {
		at := donor[(i+1)*512:]
		copy(at, "PM")
		be.PutUint32(at[4:], 3)
		be.PutUint32(at[8:], p.start)
		be.PutUint32(at[12:], p.blocks)
		copy(at[16:], p.name)
		copy(at[48:], p.typ)
		be.PutUint32(at[84:], p.blocks)
		be.PutUint32(at[88:], 0x33)
	}
	for i := 64 * 512; i < 96*512; i++ {
		donor[i] = 0x42
	}
	pkg, err := os.ReadFile("../svr4/testdata/ASHtest.pkg")
	if err != nil {
		panic(err)
	}
	return [][]byte{core, archive(nil), archive(nil), kernel, donor, pkg}
}

type memFile []byte

func (m memFile) ReadAt(p []byte, off int64) (int, error) {
	if off >= int64(len(m)) {
		return 0, io.EOF
	}
	n := copy(p, m[off:])
	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}
func (m memFile) WriteAt(p []byte, off int64) (int, error) { return copy(m[off:], p), nil }

func media(files [][]byte) []Media {
	out := make([]Media, len(files))
	for i, f := range files {
		out[i] = Media{bytes.NewReader(f), int64(len(f))}
	}
	return out
}

var testParts = []amixtape.Digest{{Size: 7, SHA256: strings.Repeat("ab", 32)}}

func digests(t *testing.T, s Selection, files [][]byte) []Input {
	n := len(TapeSegments)
	tape, err := TapeInput(context.Background(), media(files[:n]), testParts)
	if err != nil {
		t.Fatal(err)
	}
	out := []Input{tape}
	for i, role := range Roles(s)[1:] {
		in, err := Digest(context.Background(), role, bytes.NewReader(files[n+i]), int64(len(files[n+i])))
		if err != nil {
			t.Fatal(err)
		}
		out = append(out, in)
	}
	return out
}

func build(t *testing.T, r Recipe, files [][]byte) []byte {
	s, _ := r.Selection()
	var out bytes.Buffer
	if err := Quadra(context.Background(), r, media(files), make(memFile, s.Settings.RootMiB<<20), &out); err != nil {
		t.Fatal(err)
	}
	return out.Bytes()
}

func testSelection() Selection {
	p, _ := Lookup("quadra800")
	s := Selection{Preset: p.ID, Machine: p.Machine, Settings: p.Settings, Provision: []Package{{"app", "amiga", "test-1"}}}
	s.Settings.SwapMiB = 4
	return s
}

func TestPresets(t *testing.T) {
	want := map[string]string{"quadra800": "available", "falcon030": "planned", "falcon-ct60": "planned", "tt030": "planned", "a4000-040": "planned"}
	for _, p := range Presets() {
		if want[p.ID] != p.Status {
			t.Errorf("%s: status %q", p.ID, p.Status)
		}
		delete(want, p.ID)
		s := Selection{Preset: p.ID, Machine: p.Machine, Settings: p.Settings}
		s.Settings.Devices = sorted(s.Settings.Devices)
		if err := s.Runnable(); (err == nil) != (p.Status == "available") {
			t.Errorf("%s: runnable %v", p.ID, err)
		}
	}
	if len(want) != 0 {
		t.Errorf("missing presets %v", want)
	}
	// The fixed console recipe: default devices, 64 MiB root and swap.
	p, _ := Lookup("quadra800")
	if !reflect.DeepEqual(p.Settings, Settings{Devices: []string{"adb", "framebuffer", "scc", "scsi53c96"}, RootMiB: 64, SwapMiB: 64}) || p.Recipe != "quadra-console" {
		t.Fatalf("quadra preset drifted: %+v", p)
	}
	f, _ := Lookup("falcon030")
	if f.Settings.DiskMiB != 512 || !contains(f.Settings.Devices, "falcon-ide") {
		t.Fatalf("falcon preset: %+v", f)
	}
}

// The preset root equals the fixed recipe's root plus the recipe file.
func TestPresetMatchesFixedRecipe(t *testing.T) {
	files := fixtureMedia()
	ctx := context.Background()
	layers := func() []ufs.Entry {
		var sources []rootfs.Source
		for _, f := range files[:3] {
			sources = append(sources, rootfs.Source{Reader: bytes.NewReader(f), Size: int64(len(f))})
		}
		e, err := rootfs.ScanLayers(ctx, sources, rootfs.Options{})
		if err != nil {
			t.Fatal(err)
		}
		return e
	}
	legacy, err := recipes.QuadraRoot(layers(), bytes.NewReader(files[3]), int64(len(files[3])))
	if err != nil {
		t.Fatal(err)
	}
	s := testSelection()
	r, err := NewRecipe(s, digests(t, s, files))
	if err != nil {
		t.Fatal(err)
	}
	a := r.Artifacts(s)
	if legacy, err = provision.Stage(ctx, legacy, a, []io.ReaderAt{bytes.NewReader(files[5])}); err != nil {
		t.Fatal(err)
	}
	text, _ := Encode(r)
	portable, _ := Encode(r.Portable())
	got, err := Root(ctx, text, layers(), media(files)[3], media(files)[5:])
	if err != nil {
		t.Fatal(err)
	}
	seen := map[string]ufs.Entry{}
	for _, e := range got {
		seen[e.Path] = e
	}
	for _, e := range legacy {
		g, ok := seen[e.Path]
		if !ok || g.Kind != e.Kind || g.Mode != e.Mode || g.UID != e.UID || g.GID != e.GID || g.Size != e.Size || g.Target != e.Target {
			t.Fatalf("%s differs", e.Path)
		}
		delete(seen, e.Path)
	}
	dir, file := seen["/etc/forge"], seen[RecipePath]
	if len(seen) != 2 || dir.Kind != 'd' || file.Mode != 0644 || file.UID != 0 || file.GID != 0 || file.Size != int64(len(portable)) {
		t.Fatalf("unexpected extra entries %v", seen)
	}
}

func TestExportImportRebuild(t *testing.T) {
	files := fixtureMedia()
	s := testSelection()
	r, err := NewRecipe(s, digests(t, s, files))
	if err != nil {
		t.Fatal(err)
	}
	exported, err := Encode(r)
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Contains(exported, []byte("ASHtest")) || bytes.Contains(exported, []byte(".pkg")) {
		t.Fatal("export leaks a file name")
	}
	first := build(t, r, files)
	imported, err := Decode(exported)
	if err != nil {
		t.Fatal(err)
	}
	sel, err := imported.Selection()
	if err != nil || !reflect.DeepEqual(sel, s) {
		t.Fatalf("selection not restored: %+v %v", sel, err)
	}
	again, err := NewRecipe(sel, digests(t, sel, files))
	if err != nil {
		t.Fatal(err)
	}
	if b, _ := Encode(again); !bytes.Equal(b, exported) {
		t.Fatalf("re-export differs:\n%s\n%s", b, exported)
	}
	second := build(t, again, files)
	if !bytes.Equal(first, second) {
		t.Fatal("rebuild is not byte-identical")
	}
	portable, _ := Encode(r.Portable())
	if !bytes.Contains(first, portable) || bytes.Contains(portable, []byte("parts")) || !bytes.Contains(exported, []byte(`"parts"`)) {
		t.Fatal("image does not carry its recipe without tape parts")
	}
	// Another packaging of the same tape gives the same image.
	other, _ := NewRecipe(s, append([]Input{{Role: TapeRole, Size: r.Inputs[0].Size, SHA256: r.Inputs[0].SHA256}}, r.Inputs[1:]...))
	if !bytes.Equal(build(t, other, files), first) || len(Mismatches(r.Inputs, other.Inputs)) != 0 {
		t.Fatal("tape packaging changes the image")
	}
	sum := sha256.Sum256(first)
	t.Logf("image %d bytes, sha256 %s", len(first), hex.EncodeToString(sum[:]))

	files[3] = append([]byte(nil), files[3]...)
	files[3][300] ^= 1
	var out bytes.Buffer
	err = Quadra(context.Background(), again, media(files), make(memFile, 64<<20), &out)
	if err == nil || !strings.Contains(err.Error(), "inputs differ from the recipe: kernel") {
		t.Fatalf("changed kernel accepted: %v", err)
	}
}

func TestDecodeRejects(t *testing.T) {
	s := testSelection()
	r, _ := NewRecipe(s, digests(t, s, fixtureMedia()))
	good, _ := Encode(r)
	for _, c := range []struct{ old, new, want string }{
		{`"formatVersion": 2`, `"formatVersion": 3`, "newer than this forge"},
		{`"formatVersion": 2`, `"formatVersion": "2"`, "missing formatVersion"},
		{`"role": "kernel",`, `"role": "kernel", "parts": [{"size": 1, "sha256": "` + strings.Repeat("0", 64) + `"}],`, "cannot have parts"},
		{`"forgeVersion"`, `"fileName": "x", "forgeVersion"`, "unknown property"},
		{`"id": "quadra800"`, `"id": "nosuch"`, "unknown preset"},
		{`"revision": 1`, `"revision": 9`, "revision 9 is newer"},
		{`"role": "kernel"`, `"role": "donor"`, "expected \"kernel\""},
		{`"swapMiB": 4`, `"swapMiB": 1`, "too small"},
		{`"sha256": "`, `"sha256": "x`, "invalid string"},
		{`}`, `},`, "not a forge recipe"},
	} {
		if !strings.Contains(string(good), c.old) {
			t.Fatalf("fixture lacks %s", c.old)
		}
		_, err := Decode([]byte(strings.Replace(string(good), c.old, c.new, 1)))
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: got %v", c.new, err)
		}
	}
	if _, err := Decode([]byte("not json")); err == nil || !strings.Contains(err.Error(), "not a forge recipe") {
		t.Errorf("malformed: %v", err)
	}
	p, _ := Lookup("falcon030")
	planned, err := NewRecipe(Selection{Preset: p.ID, Machine: p.Machine, Settings: p.Settings}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err = Quadra(context.Background(), planned, nil, nil, io.Discard); err == nil || !strings.Contains(err.Error(), "planned") {
		t.Errorf("planned preset ran: %v", err)
	}
}

func TestRootRefusesRecipeWithoutDigests(t *testing.T) {
	s := testSelection()
	r, err := NewRecipe(s, nil)
	if err != nil {
		t.Fatal(err)
	}
	text, _ := Encode(r)
	if _, err := Root(context.Background(), text, nil, Media{Reader: bytes.NewReader(nil)}, make([]Media, len(s.Provision))); err == nil {
		t.Fatal("recipe without input digests accepted")
	}
}

// Format 1 recipes named segments 02, 03 and 10 as separate inputs.
func TestDecodeUpgradesSegmentRoles(t *testing.T) {
	files := fixtureMedia()
	s := testSelection()
	r, _ := NewRecipe(s, digests(t, s, files))
	r.Inputs[0].Parts = nil
	text, _ := Encode(r)
	var segs []string
	for i, id := range TapeSegments {
		in, _ := Digest(context.Background(), "amix-"+id, bytes.NewReader(files[i]), int64(len(files[i])))
		segs = append(segs, fmt.Sprintf(`{"role": %q, "size": %d, "sha256": %q}`, in.Role, in.Size, in.SHA256))
	}
	tape := fmt.Sprintf(`{
      "role": "amix-tape",
      "size": %d,
      "sha256": %q
    }`, r.Inputs[0].Size, r.Inputs[0].SHA256)
	old := strings.Replace(strings.Replace(string(text), tape, strings.Join(segs, ", "), 1), `"formatVersion": 2`, `"formatVersion": 1`, 1)
	if old == string(text) || !strings.Contains(old, "amix-10") {
		t.Fatal("fixture did not convert")
	}
	got, err := Decode([]byte(old))
	if err != nil {
		t.Fatal(err)
	}
	if b, _ := Encode(got); !bytes.Equal(b, text) {
		t.Fatalf("upgrade differs:\n%s", b)
	}
	if _, err = Decode([]byte(strings.Replace(old, "amix-03", "amix-04", 1))); err == nil || !strings.Contains(err.Error(), `expected "amix-03"`) {
		t.Fatal(err)
	}
}
