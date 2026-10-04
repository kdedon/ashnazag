package planner

import (
	"strings"
	"testing"
)

func TestStartupRequiresInstalledSessions(t *testing.T) {
	s := q800()
	s.Boot = BootOptions{Login: "xdm"}
	if _, err := Plan(s); err == nil {
		t.Fatal("xdm accepted without X desktop")
	}
	s.Packages = []string{"x11"}
	s.Desktop = "twm"
	s.Boot.DefaultSession = "guest:missing"
	if _, err := Plan(s); err == nil {
		t.Fatal("missing default guest accepted")
	}
	s.ContainerInstances = []ContainerInstance{{ID: "mac", Profile: "macenv", Version: "macos81", MemoryMiB: 32}}
	s.Boot.DefaultSession = "guest:mac"
	m, err := Plan(s)
	if err != nil {
		t.Fatal(err)
	}
	if m.Selection.Boot.DefaultSession != "guest:mac" {
		t.Fatal("default guest lost")
	}
	s.ContainerInstances = nil
	if _, err := Plan(s); err == nil {
		t.Fatal("removed default guest accepted")
	}
	s.Boot = BootOptions{Login: "console", DefaultSession: "desktop:twm"}
	if _, err := Plan(s); err == nil {
		t.Fatal("desktop default accepted for console login")
	}
}

func TestMemoryBudgetIncludesAllGuests(t *testing.T) {
	s := q800()
	s.MemoryMiB = map[string]int{"ram": 64}
	s.Packages = []string{"x11"}
	s.Boot = BootOptions{Login: "xdm", Animation: true}
	s.ContainerInstances = []ContainerInstance{{ID: "one", Profile: "macenv", Version: "macos81", MemoryMiB: 32}, {ID: "two", Profile: "macenv", Version: "macos761", MemoryMiB: 32}}
	m, err := Plan(s)
	if err != nil {
		t.Fatal(err)
	}
	if m.MemoryEstimate.TotalMiB != 98 || m.MemoryEstimate.RemainingMiB != -34 {
		t.Fatalf("bad estimate: %+v", m.MemoryEstimate)
	}
	if !strings.Contains(strings.Join(m.MemoryEstimate.Warnings, " "), "exceeds physical RAM") {
		t.Fatal("missing pressure warning")
	}
	if !strings.Contains(strings.Join(m.MemoryEstimate.Warnings, " "), "isolation") {
		t.Fatal("missing multi-Mac limitation")
	}
	s.ContainerInstances[0].MemoryMiB = -1
	if _, err := Plan(s); err == nil {
		t.Fatal("negative guest budget accepted")
	}
}

func TestSystemSixIsPlanned(t *testing.T) {
	s := q800()
	s.ContainerInstances = []ContainerInstance{{ID: "six", Profile: "macenv", Version: "system6", MemoryMiB: 8}}
	m, err := Plan(s)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(strings.Join(m.Warnings, " "), "24-bit") {
		t.Fatal("missing System 6 compatibility limitation")
	}
	if m.ImageBuildSupported {
		t.Fatal("claimed buildable image")
	}
}
