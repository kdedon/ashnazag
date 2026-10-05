package planner

import (
	"bytes"
	_ "embed"
	"encoding/json"
	"fmt"
	"io"
	"sort"
	"strings"
)

//go:embed catalog.json
var catalogJSON []byte

type MediaRequirement struct {
	ID       string `json:"id"`
	Label    string `json:"label"`
	Multiple bool   `json:"multiple,omitempty"`
}
type MemoryBank struct {
	ID         string `json:"id"`
	Label      string `json:"label"`
	DefaultMiB int    `json:"defaultMiB"`
}
type Environment struct {
	Status            string             `json:"status,omitempty"`
	ID                string             `json:"id"`
	Label             string             `json:"label"`
	MediaRequirements []MediaRequirement `json:"mediaRequirements"`
}
type ContainerInstance struct {
	MemoryMiB int                 `json:"memoryMiB,omitempty"`
	ROMMode   string              `json:"romMode,omitempty"`
	ID        string              `json:"id"`
	Profile   string              `json:"profile"`
	Version   string              `json:"version"`
	Media     map[string][]string `json:"media,omitempty"`
}
type Item struct {
	Family            string             `json:"family,omitempty"`
	Model             string             `json:"model,omitempty"`
	Variant           string             `json:"variant,omitempty"`
	MemoryBanks       []MemoryBank       `json:"memoryBanks,omitempty"`
	Environments      []Environment      `json:"environments,omitempty"`
	MediaRequirements []MediaRequirement `json:"mediaRequirements,omitempty"`
	ID                string             `json:"id"`
	Label             string             `json:"label"`
	Status            string             `json:"status"`
	Description       string             `json:"description"`
	CPU               string             `json:"cpu,omitempty"`
	DeviceIDs         []string           `json:"deviceIds,omitempty"`
	DefaultDevices    []string           `json:"defaultDevices,omitempty"`
	MachineIDs        []string           `json:"machineIds,omitempty"`
	BuiltIn           bool               `json:"builtIn,omitempty"`
	Required          bool               `json:"required,omitempty"`
	Module            string             `json:"module,omitempty"`
	Modules           []string           `json:"modules,omitempty"`
}
type Catalog struct {
	SchemaVersion string `json:"schemaVersion"`
	Version       string `json:"version"`
	Machines      []Item `json:"machines"`
	Devices       []Item `json:"devices"`
	Packages      []Item `json:"packages"`
	Containers    []Item `json:"containers"`
}
type Selection struct {
	Desktop            string              `json:"desktop,omitempty"`
	Boot               BootOptions         `json:"boot"`
	MemoryMiB          map[string]int      `json:"memoryMiB,omitempty"`
	ContainerInstances []ContainerInstance `json:"containerInstances,omitempty"`
	BaseMedia          map[string][]string `json:"baseMedia,omitempty"`
	Machine            string              `json:"machine"`
	Devices            []string            `json:"devices"`
	Packages           []string            `json:"packages"`
	Containers         []string            `json:"containers"`
}
type Kernel struct {
	BuiltIn []string `json:"builtIn"`
	Modules []string `json:"modules"`
}
type Manifest struct {
	MemoryEstimate      MemoryEstimate `json:"memoryEstimate"`
	SchemaVersion       string         `json:"schemaVersion"`
	CatalogVersion      string         `json:"catalogVersion"`
	Selection           Selection      `json:"selection"`
	Kernel              Kernel         `json:"kernel"`
	RequiredInputs      []string       `json:"requiredInputs"`
	Steps               []string       `json:"steps"`
	Warnings            []string       `json:"warnings"`
	ImageBuildSupported bool           `json:"imageBuildSupported"`
}
type Request struct {
	Action    string    `json:"action"`
	Selection Selection `json:"selection"`
}
type Response struct {
	OK       bool      `json:"ok"`
	Catalog  *Catalog  `json:"catalog,omitempty"`
	Manifest *Manifest `json:"manifest,omitempty"`
	Error    string    `json:"error,omitempty"`
}

