package forge

import (
	"strings"
	"testing"

	"amigaux.org/imagebuilder/planner"
)

func TestCatalogSuperVidelOnlyOn060(t *testing.T) {
	c := planner.GetCatalog()
	for _, m := range c.Machines {
		has := contains(m.DeviceIDs, "supervidel")
		if has != (m.ID == "falcon060") {
			t.Errorf("%s: supervidel offered = %v", m.ID, has)
		}
		if contains(m.DefaultDevices, "supervidel") {
			t.Errorf("%s: supervidel is on by default", m.ID)
		}
	}
	for _, d := range c.Devices {
		if d.ID == "supervidel" && (d.Required || len(d.MachineIDs) != 1 || d.MachineIDs[0] != "falcon060" || d.Module != "") {
			t.Errorf("supervidel device: %+v", d)
		}
	}
}

func TestPresetOffersSuperVidelOptionally(t *testing.T) {
	p, _ := Lookup("falcon-ct60")
	if p.Status != "planned" || contains(p.Settings.Devices, "supervidel") || !contains(p.Settings.OptionalDevices, "supervidel") {
		t.Fatalf("preset: %+v", p)
	}
}

func selection(t *testing.T, preset string, devices ...string) (Selection, error) {
	p, _ := Lookup(preset)
	return Recipe{Preset: PresetRef{p.ID, p.Revision}, Changes: Changes{Devices: &devices}}.Selection()
}

func TestFalconVariant(t *testing.T) {
	base := []string{"falcon-ide", "falcon-video", "ikbd"}
	for _, c := range []struct {
		devices []string
		kernel  string
		noSV    bool
	}{
		{base, "unix-atari060-nosv.elf", true},
		{append([]string{"supervidel"}, base...), "unix-atari060.elf", false},
	} {
		s, err := selection(t, "falcon-ct60", c.devices...)
		if err != nil {
			t.Fatal(err)
		}
		if k, n := FalconVariant(s); k != c.kernel || n != c.noSV {
			t.Errorf("%v: got %s %v", c.devices, k, n)
		}
	}
}

func TestSuperVidelRejectedOnFalcon030(t *testing.T) {
	_, err := selection(t, "falcon030", "falcon-ide", "falcon-video", "ikbd", "supervidel")
	if err == nil || !strings.Contains(err.Error(), "supervidel") {
		t.Fatalf("got %v", err)
	}
}
