package svr4

import (
	"bytes"
	"context"
	_ "embed"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"amigaux.org/imagebuilder/ufs"
)

func fixture() (Info, []ufs.Entry) {
	return Info{Package: "ASHtest", Name: "Test application", Version: "1.0", Architecture: "m68k", BaseDir: "/amiga/apps/test", Timestamp: 42}, []ufs.Entry{{Path: "/Apps", Kind: 'd', Mode: 0755}, {Path: "/Apps/readme", Kind: 'f', Mode: 0644, Data: strings.NewReader("hello\n"), Size: 6}, {Path: "/Apps/copy", Kind: 'h', Target: "/Apps/readme"}, {Path: "/alias", Kind: 'l', Target: "Apps/readme"}}
}
func encoded(t *testing.T) []byte {
	t.Helper()
	v, es := fixture()
	var b bytes.Buffer
	if e := Emit(context.Background(), &b, v, es); e != nil {
		t.Fatal(e)
	}
	return b.Bytes()
}
func TestRoundTrip(t *testing.T) {
	b := encoded(t)
	v, es, e := Import(context.Background(), bytes.NewReader(b), int64(len(b)), Options{})
	if e != nil {
		t.Fatal(e)
	}
	if v.Timestamp != 42 || len(es) != 4 {
		t.Fatalf("%+v %+v", v, es)
	}
	for _, x := range es {
		if x.Kind == 'f' {
			data, _ := io.ReadAll(io.NewSectionReader(x.Data, 0, x.Size))
			if string(data) != "hello\n" {
				t.Fatal(string(data))
			}
		}
	}
	v0, es0 := fixture()
	es0[0], es0[3] = es0[3], es0[0]
	var again bytes.Buffer
	if e := Emit(context.Background(), &again, v0, es0); e != nil {
		t.Fatal(e)
	}
	if !bytes.Equal(b, again.Bytes()) {
		t.Fatal("nondeterministic")
	}
}
func TestRejects(t *testing.T) {
	for _, mutate := range []func([]byte) []byte{
		func(b []byte) []byte { return bytes.Replace(b, []byte("ASHtest 1"), []byte("ASHtest 2"), 1) },
		func(b []byte) []byte { return bytes.Replace(b, []byte("none"), []byte("evil"), 1) },
		func(b []byte) []byte { return bytes.Replace(b, []byte("hello\n"), []byte("evil!\n"), 1) },
		func(b []byte) []byte { return append(b, make([]byte, 512)...) },
		func(b []byte) []byte { return b[:len(b)-1] },
	} {
		b := mutate(encoded(t))
		if _, _, e := Import(context.Background(), bytes.NewReader(b), int64(len(b)), Options{}); e == nil {
			t.Fatal("accepted malformed package")
		}
	}
	v, es := fixture()
	for _, p := range []string{"/../escape", "/x/../escape", "/tab\tname", "//double"} {
		es[0].Path = p
		if e := Emit(context.Background(), io.Discard, v, es); e == nil {
			t.Fatal(p)
		}
	}
}
func TestBadEntries(t *testing.T) {
	v, es := fixture()
	for _, extra := range []ufs.Entry{{Path: "/Apps", Kind: 'd'}, {Path: "/bad", Kind: 'h', Target: "/missing"}, {Path: "/bad", Kind: 'l', Target: "../../escape"}, {Path: "/Apps/readme/child", Kind: 'f'}, {Path: "/device", Kind: 'c'}} {
		all := append(append([]ufs.Entry{}, es...), extra)
		if e := Emit(context.Background(), io.Discard, v, all); e == nil {
			t.Fatal(extra)
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if e := Emit(ctx, io.Discard, v, es); e == nil {
		t.Fatal("ignored cancellation")
	}
}
func TestIndependentCPIO(t *testing.T) {
	cpio, e := exec.LookPath("cpio")
	if e != nil {
		t.Skip("cpio unavailable")
	}
	b := encoded(t)
	dir := t.TempDir()
	meta, end, e := archive(context.Background(), bytes.NewReader(b), 512, int64(len(b)), Options{MaxMetadataBytes: 1 << 20})
	if e != nil || len(meta) != 2 {
		t.Fatal(e)
	}
	for _, off := range []int64{512, (end + 511) &^ 511} {
		cmd := exec.Command(cpio, "-idmu", "--quiet", "--no-absolute-filenames")
		cmd.Dir = dir
		cmd.Stdin = bytes.NewReader(b[off:])
		if out, e := cmd.CombinedOutput(); e != nil {
			t.Fatalf("%v %s", e, out)
		}
	}
	got, e := os.ReadFile(filepath.Join(dir, "reloc", "Apps", "readme"))
	if e != nil || string(got) != "hello\n" {
		t.Fatalf("%q %v", got, e)
	}
}

//go:embed testdata/ASHtest.pkg
var reference []byte

func TestReferenceBytes(t *testing.T) {
	if !bytes.Equal(encoded(t), reference) {
		t.Fatal("datastream bytes differ from native fixture")
	}
}

func TestSpacePaths(t *testing.T) {
	v, _ := fixture()
	es := []ufs.Entry{{Path: "/System Folder", Kind: 'd', Mode: 0755}, {Path: "/System Folder/Mac OS", Kind: 'f', Mode: 0644, Size: 4, Data: strings.NewReader("data")}, {Path: "/OS Alias", Kind: 'l', Target: "System Folder/Mac OS"}, {Path: "/OS Hardlink", Kind: 'h', Target: "/System Folder/Mac OS"}}
	var b bytes.Buffer
	if e := Emit(context.Background(), &b, v, es); e != nil {
		t.Fatal(e)
	}
	if !bytes.Contains(b.Bytes(), []byte("'System Folder/Mac OS'")) {
		t.Fatal("missing quoted path")
	}
	_, got, e := Import(context.Background(), bytes.NewReader(b.Bytes()), int64(b.Len()), Options{})
	if e != nil {
		t.Fatal(e)
	}
	if len(got) != 4 {
		t.Fatal(len(got))
	}
	for _, e := range got {
		if e.Kind == 'l' && e.Target != "System Folder/Mac OS" {
			t.Fatal(e.Target)
		}
	}
}
func TestMalformedQuotes(t *testing.T) {
	for _, s := range []string{"1 f none 'unfinished", "1 f none 'System Folder'junk", "1 f none app'part'", "1 f none ''"} {
		if _, e := mapFields(s); e == nil {
			t.Fatal(s)
		}
	}
	v, _ := fixture()
	for _, p := range []string{"/unsafe'quote", "/unsafe\"quote", "/unsafe$variable", "/unsafe`substitution"} {
		if e := Emit(context.Background(), io.Discard, v, []ufs.Entry{{Path: p, Kind: 'd'}}); e == nil {
			t.Fatal(p)
		}
	}
}

func TestStripsSetIDAndWrite(t *testing.T) {
	v, _ := fixture()
	es := []ufs.Entry{{Path: "/bin", Kind: 'd', Mode: 02777}, {Path: "/bin/su", Kind: 'f', Mode: 06777, Size: 1, Data: strings.NewReader("x")}}
	var b bytes.Buffer
	if e := Emit(context.Background(), &b, v, es); e != nil {
		t.Fatal(e)
	}
	_, got, e := Import(context.Background(), bytes.NewReader(b.Bytes()), int64(b.Len()), Options{})
	if e != nil {
		t.Fatal(e)
	}
	for _, x := range got {
		if x.Mode != 0755 {
			t.Fatalf("%s mode %04o, want 0755", x.Path, x.Mode)
		}
	}
}