func GetCatalog() Catalog {
	var c Catalog
	if err := json.Unmarshal(catalogJSON, &c); err != nil {
		panic(err)
	}
	return c
}
func contains(xs []string, s string) bool {
	for _, x := range xs {
		if x == s {
			return true
		}
	}
	return false
}
func normalized(xs []string) []string {
	result := []string{}
	for _, x := range xs {
		if !contains(result, x) {
			result = append(result, x)
		}
	}
	sort.Strings(result)
	return result
}
func find(items []Item, id string) (Item, bool) {
	for _, x := range items {
		if x.ID == id {
			return x, true
		}
	}
	return Item{}, false
}
func validateMedia(bindings map[string][]string, requirements []MediaRequirement) error {
	for role, refs := range bindings {
		var requirement *MediaRequirement
		for i := range requirements {
			if requirements[i].ID == role {
				requirement = &requirements[i]
				break
			}
		}
		if requirement == nil {
			return fmt.Errorf("unknown media role %q", role)
		}
		if !requirement.Multiple && len(refs) > 1 {
			return fmt.Errorf("media role %s accepts one file", role)
		}
		for _, ref := range refs {
			if strings.TrimSpace(ref) == "" {
				return fmt.Errorf("empty media reference for %s", role)
			}
		}
	}
	return nil
}
func instanceRequirements(instance *ContainerInstance, profile Item, machine Item) ([]MediaRequirement, error) {
	var requirements []MediaRequirement
	found := false
	for _, env := range profile.Environments {
		if env.ID == instance.Version {
			requirements = append(requirements, env.MediaRequirements...)
			found = true
		}
	}
	if !found {
		return nil, fmt.Errorf("unsupported environment %q for %s", instance.Version, profile.Label)
	}
	if profile.ID == "macenv" {
		if instance.ROMMode == "" {
			if machine.Family == "Macintosh" {
				instance.ROMMode = "host"
			} else {
				instance.ROMMode = "file"
			}
		}
		switch instance.ROMMode {
		case "host":
			if machine.Family != "Macintosh" {
				return nil, fmt.Errorf("host Macintosh ROM unavailable on %s", machine.Label)
			}
		case "override":
			if machine.Family != "Macintosh" {
				return nil, fmt.Errorf("ROM override requires a Macintosh host")
			}
			requirements = append(requirements, MediaRequirement{ID: "rom", Label: "Macintosh ROM override"})
		case "file":
			if machine.Family == "Macintosh" {
				return nil, fmt.Errorf("use explicit override for a Macintosh host")
			}
			requirements = append(requirements, MediaRequirement{ID: "rom", Label: "Macintosh ROM"})
		default:
			return nil, fmt.Errorf("unknown ROM mode %q", instance.ROMMode)
		}
	} else if instance.ROMMode != "" {
		return nil, fmt.Errorf("ROM mode applies only to Macintosh guests")
	}
	return requirements, nil
}
func Plan(s Selection) (Manifest, error) {
	c := GetCatalog()
	machine, ok := find(c.Machines, s.Machine)
	if !ok {
		return Manifest{}, fmt.Errorf("unknown machine %q", s.Machine)
	}
	if machine.Status == "planned" {
		return Manifest{}, fmt.Errorf("machine %q is planned; no validated recipe exists", s.Machine)
	}
	if s.MemoryMiB == nil {
		s.MemoryMiB = map[string]int{}
	}
	for key, value := range s.MemoryMiB {
		known := false
		for _, bank := range machine.MemoryBanks {
			if bank.ID == key {
				known = true
			}
		}
		if !known || value < 0 || value > 4096 {
			return Manifest{}, fmt.Errorf("invalid memory bank or size: %s", key)
		}
	}
	for _, bank := range machine.MemoryBanks {
		if _, ok := s.MemoryMiB[bank.ID]; !ok {
			s.MemoryMiB[bank.ID] = bank.DefaultMiB
		}
	}
	total := 0
	for _, value := range s.MemoryMiB {
		total += value
	}
	if total == 0 {
		return Manifest{}, fmt.Errorf("installed memory must be greater than zero")
	}
	s.Devices = normalized(s.Devices)
	s.Packages = normalized(s.Packages)
	seen := map[string]bool{}
	for index := range s.ContainerInstances {
		instance := &s.ContainerInstances[index]
		if strings.TrimSpace(instance.ID) == "" || seen[instance.ID] {
			return Manifest{}, fmt.Errorf("container instance IDs must be nonempty and unique")
		}
		seen[instance.ID] = true
		if strings.TrimSpace(instance.Version) == "" {
			return Manifest{}, fmt.Errorf("container %s needs a version label", instance.ID)
		}
		profile, exists := find(c.Containers, instance.Profile)
		if !exists {
			return Manifest{}, fmt.Errorf("unknown container profile %q", instance.Profile)
		}
		requirements, err := instanceRequirements(instance, profile, machine)
		if err != nil {
			return Manifest{}, err
		}
		if err := validateMedia(instance.Media, requirements); err != nil {
			return Manifest{}, fmt.Errorf("container %s: %w", instance.ID, err)
		}
		s.Containers = append(s.Containers, instance.Profile)
	}
	if err := validateMedia(s.BaseMedia, machine.MediaRequirements); err != nil {
		return Manifest{}, err
	}
	s.Containers = normalized(s.Containers)
	m := Manifest{SchemaVersion: "2", CatalogVersion: c.Version, Selection: s, Kernel: Kernel{BuiltIn: []string{}, Modules: []string{}}, RequiredInputs: []string{"User-supplied AMIX installation media", "User-supplied AMIX 2.1 patch media"}, Steps: []string{"Validate local media and record content hashes (pending)", "Extract the AMIX relink kit (pending)", "Relink pinned platform objects and validate relocations (pending)", "Install selected packages and guest modules (pending)", "Assemble and validate filesystem and partition images (pending)"}, Warnings: []string{"Planning only: no disk image is generated.", "Selections are intent; native build scripts do not yet consume this manifest.", "Artifact hashes, toolchain pins and media hashes must be resolved before a reproducible image build."}}
	for _, group := range []struct {
		kind  string
		ids   []string
		items []Item
	}{{"device", s.Devices, c.Devices}, {"package", s.Packages, c.Packages}, {"container", s.Containers, c.Containers}} {
		for _, id := range group.ids {
			item, exists := find(group.items, id)
			if !exists {
				return Manifest{}, fmt.Errorf("unknown %s %q", group.kind, id)
			}
			if !contains(item.MachineIDs, s.Machine) {
				return Manifest{}, fmt.Errorf("%s %q is incompatible with %s", group.kind, id, s.Machine)
			}
			if item.Status == "planned" {
				return Manifest{}, fmt.Errorf("%s %q is planned and unavailable", group.kind, id)
			}
			m.Kernel.Modules = append(m.Kernel.Modules, item.Modules...)
		}
	}
	for _, id := range machine.DeviceIDs {
		d, _ := find(c.Devices, id)
		if d.Required && !contains(s.Devices, id) {
			return Manifest{}, fmt.Errorf("required boot device %q must be selected", id)
		}
	}
	if s.Machine == "q800" {
		m.Kernel.BuiltIn = []string{"AMIX core and s5 root filesystem", "68040 platform, MMU and interrupt handling", "SCC early console and serial TTY", "53C96 SCSI root storage", "RAM disk", "RTC", "DLM loader, relocator and kernel symbols", "guest shims and exception gates", "ADB input", "framebuffer console", "display service", "SONIC Ethernet"}
		m.Warnings = append(m.Warnings, "ADB, framebuffer, display and SONIC remain statically linked even when unselected; module conversion and build switches are pending.")
	} else {
		m.Kernel.BuiltIn = []string{"AMIX core and s5 root filesystem", "68030 platform, MMU and interrupt handling", "Falcon IDE and AHDI partitions", "RAM disk", "RTC", "IKBD input", "framebuffer console and display service", "FPU emulator"}
		m.Warnings = append(m.Warnings, "Falcon relink currently lacks DLM and guest integration; guest containers are unavailable.")
	}
	for _, id := range s.Containers {
		switch id {
		case "macenv":
			m.RequiredInputs = append(m.RequiredInputs, "Shared A/UX support media")
		case "tosenv":
			configured := false
			for _, instance := range s.ContainerInstances {
				if instance.Profile == id {
					configured = true
				}
			}
			if configured {
				continue
			}
			m.RequiredInputs = append(m.RequiredInputs, "User-supplied TOS ROM and system files")
		}
	}
	for _, instance := range s.ContainerInstances {
		profile, _ := find(c.Containers, instance.Profile)
		requirements, _ := instanceRequirements(&instance, profile, machine)
		for _, media := range requirements {
			m.RequiredInputs = append(m.RequiredInputs, fmt.Sprintf("%s (%s, %s): %s", profile.Label, instance.Version, instance.ID, media.Label))
		}
		m.Warnings = append(m.Warnings, fmt.Sprintf("%s: selected environment and media require compatibility validation.", instance.ID))
		if instance.ROMMode == "override" {
			m.Warnings = append(m.Warnings, fmt.Sprintf("%s: overriding the host ROM may require compatibility handling and reduce performance.", instance.ID))
		}
	}
	if err := configureStartup(&m); err != nil {
		return Manifest{}, err
	}
	s = m.Selection
	if s.Machine == "q800" && len(s.Devices) == 4 && contains(s.Devices, "adb") && contains(s.Devices, "framebuffer") && contains(s.Devices, "scc") && contains(s.Devices, "scsi53c96") && len(s.Packages) == 0 && len(s.Containers) == 0 && len(s.ContainerInstances) == 0 && s.Desktop == "none" && s.Boot.Login == "console" && s.Boot.DefaultSession == "console" && !s.Boot.Animation {
		m.ImageBuildSupported = true
		m.RequiredInputs = []string{"AMIX 2.1 tape archive (one or more parts)", "Prebuilt Quadra kernel ELF", "A/UX donor disk for its Apple disk driver"}
		m.Steps = []string{"Apply Quadra console root recipe", "Create UFS root filesystem", "Create HFS boot volume with supplied kernel", "Assemble Apple partition map, root and swap", "Download disk image"}
		m.Warnings = []string{"The supplied kernel is used unchanged; kernel compilation and driver selection are not performed in the browser.", "This console recipe does not install guests, packages, graphical login or boot animations."}
	}
	m.Kernel.Modules = normalized(m.Kernel.Modules)
	return m, nil
}
func Handle(data []byte) []byte {
	var req Request
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	err := decoder.Decode(&req)
	if err == nil {
		var extra any
		if e := decoder.Decode(&extra); e != io.EOF {
			err = fmt.Errorf("expected one JSON request")
		}
	}
	response := Response{}
	if err == nil {
		switch req.Action {
		case "catalog":
			c := GetCatalog()
			response = Response{OK: true, Catalog: &c}
		case "plan":
			var m Manifest
			m, err = Plan(req.Selection)
			if err == nil {
				response = Response{OK: true, Manifest: &m}
			}
		default:
			err = fmt.Errorf("unknown action %q", req.Action)
		}
	}
	if err != nil {
		response = Response{Error: err.Error()}
	}
	out, _ := json.Marshal(response)
	return out
}
