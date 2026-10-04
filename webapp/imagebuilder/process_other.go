//go:build !linux && !darwin

package imagebuilder

import "os/exec"

func configureProcess(cmd *exec.Cmd) {}
