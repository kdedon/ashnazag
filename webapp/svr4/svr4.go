// Package svr4 reads and writes relocatable, script-free SVR4 datastreams.
package svr4

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"path"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"amigaux.org/imagebuilder/media"
	"amigaux.org/imagebuilder/ufs"
)

type Info struct {
	Package      string `json:"package"`
	Name         string `json:"name"`
	Version      string `json:"version"`
	Architecture string `json:"architecture"`
	BaseDir      string `json:"baseDir"`
	Timestamp    uint32 `json:"timestamp"`
}
type Options struct {
	Limits           media.Limits
	MaxMetadataBytes uint64
}

var packageName = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9]{0,8}$`)

func safe(p string) bool {
	return p != "" && p != "." && !strings.HasPrefix(p, "/") && path.Clean(p) == p && p != ".." && !strings.HasPrefix(p, "../") && !strings.ContainsAny(p, "\t\r\n\x00=\\\"'`$")
}
func validInfo(v Info) error {
	if !packageName.MatchString(v.Package) {
		return fmt.Errorf("invalid SVR4 package abbreviation")
	}
	for _, s := range []string{v.Name, v.Version, v.Architecture} {
		if s == "" || strings.ContainsAny(s, "\r\n\x00\"'`$\\") {
			return fmt.Errorf("invalid pkginfo value")
		}
	}
	if strings.ContainsAny(v.Architecture, " \t") || v.BaseDir == "" || !strings.HasPrefix(v.BaseDir, "/") || path.Clean(v.BaseDir) != v.BaseDir || strings.ContainsAny(v.BaseDir, " \t\r\n\x00\"'`$\\=") {
		return fmt.Errorf("invalid architecture or BASEDIR")
	}
	return nil
}
func validate(entries []ufs.Entry) ([]ufs.Entry, error) {
	es := append([]ufs.Entry(nil), entries...)
	for i := range es {
		if es[i].Mode <= 07777 {
			es[i].Mode &^= 06022 // no set-id, no group/other write
		}
	}
	sort.Slice(es, func(i, j int) bool { return es[i].Path < es[j].Path })
	seen := map[string]ufs.Entry{}
	for _, e := range es {
		if !strings.HasPrefix(e.Path, "/") || !safe(strings.TrimPrefix(e.Path, "/")) {
			return nil, fmt.Errorf("invalid package path %q", e.Path)
		}
		if _, ok := seen[e.Path]; ok {
			return nil, fmt.Errorf("duplicate path %q", e.Path)
		}
		if e.Mode > 07777 || e.Size < 0 || e.Size > 0xffffffff {
			return nil, fmt.Errorf("invalid metadata %q", e.Path)
		}
		switch e.Kind {
		case 'f':
			if e.Size > 0 && e.Data == nil {
				return nil, fmt.Errorf("missing data %q", e.Path)
			}
		case 'd':
		case 'h':
			if !strings.HasPrefix(e.Target, "/") || !safe(strings.TrimPrefix(e.Target, "/")) {
				return nil, fmt.Errorf("unsafe hardlink")
			}
		case 'l':
			if !safe(e.Target) || strings.HasPrefix(path.Clean(path.Join(path.Dir(e.Path), e.Target)), "/../") {
				return nil, fmt.Errorf("unsafe symlink")
			}
		default:
			return nil, fmt.Errorf("unsupported entry kind %q", e.Kind)
		}
		seen[e.Path] = e
	}
	for _, e := range es {
		for p := path.Dir(e.Path); p != "/"; p = path.Dir(p) {
			if x, ok := seen[p]; ok && x.Kind != 'd' {
				return nil, fmt.Errorf("non-directory parent %q", p)
			}
		}
		if e.Kind == 'h' {
			x, ok := seen[e.Target]
			if !ok || x.Kind != 'f' {
				return nil, fmt.Errorf("hardlink target must be regular file")
			}
		}
	}
	return es, nil
}
func checksum(ctx context.Context, r io.ReaderAt, n int64) (uint32, error) {
	var sum uint64
	b := make([]byte, 64<<10)
	for off := int64(0); off < n; {
		if err := ctx.Err(); err != nil {
			return 0, err
		}
		size := int64(len(b))
		if size > n-off {
			size = n - off
		}
		if _, err := r.ReadAt(b[:size], off); err != nil {
			return 0, err
		}
		for _, v := range b[:size] {
			sum += uint64(v)
		}
		off += size
	}
	for sum>>16 != 0 {
		sum = (sum & 65535) + (sum >> 16)
	}
	return uint32(sum), nil
}

