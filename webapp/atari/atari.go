// Package atari writes the Falcon disk layout: an AHDI root sector with
// boot code, a boot partition (AXB) holding the loader, command line and
// kernel, then root (AXR), swap (AXS) and /home (AXU) partitions, and a
// FAT16 tail partition (ICD entry GEM) whose \AUTO\AXBOOT.PRG lets EmuTOS
// start the loader.
package atari

import (
	"context"
	_ "embed"
	"encoding/binary"
	"fmt"
	"io"
)

var (
	//go:embed bootsec.bin
	bootSector []byte
	//go:embed axbload.bin
	loader []byte
	//go:embed axbload-nosv.bin
	loaderNoSV []byte
	//go:embed axboot.prg
	autoPrg []byte
)

const (
	sector   = 512
	codeMax  = 0x156
	loadMax  = 15 * sector
	cmdSec   = 15
	kernSec  = 16
	modTab   = 256
	modMax   = 8
	icd      = 0x156
	gemSec   = 3 * mib
	gemFat   = (gemSec*2 + sector - 1) / sector
	gemData  = 1 + 2*gemFat + 32 // boot sector, two FATs, 512 root entries
	axbStart = 64
	mib      = 2048 // sectors per MiB
	timeout  = 3
	chunk    = 64 << 10
)

var be = binary.BigEndian

// Params describes one disk. Root and Home are finished filesystems of
// RootMiB and HomeMiB.
type Params struct {
	DiskMiB, SwapMiB int
	Kernel           []byte
	CmdLine          string
	Root, Home       io.ReaderAt
	RootMiB, HomeMiB int
	NoSuperVidel     bool // boot with the loader that has no SuperVidel probe
	Modules          []Module
}

// Module is a boot module the loader reads from AXB.
type Module struct {
	Name string
	Data []byte
}

// Sizes returns the boot partition and /home sizes in MiB for a disk, both
// in 4 MiB steps, as the disk is laid out. bootSize is the kernel's and boot
// modules' bytes.
func Sizes(diskMiB, bootSize, rootMiB, swapMiB int) (axb, home int, err error) {
	axb = (bootSize + (kernSec+modMax)*sector + 4<<20 - 1) / (4 << 20) * 4
	home = (diskMiB - axb - rootMiB - swapMiB - 4) / 4 * 4
	if home < 4 {
		return 0, 0, fmt.Errorf("the disk is too small for this root and swap")
	}
	if home > 2048 {
		home = 2048
	}
	return axb, home, nil
}

// RootSector returns the AHDI root sector (partition table, boot code and
// checksum) for the layout.
func RootSector(diskMiB, axbMiB, rootMiB, swapMiB, homeMiB int) []byte {
	rs := make([]byte, sector)
	be.PutUint32(rs[0x1C2:], uint32(diskMiB*mib))
	st := uint32(axbStart)
	for i, p := range []struct {
		id string
		n  int
	}{{"AXB", axbMiB * mib}, {"AXR", rootMiB * mib}, {"AXS", swapMiB * mib}, {"AXU", homeMiB * mib}} {
		e := rs[0x1C6+12*i:]
		e[0] = 1
		copy(e[1:], p.id)
		be.PutUint32(e[4:], st)
		be.PutUint32(e[8:], uint32(p.n))
		st += uint32(p.n)
	}
	e := rs[icd:]
	e[0] = 1
	copy(e[1:], "GEM")
	be.PutUint32(e[4:], uint32(diskMiB*mib-gemSec))
	be.PutUint32(e[8:], gemSec)
	copy(rs, bootSector)
	be.PutUint16(rs[0x1FE:], 0x1234-wordSum(rs))
	return rs
}

func wordSum(b []byte) uint16 {
	var s uint16
	for i := 0; i < sector; i += 2 {
		s += be.Uint16(b[i:])
	}
	return s
}

