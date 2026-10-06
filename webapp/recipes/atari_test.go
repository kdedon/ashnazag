package recipes

import (
	"bytes"
	"os"
	"testing"

	"amigaux.org/imagebuilder/ufs"
)

func TestAtariRoot(t *testing.T) {
	es, _ := fixture()
	out, err := AtariRoot(es, "CST6CDT")
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]ufs.Entry{}
	for _, e := range out {
		got[e.Path] = e
	}
	b := make([]byte, got["/etc/TIMEZONE"].Size)
	got["/etc/TIMEZONE"].Data.ReadAt(b, 0)
	if string(b) != "TZ=CST6CDT\nexport TZ\n" || got["/home"].Kind != 'd' {
		t.Fatalf("timezone %q home %v", b, got["/home"])
	}
	if _, err := AtariRoot(es, "X;rm"); err == nil {
		t.Fatal("accepted a hostile zone")
	}
}

func TestAtariConfigMatchesSource(t *testing.T) {
	for _, name := range []string{"sysinit", "nodename"} {
		want, err := os.ReadFile("../../kernel/atari/etc/" + name)
		if os.IsNotExist(err) {
			t.Skip("source tree unavailable")
		}
		got, _ := atariConfig.ReadFile("atarietc/" + name)
		if err != nil || !bytes.Equal(got, want) {
			t.Fatalf("%s differs from source", name)
		}
	}
}
