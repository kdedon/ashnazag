package planner

import "fmt"

type BootOptions struct {
	Login          string `json:"login"`
	DefaultSession string `json:"defaultSession"`
	Animation      bool   `json:"animation"`
}
type MemoryEstimate struct {
	PhysicalMiB  int      `json:"physicalMiB"`
	HostMiB      int      `json:"hostMiB"`
	DesktopMiB   int      `json:"desktopMiB"`
	GuestsMiB    int      `json:"guestsMiB"`
	AnimationMiB int      `json:"animationMiB"`
	TotalMiB     int      `json:"totalMiB"`
	RemainingMiB int      `json:"remainingMiB"`
	Warnings     []string `json:"warnings"`
}

func configureStartup(m *Manifest) error {
	s := &m.Selection
	if s.Desktop == "" {
		s.Desktop = "none"
		if contains(s.Packages, "x11") {
			s.Desktop = "twm"
		}
	}
	switch s.Desktop {
	case "none", "twm", "olwm", "olvwm":
	default:
		return fmt.Errorf("unknown X desktop %q", s.Desktop)
	}
	if s.Desktop != "none" && !contains(s.Packages, "x11") {
		return fmt.Errorf("X desktop requires the X11 package")
	}
	if s.Boot.Login == "" {
		s.Boot.Login = "console"
	}
	if s.Boot.Login != "console" && s.Boot.Login != "xdm" {
		return fmt.Errorf("unknown login mode")
	}
	if s.Boot.Login == "xdm" && s.Desktop == "none" {
		return fmt.Errorf("graphical login requires an X desktop")
	}
	if s.Boot.DefaultSession == "" {
		s.Boot.DefaultSession = "console"
		if s.Boot.Login == "xdm" {
			s.Boot.DefaultSession = "desktop:" + s.Desktop
		}
	}
	valid := s.Boot.DefaultSession == "console" && s.Boot.Login == "console"
	if s.Boot.Login == "xdm" {
		valid = s.Boot.DefaultSession == "desktop:"+s.Desktop
		for _, guest := range s.ContainerInstances {
			if s.Boot.DefaultSession == "guest:"+guest.ID {
				valid = true
			}
		}
	}
	if !valid {
		return fmt.Errorf("default session must reference the selected desktop or an installed guest; console login uses the console")
	}
	e := MemoryEstimate{HostMiB: 16, Warnings: []string{}}
	for _, ram := range s.MemoryMiB {
		e.PhysicalMiB += ram
	}
	if s.Desktop != "none" {
		e.DesktopMiB = 16
	}
	if s.Boot.Animation {
		e.AnimationMiB = 2
		m.Warnings = append(m.Warnings, "Boot splash/animation is planned; verbose boot, errors and fsck prompts must remain accessible.")
	}
	macs := 0
	for i := range s.ContainerInstances {
		g := &s.ContainerInstances[i]
		min, max, fallback := 8, 512, 32
		if g.Profile == "tosenv" {
			min, max, fallback = 1, 14, 14
		}
		if g.MemoryMiB == 0 {
			g.MemoryMiB = fallback
		}
		if g.MemoryMiB < min || g.MemoryMiB > max {
			return fmt.Errorf("%s memory budget must be %d–%d MiB", g.ID, min, max)
		}
		e.GuestsMiB += g.MemoryMiB
		if g.Profile == "macenv" {
			macs++
		}
		if g.Version == "system6" {
			m.Warnings = append(m.Warnings, "A/UX System 6 is planned: 24-bit uinter/libmac, ROM compatibility and media extraction need validation.")
		}
	}
	if macs > 1 {
		e.Warnings = append(e.Warnings, "Multiple Mac environments are configured; concurrent execution still needs uinter, ROM and PRAM isolation.")
	}
	if len(s.ContainerInstances) == 0 && len(s.Containers) > 0 {
		e.Warnings = append(e.Warnings, "Legacy profile selections have no per-instance RAM budgets; guest memory is excluded from this estimate.")
	}
	e.TotalMiB = e.HostMiB + e.DesktopMiB + e.GuestsMiB + e.AnimationMiB
	e.RemainingMiB = e.PhysicalMiB - e.TotalMiB
	if e.RemainingMiB < 0 {
		e.Warnings = append(e.Warnings, fmt.Sprintf("Running every selected environment together exceeds physical RAM by %d MiB. Reduce guest budgets, run fewer sessions, or add RAM; swap may be slow.", -e.RemainingMiB))
	} else if e.RemainingMiB < 8 {
		e.Warnings = append(e.Warnings, "Less than 8 MiB remains for applications and buffers; running everything together may cause heavy paging.")
	}
	m.MemoryEstimate = e
	m.Warnings = append(m.Warnings, "RAM budgets and startup choices are configuration intent; image scripts do not apply them yet. Memory estimates are provisional, not measured peak usage.")
	if s.Boot.Login == "xdm" {
		m.Warnings = append(m.Warnings, "Graphical login and the xdm session menu are planned. The default session is chosen after authentication; this does not enable automatic login.")
	}
	if s.Desktop == "olwm" || s.Desktop == "olvwm" {
		m.Warnings = append(m.Warnings, "OpenLook requires an XView desktop bundle; automatic package installation remains pending.")
	}
	return nil
}
