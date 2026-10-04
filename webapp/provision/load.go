package provision

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"

	"amigaux.org/imagebuilder/svr4"
	"amigaux.org/imagebuilder/ufs"
)

type Artifact struct {
	Kind   string `json:"kind"`
	Family string `json:"family"`
	ID     string `json:"id"`
	SHA256 string `json:"sha256"`
	Size   int64  `json:"size"`
}

// Stage verifies and freezes each datastream before adding its system payload.
func Stage(ctx context.Context, base []ufs.Entry, artifacts []Artifact, readers []io.ReaderAt) ([]ufs.Entry, error) {
	if len(artifacts) != len(readers) || len(artifacts) > 64 {
		return nil, fmt.Errorf("expected at most 64 package readers")
	}
	var templates, apps []Bundle
	var total int64
	for i, a := range artifacts {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		if readers[i] == nil {
			return nil, fmt.Errorf("missing package reader")
		}
		if a.Kind != "template" && a.Kind != "app" {
			return nil, fmt.Errorf("invalid provisioning kind")
		}
		total += a.Size
		if a.Size <= 0 || a.Size > 256<<20 || total > 256<<20 {
			return nil, fmt.Errorf("provisioning packages exceed 256 MiB working budget")
		}
		b := make([]byte, int(a.Size))
		for off := int64(0); off < a.Size; {
			if err := ctx.Err(); err != nil {
				return nil, err
			}
			n := int64(65536)
			if n > a.Size-off {
				n = a.Size - off
			}
			if _, err := readers[i].ReadAt(b[off:off+n], off); err != nil {
				return nil, err
			}
			off += n
		}
		sum := sha256.Sum256(b)
		if hex.EncodeToString(sum[:]) != a.SHA256 {
			return nil, fmt.Errorf("provisioning package digest mismatch")
		}
		_, entries, err := svr4.Import(ctx, bytes.NewReader(b), a.Size, svr4.Options{})
		if err != nil {
			return nil, err
		}
		bundle := Bundle{Family: a.Family, ID: a.ID, Entries: entries}
		if a.Kind == "template" {
			templates = append(templates, bundle)
		} else {
			apps = append(apps, bundle)
		}
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return Apply(base, DefaultPolicies(), templates, apps)
}