// kernelSum is the word sum of the ELF's loaded part, as the loader checks.
func kernelSum(k []byte) (uint32, error) {
	if len(k) < 52 || string(k[:4]) != "\x7fELF" || k[18] != 0 || k[19] != 4 {
		return 0, fmt.Errorf("kernel is not an m68k ELF file")
	}
	phoff, n := int(be.Uint32(k[28:])), int(be.Uint16(k[44:]))
	end := 0
	for i := 0; i < n; i++ {
		p := phoff + 32*i
		if p < 0 || p+32 > len(k) {
			return 0, fmt.Errorf("kernel program headers are outside the file")
		}
		if be.Uint32(k[p:]) == 1 {
			if e := int(be.Uint32(k[p+4:])) + int(be.Uint32(k[p+16:])); e > end {
				end = e
			}
		}
	}
	end += -end % 4
	if end > len(k) {
		k = append(append([]byte{}, k...), make([]byte, end-len(k))...)
	}
	var s uint32
	for i := 0; i < end; i += 4 {
		s += be.Uint32(k[i:])
	}
	return s, nil
}

// Boot returns the AXB contents: loader, command line and module table,
// kernel, modules.
func Boot(kernel []byte, cmd string, mods ...Module) ([]byte, error) {
	return boot(loader, kernel, cmd, mods)
}

func secs(n int) int { return (n + sector - 1) / sector }

func boot(loader, kernel []byte, cmd string, mods []Module) ([]byte, error) {
	if len(cmd) > 255 {
		return nil, fmt.Errorf("command line longer than 255 bytes")
	}
	if len(mods) > modMax {
		return nil, fmt.Errorf("more than %d boot modules", modMax)
	}
	sum, err := kernelSum(kernel)
	if err != nil {
		return nil, err
	}
	n := kernSec + secs(len(kernel))
	for _, m := range mods {
		if len(m.Name) > 15 {
			return nil, fmt.Errorf("module name %s longer than 15 bytes", m.Name)
		}
		n += secs(len(m.Data))
	}
	out := make([]byte, n*sector)
	copy(out, loader)
	be.PutUint32(out[8:], timeout)
	be.PutUint32(out[12:], sum)
	copy(out[cmdSec*sector:], cmd)
	t := out[cmdSec*sector+modTab:]
	copy(t, "BMOD")
	be.PutUint32(t[4:], uint32(len(mods)))
	copy(out[kernSec*sector:], kernel)
	s := kernSec + secs(len(kernel))
	for i, m := range mods {
		e := t[8+24*i:]
		be.PutUint32(e, uint32(s))
		be.PutUint32(e[4:], uint32(len(m.Data)))
		copy(e[8:24], m.Name)
		copy(out[s*sector:], m.Data)
		s += secs(len(m.Data))
	}
	return out, nil
}

// GEMDOS returns the FAT16 tail partition holding \AUTO\AXBOOT.PRG:
// cluster 2 is AUTO, 3 on the program.
func GEMDOS(prg []byte) ([]byte, error) {
	ncl := secs(len(prg))
	if ncl == 0 || gemData+1+ncl > gemSec {
		return nil, fmt.Errorf("AUTO program does not fit the GEMDOS partition")
	}
	le := binary.LittleEndian
	fs := make([]byte, gemSec*sector)
	copy(fs, "\xeb\x3c\x90AXBOOT  ")
	le.PutUint16(fs[11:], sector)
	fs[13] = 1 // sectors per cluster
	le.PutUint16(fs[14:], 1)
	fs[16] = 2
	le.PutUint16(fs[17:], 512)
	le.PutUint16(fs[19:], gemSec)
	fs[21] = 0xF8
	le.PutUint16(fs[22:], gemFat)
	le.PutUint16(fs[24:], 32)
	le.PutUint16(fs[26:], 64)
	fs[0x26] = 0x29
	copy(fs[0x2B:], "AXBOOT     FAT16   ")
	fat := []uint16{0xFFF8, 0xFFFF, 0xFFFF}
	for c := 4; c < 3+ncl; c++ {
		fat = append(fat, uint16(c))
	}
	fat = append(fat, 0xFFFF)
	for f := 0; f < 2; f++ {
		for i, v := range fat {
			le.PutUint16(fs[(1+f*gemFat)*sector+2*i:], v)
		}
	}
	ent := func(b []byte, name string, attr byte, cl, size int) {
		copy(b, name)
		b[11] = attr
		le.PutUint16(b[24:], 0x5521) // date
		le.PutUint16(b[26:], uint16(cl))
		le.PutUint32(b[28:], uint32(size))
	}
	root := fs[(1+2*gemFat)*sector:]
	ent(root, "AXBOOT     ", 0x08, 0, 0)
	ent(root[32:], "AUTO       ", 0x10, 2, 0)
	auto := fs[gemData*sector:]
	ent(auto, ".          ", 0x10, 2, 0)
	ent(auto[32:], "..         ", 0x10, 0, 0)
	ent(auto[64:], "AXBOOT  PRG", 0x20, 3, len(prg))
	copy(auto[sector:], prg)
	return fs, nil
}

