// Package diskimage assembles Apple partitioned disk images with bounded memory.
package diskimage

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"math"
	"strings"
)

const BlockSize int64 = 512
const bufferSize = 64 * 1024
const maxPartitions = 63

var be = binary.BigEndian

type Options struct {
	SwapBytes  int64
	SpareBytes []int64
}

type Partition struct {
	Name       string
	Type       string
	StartBlock uint32
	Blocks     uint32
}

type Layout struct {
	SizeBytes  int64
	Partitions []Partition
	Root       Partition
	Swap       Partition
}

type prepared struct {
	layout  Layout
	header  [512]byte
	entries [][512]byte
}

// Plan validates the boot disk and calculates the resulting layout before writing.
func Plan(boot io.ReaderAt, bootSize, rootSize int64, opts Options) (Layout, error) {
	p, err := prepare(boot, bootSize, rootSize, opts)
	return p.layout, err
}

// Assemble writes a complete image sequentially. Sources must remain unchanged;
// dst must be separate from both sources. On error, discard the partial output.
func Assemble(ctx context.Context, dst io.Writer, boot io.ReaderAt, bootSize int64, root io.ReaderAt, rootSize int64, opts Options) (Layout, error) {
	if err := ctx.Err(); err != nil {
		return Layout{}, err
	}
	p, err := prepare(boot, bootSize, rootSize, opts)
	if err != nil {
		return Layout{}, err
	}
	if err = ValidateRoot(root, rootSize); err != nil {
		return Layout{}, err
	}
	if err = write(ctx, dst, p.header[:]); err != nil {
		return Layout{}, err
	}
	for _, e := range p.entries {
		if err = write(ctx, dst, e[:]); err != nil {
			return Layout{}, err
		}
	}
	buf := make([]byte, bufferSize)
	offset := int64(len(p.entries)+1) * BlockSize
	if err = copyRange(ctx, dst, boot, offset, bootSize-offset, buf); err != nil {
		return Layout{}, fmt.Errorf("boot disk: %w", err)
	}
	if err = copyRange(ctx, dst, root, 0, rootSize, buf); err != nil {
		return Layout{}, fmt.Errorf("root image: %w", err)
	}
	clear(buf)
	remaining := p.layout.SizeBytes - bootSize - rootSize
	for remaining > 0 {
		n := min(remaining, int64(len(buf)))
		if err = write(ctx, dst, buf[:n]); err != nil {
			return Layout{}, err
		}
		remaining -= n
	}
	return p.layout, nil
}

func prepare(boot io.ReaderAt, bootSize, rootSize int64, opts Options) (p prepared, err error) {
	fail := func(s string) (prepared, error) { return prepared{}, fmt.Errorf("diskimage: %s", s) }
	if bootSize < 2*BlockSize || bootSize%BlockSize != 0 || bootSize/BlockSize > math.MaxUint32 {
		return fail("invalid boot disk size")
	}
	if rootSize <= 0 || rootSize%BlockSize != 0 {
		return fail("root size must be positive whole blocks")
	}
	if opts.SwapBytes <= 0 || opts.SwapBytes%BlockSize != 0 {
		return fail("swap size must be positive whole blocks")
	}
	if len(opts.SpareBytes) > maxPartitions-3 {
		return fail("too many spare partitions")
	}
	if _, err = boot.ReadAt(p.header[:], 0); err != nil {
		return p, fmt.Errorf("diskimage: header: %w", err)
	}
	if string(p.header[:2]) != "ER" || be.Uint16(p.header[2:4]) != uint16(BlockSize) {
		return fail("expected DDM with 512-byte blocks")
	}
	bootBlocks := uint32(bootSize / BlockSize)
	if be.Uint32(p.header[4:8]) != bootBlocks {
		return fail("DDM size disagrees with boot disk")
	}
	drivers := int(be.Uint16(p.header[16:18]))
	if drivers > (512-18)/8 {
		return fail("driver descriptor count exceeds block")
	}
	for i := 0; i < drivers; i++ {
		d := p.header[18+i*8 : 26+i*8]
		start, count := uint64(be.Uint32(d)), uint64(be.Uint16(d[4:]))
		if start == 0 || count == 0 || start+count > uint64(bootBlocks) {
			return fail("driver descriptor outside boot disk")
		}
	}
	var first [512]byte
	if _, err = boot.ReadAt(first[:], BlockSize); err != nil {
		return p, fmt.Errorf("diskimage: first partition: %w", err)
	}
	n := int64(be.Uint32(first[4:8]))
	count := n + 2 + int64(len(opts.SpareBytes))
	if n < 1 || count > maxPartitions || (count+1)*BlockSize > bootSize {
		return fail("partition count exceeds map capacity")
	}
	var mapPart *Partition
	for i := int64(1); i <= n; i++ {
		var e [512]byte
		if _, err = boot.ReadAt(e[:], i*BlockSize); err != nil {
			return p, fmt.Errorf("diskimage: partition %d: %w", i, err)
		}
		if string(e[:2]) != "PM" || int64(be.Uint32(e[4:8])) != n {
			return fail("inconsistent partition map")
		}
		part := Partition{cstring(e[16:48]), cstring(e[48:80]), be.Uint32(e[8:12]), be.Uint32(e[12:16])}
		start, size := uint64(part.StartBlock), uint64(part.Blocks)
		if start == 0 || size == 0 || start+size > uint64(bootBlocks) {
			return fail("partition outside boot disk")
		}
		dataStart, dataSize := uint64(be.Uint32(e[80:84])), uint64(be.Uint32(e[84:88]))
		if dataStart+dataSize > size {
			return fail("partition data exceeds partition")
		}
		for _, old := range p.layout.Partitions {
			if start < uint64(old.StartBlock)+uint64(old.Blocks) && uint64(old.StartBlock) < start+size {
				return fail("overlapping partitions")
			}
		}
		if part.Type == "Apple_partition_map" {
			if mapPart != nil {
				return fail("multiple partition maps")
			}
			cp := part
			mapPart = &cp
		}
		p.layout.Partitions = append(p.layout.Partitions, part)
		be.PutUint32(e[4:8], uint32(count))
		p.entries = append(p.entries, e)
	}
	if mapPart == nil || mapPart.StartBlock != 1 || int64(mapPart.Blocks) < count {
		return fail("partition map has no space for added entries")
	}
	total := int64(bootBlocks)
	add := func(name string, bytes int64, fs byte, flags uint16) error {
		if bytes <= 0 || bytes%BlockSize != 0 || bytes/BlockSize > math.MaxUint32-total {
			return fmt.Errorf("diskimage: invalid or overflowing %s size", name)
		}
		part := Partition{name, "Apple_UNIX_SVR2", uint32(total), uint32(bytes / BlockSize)}
		p.layout.Partitions = append(p.layout.Partitions, part)
		p.entries = append(p.entries, entry(uint32(count), part, fs, flags))
		total += bytes / BlockSize
		return nil
	}
	if err = add("Root", rootSize, 1, 0xC000); err != nil {
		return p, err
	}
	p.layout.Root = p.layout.Partitions[len(p.layout.Partitions)-1]
	if err = add("Swap", opts.SwapBytes, 3, 0x2000); err != nil {
		return p, err
	}
	p.layout.Swap = p.layout.Partitions[len(p.layout.Partitions)-1]
	for i, size := range opts.SpareBytes {
		if err = add(fmt.Sprintf("Spare%d", i+1), size, 0, 0); err != nil {
			return p, err
		}
	}
	be.PutUint32(p.header[4:8], uint32(total))
	p.layout.SizeBytes = total * BlockSize
	return p, nil
}