type writer struct {
	w   io.Writer
	n   int64
	ctx context.Context
}

func (w *writer) put(b []byte) error {
	if e := w.ctx.Err(); e != nil {
		return e
	}
	n, e := w.w.Write(b)
	w.n += int64(n)
	if e == nil && n != len(b) {
		e = io.ErrShortWrite
	}
	return e
}
func (w *writer) pad(n int64) error { return w.put(make([]byte, (n-w.n%n)%n)) }
func (w *writer) entry(name string, mode, uid, gid, stamp, ino uint32, r io.ReaderAt, size int64) error {
	h := fmt.Sprintf("070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x", ino, mode, uid, gid, 1, stamp, uint32(size), 0, 0, 0, 0, len(name)+1, 0)
	if e := w.put([]byte(h + name + "\x00")); e != nil {
		return e
	}
	if e := w.pad(4); e != nil {
		return e
	}
	b := make([]byte, 64<<10)
	for off := int64(0); off < size; {
		n := int64(len(b))
		if n > size-off {
			n = size - off
		}
		if _, e := r.ReadAt(b[:n], off); e != nil {
			return e
		}
		if e := w.put(b[:n]); e != nil {
			return e
		}
		off += n
	}
	return w.pad(4)
}
func (w *writer) end() error {
	if e := w.entry("TRAILER!!!", 0, 0, 0, 0, 0, nil, 0); e != nil {
		return e
	}
	return w.pad(512)
}
func stamp(e ufs.Entry, v Info) uint32 {
	if e.ModTime != nil {
		return *e.ModTime
	}
	return v.Timestamp
}
func Emit(ctx context.Context, dst io.Writer, v Info, entries []ufs.Entry) error {
	if e := validInfo(v); e != nil {
		return e
	}
	es, e := validate(entries)
	if e != nil {
		return e
	}
	info := []byte(fmt.Sprintf("PKG=%s\nNAME=%s\nARCH=%s\nVERSION=%s\nCATEGORY=application\nCLASSES=none\nBASEDIR=%s\n", v.Package, v.Name, v.Architecture, v.Version, v.BaseDir))
	ck, _ := checksum(ctx, bytes.NewReader(info), int64(len(info)))
	var lines strings.Builder
	fmt.Fprintf(&lines, "1 i pkginfo %d %d %d\n", len(info), ck, v.Timestamp)
	var blocks int64 = 16
	for _, x := range es {
		p := strings.TrimPrefix(x.Path, "/")
		switch x.Kind {
		case 'f':
			c, e := checksum(ctx, x.Data, x.Size)
			if e != nil {
				return e
			}
			fmt.Fprintf(&lines, "1 f none %s %04o %d %d %d %d %d\n", quote(p), x.Mode, x.UID, x.GID, x.Size, c, stamp(x, v))
			blocks += (x.Size+511)/512 + 1
		case 'd':
			fmt.Fprintf(&lines, "1 d none %s %04o %d %d\n", quote(p), x.Mode, x.UID, x.GID)
		case 'l':
			fmt.Fprintf(&lines, "1 s none %s=%s\n", quote(p), quote(x.Target))
		case 'h':
			fmt.Fprintf(&lines, "1 l none %s=%s\n", quote(p), quote(strings.TrimPrefix(x.Target, "/")))
		}
	}
	pkgmap := []byte(fmt.Sprintf(": 1 %d\n", blocks) + lines.String())
	w := writer{w: dst, ctx: ctx}
	if e := w.put([]byte(fmt.Sprintf("# PaCkAgE DaTaStReAm\n%s 1 %d\n# end of header\n", v.Package, blocks))); e != nil {
		return e
	}
	if e := w.pad(512); e != nil {
		return e
	}
	for i, x := range []struct {
		n string
		b []byte
	}{{v.Package + "/pkginfo", info}, {v.Package + "/pkgmap", pkgmap}} {
		if e := w.entry(x.n, 0100644, 0, 0, v.Timestamp, uint32(i+1), bytes.NewReader(x.b), int64(len(x.b))); e != nil {
			return e
		}
	}
	if e := w.end(); e != nil {
		return e
	}
	// pkgadd expects pkginfo in each payload archive.
	if e := w.entry("pkginfo", 0100644, 0, 0, v.Timestamp, 1, bytes.NewReader(info), int64(len(info))); e != nil {
		return e
	}
	for i, x := range es {
		if x.Kind != 'f' {
			continue
		}
		if e := w.entry("reloc/"+strings.TrimPrefix(x.Path, "/"), 0100000|x.Mode, x.UID, x.GID, stamp(x, v), uint32(i+2), x.Data, x.Size); e != nil {
			return e
		}
	}
	return w.end()
}

