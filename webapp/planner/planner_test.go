package planner

import (
	"bytes"
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

func q800() Selection { return Selection{Machine: "q800", Devices: []string{"scsi53c96", "scc"}} }
func TestBootDependencies(t *testing.T) {
	for _, machine := range GetCatalog().Machines {
		if machine.Status == "planned" {
			continue
		}
		for _, id := range machine.DeviceIDs {
			d, _ := find(GetCatalog().Devices, id)
			if !d.Required {
				continue
			}
			var devices []string
			for _, other := range machine.DefaultDevices {
				if other != id {
					devices = append(devices, other)
				}
			}
			if _, err := Plan(Selection{Machine: machine.ID, Devices: devices}); err == nil {
				t.Errorf("accepted %s without %s", machine.ID, id)
			}
		}
	}
}
func TestRejectUnsupportedSelections(t *testing.T) {
	cases := []Selection{
		{Machine: "unknown"}, {Machine: "tt030"}, {Machine: "amiga040"},
		{Machine: "q800", Devices: []string{"scsi53c96", "scc", "falcon-ide"}},
		{Machine: "q800", Devices: []string{"scsi53c96", "scc"}, Packages: []string{"unknown"}},
		{Machine: "q800", Devices: []string{"scsi53c96", "scc"}, Containers: []string{"amigaenv"}},
		{Machine: "falcon030", Devices: []string{"falcon-ide", "ikbd", "falcon-video"}, Containers: []string{"macenv"}},
	}
	for _, s := range cases {
		if _, err := Plan(s); err == nil {
			t.Errorf("accepted unsupported selection: %+v", s)
		}
	}
}
func TestStaticDriversRemainAndModulesDeduplicate(t *testing.T) {
	s := q800()
	s.Containers = []string{"tosenv", "macenv"}
	m, err := Plan(s)
	if err != nil {
		t.Fatal(err)
	}
	if m.ImageBuildSupported {
		t.Fatal("planner claimed image support")
	}
	if !contains(m.Kernel.BuiltIn, "SONIC Ethernet") || !contains(m.Kernel.BuiltIn, "DLM loader, relocator and kernel symbols") {
		t.Fatal("lost current static kernel dependencies")
	}
	expected := []string{"auxcore", "auxexec", "guestcore", "tosguest", "uinter"}
	if !reflect.DeepEqual(m.Kernel.Modules, expected) {
		t.Fatalf("modules: %v", m.Kernel.Modules)
	}
	if len(m.RequiredInputs) != 4 {
		t.Fatalf("missing guest media: %v", m.RequiredInputs)
	}
}
func TestCanonicalManifest(t *testing.T) {
	a := q800()
	a.Packages = []string{"x11", "apkg", "x11"}
	b := q800()
	b.Devices = []string{"scc", "scsi53c96", "scc"}
	b.Packages = []string{"apkg", "x11"}
	ma, err := Plan(a)
	if err != nil {
		t.Fatal(err)
	}
	mb, err := Plan(b)
	if err != nil {
		t.Fatal(err)
	}
	ja, _ := json.Marshal(ma)
	jb, _ := json.Marshal(mb)
	if !bytes.Equal(ja, jb) {
		t.Fatalf("equivalent choices differ:\n%s\n%s", ja, jb)
	}
	if ma.SchemaVersion == "" || ma.CatalogVersion == "" {
		t.Fatal("missing versions")
	}
}
func TestProtocol(t *testing.T) {
	for _, input := range []string{`{`, `{"action":"catalog","typo":true}`, `{"action":"catalog"} {}`, `{"action":"build"}`, `{"action":"plan","selection":{"machine":"q800","device":[]}}`} {
		var response Response
		if err := json.Unmarshal(Handle([]byte(input)), &response); err != nil {
			t.Fatal(err)
		}
		if response.OK || response.Error == "" {
			t.Errorf("accepted invalid request: %s", input)
		}
	}
	var response Response
	if err := json.Unmarshal(Handle([]byte(`{"action":"catalog"}`)), &response); err != nil || !response.OK || response.Catalog == nil {
		t.Fatalf("catalog protocol: %+v %v", response, err)
	}
}
func TestCatalogIntegrity(t *testing.T) {
	c := GetCatalog()
	for _, group := range [][]Item{c.Machines, c.Devices, c.Packages, c.Containers} {
		seen := map[string]bool{}
		for _, item := range group {
			if item.ID == "" || seen[item.ID] || item.Label == "" || item.Status == "" || strings.TrimSpace(item.Description) == "" {
				t.Errorf("invalid catalog item %+v", item)
			}
			seen[item.ID] = true
			for _, id := range item.MachineIDs {
				if _, ok := find(c.Machines, id); !ok {
					t.Errorf("unknown machine %s", id)
				}
			}
		}
	}
	for _, machine := range c.Machines {
		for _, id := range machine.DeviceIDs {
			d, ok := find(c.Devices, id)
			if !ok || !contains(d.MachineIDs, machine.ID) {
				t.Errorf("invalid device mapping %s/%s", machine.ID, id)
			}
		}
		if machine.Status != "planned" {
			if _, err := Plan(Selection{Machine: machine.ID, Devices: machine.DefaultDevices}); err != nil {
				t.Errorf("invalid defaults: %v", err)
			}
		}
	}
}

func TestRepeatedGuestVersionsKeepSeparateMedia(t *testing.T) {
	s := q800()
	s.ContainerInstances = []ContainerInstance{
		{ID: "guest-1", Profile: "macenv", Version: "macos81", ROMMode: "override", Media: map[string][]string{"system": {"guest-1/system/0"}, "rom": {"guest-1/rom/0"}}},
		{ID: "guest-2", Profile: "macenv", Version: "macos761", ROMMode: "override", Media: map[string][]string{"system": {"guest-2/system/0"}, "rom": {"guest-2/rom/0"}}},
	}
	m, err := Plan(s)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(m.Selection.ContainerInstances, s.ContainerInstances) {
		t.Fatal("guest versions or media bindings lost")
	}
	if len(m.Kernel.Modules) != 4 {
		t.Fatalf("shared modules duplicated: %v", m.Kernel.Modules)
	}
	if len(m.Selection.Containers) != 1 {
		t.Fatal("profile dependency did not deduplicate")
	}
	s.ContainerInstances[1].ID = "guest-1"
	if _, err := Plan(s); err == nil {
		t.Fatal("accepted colliding instance IDs")
	}
}

func TestGuestMediaAndMachineValidation(t *testing.T) {
	for _, instance := range []ContainerInstance{
		{ID: "a", Profile: "macenv", Version: " "},
		{ID: "a", Profile: "macenv", Version: "macos81", ROMMode: "override", Media: map[string][]string{"kickstart": {"x"}}},
		{ID: "a", Profile: "macenv", Version: "macos81", ROMMode: "override", Media: map[string][]string{"rom": {"a", "b"}}},
		{ID: "a", Profile: "macenv", Version: "macos81", ROMMode: "override", Media: map[string][]string{"system": {""}}},
	} {
		s := q800()
		s.ContainerInstances = []ContainerInstance{instance}
		if _, err := Plan(s); err == nil {
			t.Fatalf("accepted invalid guest: %+v", instance)
		}
	}
	s := Selection{Machine: "falcon030", Devices: []string{"falcon-ide", "ikbd", "falcon-video"}, ContainerInstances: []ContainerInstance{{ID: "a", Profile: "macenv", Version: "macos81", ROMMode: "override"}}}
	if _, err := Plan(s); err == nil {
		t.Fatal("accepted incompatible guest instance")
	}
	s = q800()
	s.BaseMedia = map[string][]string{"rom": {"x"}}
	if _, err := Plan(s); err == nil {
		t.Fatal("accepted guest media role for base system")
	}
}

func TestHostROMAndSupportedEnvironments(t *testing.T) {
	s := q800()
	s.ContainerInstances = []ContainerInstance{{ID: "mac", Profile: "macenv", Version: "macos81"}}
	m, err := Plan(s)
	if err != nil {
		t.Fatal(err)
	}
	if m.Selection.ContainerInstances[0].ROMMode != "host" {
		t.Fatal("Mac host did not inherit ROM")
	}
	for _, input := range m.RequiredInputs {
		if strings.Contains(input, "ROM override") {
			t.Fatal("host ROM required a file")
		}
	}
	s.ContainerInstances[0].Version = "System 7.1"
	if _, err := Plan(s); err == nil {
		t.Fatal("accepted environment without a recipe")
	}
	s.ContainerInstances[0].Version = "macos81"
	s.ContainerInstances[0].Media = map[string][]string{"rom": {"file"}}
	if _, err := Plan(s); err == nil {
		t.Fatal("accepted ROM file without explicit override")
	}
	s.ContainerInstances[0].ROMMode = "override"
	m, err = Plan(s)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(strings.Join(m.Warnings, " "), "reduce performance") {
		t.Fatal("missing ROM override warning")
	}
	s.ContainerInstances[0].Media["aux"] = []string{"file"}
	if _, err := Plan(s); err == nil {
		t.Fatal("accepted per-guest shared A/UX media")
	}
}

func TestMemoryAndFamilyCatalog(t *testing.T) {
	s := q800()
	s.MemoryMiB = map[string]int{"ram": 64}
	m, err := Plan(s)
	if err != nil || m.Selection.MemoryMiB["ram"] != 64 {
		t.Fatalf("memory lost: %v", err)
	}
	for _, memory := range []map[string]int{{"st": 14}, {"ram": -1}, {"ram": 0}, {"ram": 4097}} {
		s.MemoryMiB = memory
		if _, err := Plan(s); err == nil {
			t.Fatalf("accepted invalid memory %v", memory)
		}
	}
	families := map[string]bool{}
	for _, machine := range GetCatalog().Machines {
		families[machine.Family] = true
		if machine.ID == "fpga" || machine.Model == "" || machine.Variant == "" {
			t.Fatal("invalid family/model/variant")
		}
	}
	if len(families) != 3 {
		t.Fatal("expected three hardware families")
	}
}
