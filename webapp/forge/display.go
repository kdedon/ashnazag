package forge

import (
	"debug/elf"
	"fmt"
	"io"
	"path"
	"sort"

	"amigaux.org/imagebuilder/ufs"
)

const DisplayModulePath = "/etc/conf/mod.d/dsuser"

func displayELF(m Media) (*elf.File, error) {
	if m.Reader == nil || m.Size < 52 || m.Size > 32<<20 {
		return nil, fmt.Errorf("invalid ELF size")
	}
	f, err := elf.NewFile(io.NewSectionReader(m.Reader, 0, m.Size))
	if err != nil {
		return nil, err
	}
	if f.Class != elf.ELFCLASS32 || f.Data != elf.ELFDATA2MSB || f.Machine != elf.EM_68K {
		return nil, fmt.Errorf("expected big-endian m68k ELF32")
	}
	for _, s := range f.Sections {
		if s.Type != elf.SHT_NOBITS && (s.Offset > uint64(m.Size) || s.Size > uint64(m.Size)-s.Offset) {
			return nil, fmt.Errorf("ELF section exceeds input")
		}
	}
	return f, nil
}

func definedSymbol(f *elf.File, name string) (bool, error) {
	symbols, err := f.Symbols()
	if err == elf.ErrNoSymbols {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	for _, s := range symbols {
		if s.Name == name && s.Section != elf.SHN_UNDEF && elf.ST_BIND(s.Info) == elf.STB_GLOBAL {
			return true, nil
		}
	}
	return false, nil
}

// ValidateDisplay requires the display module for kernels that expose its loader.
func ValidateDisplay(kernel Media, module *Media) error {
	f, err := displayELF(kernel)
	if err != nil {
		return fmt.Errorf("read kernel ELF: %w", err)
	}
	if f.Type != elf.ET_EXEC {
		return fmt.Errorf("kernel must be executable ELF")
	}
	required, err := definedSymbol(f, "ds_modinit")
	if err != nil {
		return fmt.Errorf("read kernel symbols: %w", err)
	}
	if module == nil {
		if required {
			return fmt.Errorf("this kernel requires its matching dsuser display module")
		}
		return nil
	}
	kernelSymbols, err := f.Symbols()
	if err != nil && err != elf.ErrNoSymbols {
		return fmt.Errorf("read kernel symbols: %w", err)
	}
	checkImports := err != elf.ErrNoSymbols
	defined := map[string]bool{}
	for _, sym := range kernelSymbols {
		if sym.Section != elf.SHN_UNDEF && elf.ST_BIND(sym.Info) != elf.STB_LOCAL {
			defined[sym.Name] = true
		}
	}
	if module.Size > 2<<20 {
		return fmt.Errorf("dsuser exceeds the 2 MiB module limit")
	}
	f, err = displayELF(*module)
	if err != nil {
		return fmt.Errorf("read dsuser module: %w", err)
	}
	if f.Type != elf.ET_REL {
		return fmt.Errorf("dsuser must be a relocatable ELF module")
	}
	found, err := definedSymbol(f, "dsuser_wrapper")
	if err != nil {
		return fmt.Errorf("read dsuser symbols: %w", err)
	}
	imports, err := f.Symbols()
	if err != nil {
		return fmt.Errorf("read dsuser symbols: %w", err)
	}
	for _, sym := range imports {
		if checkImports && sym.Name != "" && sym.Section == elf.SHN_UNDEF && elf.ST_BIND(sym.Info) == elf.STB_GLOBAL && !defined[sym.Name] {
			return fmt.Errorf("dsuser requires kernel symbol %s; choose the module built for this kernel", sym.Name)
		}
	}
	moddata := 0
	for _, section := range f.Sections {
		if section.Type == elf.SectionType(13) && section.Flags&elf.SHF_ALLOC != 0 && section.Size >= 4 {
			moddata++
		}
	}
	if moddata != 1 {
		return fmt.Errorf("dsuser must contain one allocated module metadata section")
	}
	if !found {
		return fmt.Errorf("module does not define dsuser_wrapper")
	}
	return nil
}

func stageDisplay(entries []ufs.Entry, module Media) ([]ufs.Entry, error) {
	tree := make(map[string]ufs.Entry, len(entries)+4)
	for _, e := range entries {
		tree[e.Path] = e
	}
	for p := path.Dir(DisplayModulePath); p != "/"; p = path.Dir(p) {
		if e, ok := tree[p]; ok {
			if e.Kind != 'd' {
				return nil, fmt.Errorf("display module parent %s is not a directory", p)
			}
		} else {
			tree[p] = ufs.Entry{Path: p, Kind: 'd', Mode: 0755}
		}
	}
	if e, ok := tree[DisplayModulePath]; ok && e.Kind != 'f' {
		return nil, fmt.Errorf("display module path is not a regular file")
	}
	tree[DisplayModulePath] = ufs.Entry{Path: DisplayModulePath, Kind: 'f', Mode: 0644, Size: module.Size, Data: module.Reader}
	out := make([]ufs.Entry, 0, len(tree))
	for _, e := range tree {
		out = append(out, e)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out, nil
}