type archiveEntry struct {
	header media.CPIOHeader
	data   io.ReaderAt
}

func archive(ctx context.Context, src io.ReaderAt, offset, size int64, opt Options) (map[string]archiveEntry, int64, error) {
	r := media.NewCPIOReader(io.NewSectionReader(src, offset, size-offset), opt.Limits)
	out := map[string]archiveEntry{}
	var metadata uint64
	for {
		if e := ctx.Err(); e != nil {
			return nil, 0, e
		}
		h, e := r.Next()
		if e == io.EOF {
			break
		}
		if e != nil {
			return nil, 0, e
		}
		if _, ok := out[h.Name]; ok {
			return nil, 0, fmt.Errorf("duplicate archive path")
		}
		metadata += uint64(len(h.Name) + 256)
		if metadata > opt.MaxMetadataBytes {
			return nil, 0, fmt.Errorf("metadata limit exceeded")
		}
		out[h.Name] = archiveEntry{*h, io.NewSectionReader(src, offset+int64(r.Offset()), int64(h.Size))}
		if _, e := io.Copy(io.Discard, r); e != nil {
			return nil, 0, e
		}
	}
	return out, offset + int64(r.Offset()), nil
}
func readText(x archiveEntry, limit uint64) (string, error) {
	if uint64(x.header.Size) > limit || x.header.Mode&0170000 != 0100000 {
		return "", fmt.Errorf("invalid metadata file")
	}
	b, e := io.ReadAll(io.NewSectionReader(x.data, 0, int64(x.header.Size)))
	return string(b), e
}
func number(s string, base int) (uint32, error) {
	n, e := strconv.ParseUint(s, base, 32)
	return uint32(n), e
}
func Import(ctx context.Context, src io.ReaderAt, size int64, opt Options) (Info, []ufs.Entry, error) {
	fail := func(e error) (Info, []ufs.Entry, error) { return Info{}, nil, e }
	if size < 512 || src == nil {
		return fail(fmt.Errorf("short datastream"))
	}
	if opt.MaxMetadataBytes == 0 {
		opt.MaxMetadataBytes = 16 << 20
	}
	if opt.Limits.MaxArchiveBytes == 0 {
		opt.Limits.MaxArchiveBytes = 8 << 30
	}
	if uint64(size) > opt.Limits.MaxArchiveBytes {
		return fail(fmt.Errorf("datastream size limit exceeded"))
	}
	header := make([]byte, 512)
	if _, e := src.ReadAt(header, 0); e != nil {
		return fail(e)
	}
	end := bytes.Index(header, []byte("# end of header\n"))
	if end < 0 {
		return fail(fmt.Errorf("invalid stream header"))
	}
	ls := strings.Split(strings.TrimSpace(string(header[:end])), "\n")
	if len(ls) != 2 || ls[0] != "# PaCkAgE DaTaStReAm" {
		return fail(fmt.Errorf("one package required"))
	}
	fields := strings.Fields(ls[1])
	if len(fields) != 3 || fields[1] != "1" || !packageName.MatchString(fields[0]) {
		return fail(fmt.Errorf("single-volume single-part package required"))
	}
	if _, e := number(fields[2], 10); e != nil {
		return fail(e)
	}
	meta, off, e := archive(ctx, src, 512, size, opt)
	if e != nil {
		return fail(e)
	}
	if len(meta) != 2 {
		return fail(fmt.Errorf("unexpected metadata or install scripts"))
	}
	a, ok := meta[fields[0]+"/pkginfo"]
	if !ok {
		return fail(fmt.Errorf("missing pkginfo"))
	}
	info, e := readText(a, opt.MaxMetadataBytes/2)
	if e != nil {
		return fail(e)
	}
	a, ok = meta[fields[0]+"/pkgmap"]
	if !ok {
		return fail(fmt.Errorf("missing pkgmap"))
	}
	pm, e := readText(a, opt.MaxMetadataBytes/2)
	if e != nil {
		return fail(e)
	}
	kv := map[string]string{}
	for _, line := range strings.Split(strings.TrimSpace(info), "\n") {
		k, v, ok := strings.Cut(line, "=")
		if !ok || kv[k] != "" {
			return fail(fmt.Errorf("invalid pkginfo"))
		}
		switch k {
		case "PKG", "NAME", "VERSION", "ARCH", "BASEDIR", "CATEGORY", "CLASSES":
		default:
			return fail(fmt.Errorf("unsupported pkginfo key %s", k))
		}
		kv[k] = v
	}
	v := Info{Package: kv["PKG"], Name: kv["NAME"], Version: kv["VERSION"], Architecture: kv["ARCH"], BaseDir: kv["BASEDIR"]}
	if e := validInfo(v); e != nil {
		return fail(e)
	}
	if v.Package != fields[0] || kv["CLASSES"] != "none" {
		return fail(fmt.Errorf("package or class mismatch"))
	}
	off = (off + 511) &^ 511
	payload, endoff, e := archive(ctx, src, off, size, opt)
	if e != nil {
		return fail(e)
	}
	if (endoff+511)&^511 != size {
		return fail(fmt.Errorf("trailing data or extra part"))
	}
	if a, ok := payload["pkginfo"]; ok {
		s, e := readText(a, opt.MaxMetadataBytes/2)
		if e != nil || s != info {
			return fail(fmt.Errorf("payload pkginfo mismatch"))
		}
		delete(payload, "pkginfo")
	}
	lines := strings.Split(strings.TrimSpace(pm), "\n")
	if len(lines) < 2 {
		return fail(fmt.Errorf("empty pkgmap"))
	}
	f := strings.Fields(lines[0])
	if len(f) != 3 || f[0] != ":" || f[1] != "1" {
		return fail(fmt.Errorf("invalid pkgmap volume count"))
	}
	entries := []ufs.Entry{}
	haveInfo := false
	for _, line := range lines[1:] {
		f, parseErr := mapFields(line)
		if parseErr != nil {
			return fail(parseErr)
		}
		if len(f) < 3 || f[0] != "1" {
			return fail(fmt.Errorf("invalid pkgmap entry"))
		}
		if f[1] == "i" {
			if len(f) != 6 || f[2] != "pkginfo" || haveInfo {
				return fail(fmt.Errorf("scripts or duplicate pkginfo forbidden"))
			}
			want, e := number(f[3], 10)
			if e != nil || want != uint32(len(info)) {
				return fail(fmt.Errorf("pkginfo size mismatch"))
			}
			want, e = number(f[4], 10)
			got, _ := checksum(ctx, strings.NewReader(info), int64(len(info)))
			if e != nil || want != got {
				return fail(fmt.Errorf("pkginfo checksum mismatch"))
			}
			v.Timestamp, e = number(f[5], 10)
			if e != nil {
				return fail(e)
			}
			haveInfo = true
			continue
		}
		if len(f) < 4 || f[2] != "none" {
			return fail(fmt.Errorf("unsupported class"))
		}
		p, target, _ := strings.Cut(f[3], "=")
		if !safe(p) {
			return fail(fmt.Errorf("unsafe pkgmap path"))
		}
		x := ufs.Entry{Path: "/" + p}
		switch f[1] {
		case "s", "l":
			if len(f) != 4 || target == "" {
				return fail(fmt.Errorf("invalid link"))
			}
			x.Kind = 'l'
			x.Target = target
			if f[1] == "l" {
				x.Kind = 'h'
				x.Target = "/" + target
			}
		case "d", "f":
			n := 7
			if f[1] == "f" {
				n = 10
			}
			if len(f) != n || target != "" {
				return fail(fmt.Errorf("invalid file metadata"))
			}
			x.Kind = f[1][0]
			x.Mode, e = number(f[4], 8)
			if e != nil {
				return fail(e)
			}
			x.UID, e = owner(f[5], false)
			if e != nil {
				return fail(e)
			}
			x.GID, e = owner(f[6], true)
			if e != nil {
				return fail(e)
			}
			if x.Kind == 'f' {
				a, ok := payload["reloc/"+p]
				if !ok || a.header.Mode&0170000 != 0100000 {
					return fail(fmt.Errorf("missing regular payload %s", p))
				}
				sz, e := number(f[7], 10)
				if e != nil || sz != a.header.Size {
					return fail(fmt.Errorf("payload size mismatch"))
				}
				ck, e := number(f[8], 10)
				got, ce := checksum(ctx, a.data, int64(sz))
				if e != nil || ce != nil || ck != got {
					return fail(fmt.Errorf("payload checksum mismatch"))
				}
				tm, e := number(f[9], 10)
				if e != nil {
					return fail(e)
				}
				x.ModTime = &tm
				x.Size = int64(sz)
				x.Data = a.data
				delete(payload, "reloc/"+p)
			}
		default:
			return fail(fmt.Errorf("unsupported pkgmap type %s", f[1]))
		}
		entries = append(entries, x)
	}
	if !haveInfo || len(payload) != 0 {
		return fail(fmt.Errorf("unlisted payload or missing pkginfo"))
	}
	entries, e = validate(entries)
	if e != nil {
		return fail(e)
	}
	return v, entries, nil
}
func owner(s string, group bool) (uint32, error) {
	if s == "root" {
		return 0, nil
	}
	if s == "bin" {
		return 2, nil
	}
	if s == "sys" {
		return 3, nil
	}
	if s == "adm" {
		return 4, nil
	}
	if (!group && s == "daemon") || (group && s == "other") {
		return 1, nil
	}
	return number(s, 10)
}

