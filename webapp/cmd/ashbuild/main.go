package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"syscall"

	"amigaux.org/imagebuilder/imagebuilder"
)

func run() error {
	requestPath := flag.String("request", "", "JSON build request")
	flag.Parse()
	if *requestPath == "" || flag.NArg() != 0 {
		return fmt.Errorf("usage: ashbuild -request build.json (writes JSON receipt to stdout)")
	}
	f, err := os.Open(*requestPath)
	if err != nil {
		return err
	}
	defer f.Close()
	var request imagebuilder.Request
	d := json.NewDecoder(io.LimitReader(f, 1024*1024))
	d.DisallowUnknownFields()
	if err := d.Decode(&request); err != nil {
		return err
	}
	var extra any
	if err := d.Decode(&extra); err != io.EOF {
		return fmt.Errorf("expected one JSON request")
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	b := imagebuilder.Builder{Log: os.Stderr, Progress: func(e imagebuilder.Event) { fmt.Fprintf(os.Stderr, "[%s] %s\n", e.Stage, e.Message) }}
	receipt, err := b.Build(ctx, request)
	if err != nil {
		return err
	}
	e := json.NewEncoder(os.Stdout)
	e.SetIndent("", "  ")
	return e.Encode(receipt)
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "ashbuild:", err)
		os.Exit(1)
	}
}