func entry(n uint32, p Partition, fs byte, flags uint16) (e [512]byte) {
	copy(e[:], "PM")
	be.PutUint32(e[4:8], n)
	be.PutUint32(e[8:12], p.StartBlock)
	be.PutUint32(e[12:16], p.Blocks)
	copy(e[16:48], p.Name)
	copy(e[48:80], p.Type)
	be.PutUint32(e[84:88], p.Blocks)
	be.PutUint32(e[88:92], 0x37)
	if fs != 0 {
		be.PutUint32(e[0x88:0x8c], 0xABADBABE)
		e[0x8d] = fs
		be.PutUint16(e[0x8e:0x90], 1)
		be.PutUint16(e[0x90:0x92], flags)
	}
	return e
}

func cstring(b []byte) string { return strings.SplitN(string(b), "\x00", 2)[0] }

func write(ctx context.Context, dst io.Writer, b []byte) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	n, err := dst.Write(b)
	if err == nil && n != len(b) {
		err = io.ErrShortWrite
	}
	return err
}

func copyRange(ctx context.Context, dst io.Writer, src io.ReaderAt, offset, size int64, buf []byte) error {
	for size > 0 {
		if err := ctx.Err(); err != nil {
			return err
		}
		want := int(min(size, int64(len(buf))))
		n, err := src.ReadAt(buf[:want], offset)
		if err != nil && !(err == io.EOF && n == want) {
			return err
		}
		if n != want {
			return io.ErrUnexpectedEOF
		}
		if err = write(ctx, dst, buf[:n]); err != nil {
			return err
		}
		offset += int64(n)
		size -= int64(n)
	}
	return nil
}

// ValidateRoot checks the big-endian UFS superblock and filesystem size.
func ValidateRoot(root io.ReaderAt, size int64) error {
	if size < 8192+1380 || size%BlockSize != 0 {
		return fmt.Errorf("diskimage: root image is too small or unaligned")
	}
	var sb [1380]byte
	if _, err := root.ReadAt(sb[:], 8192); err != nil {
		return fmt.Errorf("diskimage: root superblock: %w", err)
	}
	if be.Uint32(sb[1372:]) != 0x011954 {
		return fmt.Errorf("diskimage: invalid big-endian UFS magic")
	}
	blocks, fragments := uint64(be.Uint32(sb[48:])), uint64(be.Uint32(sb[52:]))
	if blocks < 4096 || blocks > 65536 || blocks&(blocks-1) != 0 || fragments < 512 || fragments > blocks || fragments&(fragments-1) != 0 || blocks/fragments > 8 || uint64(be.Uint32(sb[56:])) != blocks/fragments {
		return fmt.Errorf("diskimage: invalid UFS block geometry")
	}
	if uint64(be.Uint32(sb[36:]))*fragments != uint64(size) {
		return fmt.Errorf("diskimage: UFS size disagrees with root image")
	}
	if be.Uint32(sb[32:])+be.Uint32(sb[132:]) != 0x7c269d38 {
		return fmt.Errorf("diskimage: UFS filesystem is not clean")
	}
	return nil
}
