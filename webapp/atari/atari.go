// Package atari writes the Falcon disk layout: an AHDI root sector with
// boot code, a boot partition (AXB) holding the loader, command line and
// kernel, then root (AXR), swap (AXS) and /home (AXU) partitions.
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
)

const (
	sector   = 512
	codeMax  = 0x156
	loadMax  = 15 * sector
	cmdSec   = 15
	kernSec  = 16
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
}

// Sizes returns the boot partition and /home sizes in MiB for a disk, both
// in 4 MiB steps, as the disk is laid out.
func Sizes(diskMiB, kernelSize, rootMiB, swapMiB int) (axb, home int, err error) {
	axb = (kernelSize + kernSec*sector + 4<<20 - 1) / (4 << 20) * 4
	home = (diskMiB - axb - rootMiB - swapMiB - 1) / 4 * 4
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

// Boot returns the AXB contents: loader, command line, kernel.
func Boot(kernel []byte, cmd string) ([]byte, error) { return boot(loader, kernel, cmd) }

func boot(loader, kernel []byte, cmd string) ([]byte, error) {
	if len(cmd) > 255 {
		return nil, fmt.Errorf("command line longer than 255 bytes")
	}
	sum, err := kernelSum(kernel)
	if err != nil {
		return nil, err
	}
	out := make([]byte, kernSec*sector+(len(kernel)+sector-1)/sector*sector)
	copy(out, loader)
	be.PutUint32(out[8:], timeout)
	be.PutUint32(out[12:], sum)
	copy(out[cmdSec*sector:], cmd)
	copy(out[kernSec*sector:], kernel)
	return out, nil
}

// Assemble writes the whole disk to dst in order.
func Assemble(ctx context.Context, dst io.Writer, p Params) error {
	axb, home, err := Sizes(p.DiskMiB, len(p.Kernel), p.RootMiB, p.SwapMiB)
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
	axbImg, err := boot(ld, p.Kernel, p.CmdLine)
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
		func() error { return zero(int64(p.DiskMiB) * mib * sector) },
	}
	for _, s := range steps {
		if err := s(); err != nil {
			return err
		}
	}
	return nil
}