func quote(s string) string {
	if strings.Contains(s, " ") {
		return "'" + s + "'"
	}
	return s
}
func mapFields(line string) ([]string, error) {
	out := []string{}
	for i := 0; i < len(line); {
		for i < len(line) && (line[i] == ' ' || line[i] == '\t') {
			i++
		}
		if i == len(line) {
			break
		}
		var token strings.Builder
		for i < len(line) && line[i] != ' ' && line[i] != '\t' {
			if line[i] == '\'' {
				if token.Len() != 0 && !strings.HasSuffix(token.String(), "=") {
					return nil, fmt.Errorf("misplaced pkgmap quote")
				}
				i++
				start := i
				for i < len(line) && line[i] != '\'' {
					i++
				}
				if i == len(line) {
					return nil, fmt.Errorf("unterminated pkgmap quote")
				}
				if i == start {
					return nil, fmt.Errorf("empty pkgmap quoted path")
				}
				token.WriteString(line[start:i])
				i++
				if i < len(line) && line[i] != ' ' && line[i] != '\t' && line[i] != '=' {
					return nil, fmt.Errorf("invalid pkgmap quote suffix")
				}
			} else {
				token.WriteByte(line[i])
				i++
			}
		}
		out = append(out, token.String())
	}
	return out, nil
}