// Assemble writes the whole disk to dst in order.
func Assemble(ctx context.Context, dst io.Writer, p Params) error {
	size := len(p.Kernel)
	for _, m := range p.Modules {
		size += len(m.Data)
	}
	axb, home, err := Sizes(p.DiskMiB, size, p.RootMiB, p.SwapMiB)
	if err != nil {
		return err
	}
	if home != p.HomeMiB || len(loader) > loadMax || len(loaderNoSV) > loadMax || len(bootSector) > codeMax || p.DiskMiB > 8192 {
		return fmt.Errorf("layout does not match the filesystems")
	}
	ld := loader
	if p.NoSuperVidel {
		ld = loaderNoSV
	}
	axbImg, err := boot(ld, p.Kernel, p.CmdLine, p.Modules)
	if err != nil {
		return err
	}
	gem, err := GEMDOS(autoPrg)
	if err != nil {
		return err
	}
	if len(axbImg) > axb*mib*sector {
		return fmt.Errorf("boot partition is too small for the kernel")
	}
	pos := int64(0)
	put := func(b []byte) error {
		_, err := dst.Write(b)
		pos += int64(len(b))
		return err
	}
	zero := func(to int64) error {
		z := make([]byte, chunk)
		for pos < to {
			if err := ctx.Err(); err != nil {
				return err
			}
			n := min(int64(len(z)), to-pos)
			if err := put(z[:n]); err != nil {
				return err
			}
		}
		return nil
	}
	copyFS := func(src io.ReaderAt, size int64) error {
		buf := make([]byte, chunk)
		for off := int64(0); off < size; {
			if err := ctx.Err(); err != nil {
				return err
			}
			n := int(min(int64(len(buf)), size-off))
			if m, err := src.ReadAt(buf[:n], off); m != n {
				return fmt.Errorf("read filesystem: %v", err)
			}
			if err := put(buf[:n]); err != nil {
				return err
			}
			off += int64(n)
		}
		return nil
	}
	steps := []func() error{
		func() error { return put(RootSector(p.DiskMiB, axb, p.RootMiB, p.SwapMiB, home)) },
		func() error { return zero(axbStart * sector) },
		func() error { return put(axbImg) },
		func() error { return zero(int64(axbStart+axb*mib) * sector) },
		func() error { return copyFS(p.Root, int64(p.RootMiB)<<20) },
		func() error { return zero(int64(axbStart+(axb+p.RootMiB+p.SwapMiB)*mib) * sector) },
		func() error { return copyFS(p.Home, int64(home)<<20) },
		func() error { return zero(int64(p.DiskMiB*mib-gemSec) * sector) },
		func() error { return put(gem) },
	}
	for _, s := range steps {
		if err := s(); err != nil {
			return err
		}
	}
	return nil
}
