package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"amigaux.org/imagebuilder/packages"
	"amigaux.org/imagebuilder/svr4"
)

func setup(t *testing.T) (string, string) {
	t.Helper()
	dir := t.TempDir()
	b, e := os.ReadFile("testdata/resolve.json")
	if e != nil {
		t.Fatal(e)
	}
	request := filepath.Join(dir, "resolve.json")
	os.WriteFile(request, b, 0600)
	lock := filepath.Join(dir, "lock.json")
	if e := run(context.Background(), []string{"resolve", "-request", request, "-output", lock}, &bytes.Buffer{}); e != nil {
		t.Fatal(e)
	}
	return dir, lock
}
func TestCLIImagePackage(t *testing.T) {
	dir, lock := setup(t)
	req := buildRequest{Lock: lock, EnvironmentID: "first", PackageID: "amiga.fixture", Tier: "environment", Info: svr4.Info{Package: "ASHtest", Name: "Test", Version: "1", Architecture: "m68k", BaseDir: "/amiga/apps/test"}, Output: "fixture.pkg", Receipt: "receipt.json"}
	b, _ := json.Marshal(req)
	p := filepath.Join(dir, "build.json")
	os.WriteFile(p, b, 0600)
	if e := run(context.Background(), []string{"build", "-request", p}, &bytes.Buffer{}); e != nil {
		t.Fatal(e)
	}
	pkg, e := os.ReadFile(filepath.Join(dir, "fixture.pkg"))
	if e != nil {
		t.Fatal(e)
	}
	if _, es, e := svr4.Import(context.Background(), bytes.NewReader(pkg), int64(len(pkg)), svr4.Options{}); e != nil || len(es) != 2 {
		t.Fatal(e)
	}
	receipt, _ := os.ReadFile(filepath.Join(dir, "receipt.json"))
	if _, e := packages.DecodeReceipt(receipt); e != nil {
		t.Fatal(e)
	}
	if e := run(context.Background(), []string{"build", "-request", p}, &bytes.Buffer{}); e == nil {
		t.Fatal("overwrote output")
	}
}
func TestInvalidLockPublishesNothing(t *testing.T) {
	dir, lock := setup(t)
	b, _ := os.ReadFile(lock)
	var l packages.Lock
	json.Unmarshal(b, &l)
	l.Bindings[0].Recipe.Status = "planned"
	l.Bindings[0].RecipeSHA256 = packages.HashRecipe(l.Bindings[0].Recipe)
	b, _ = json.Marshal(l)
	os.WriteFile(lock, b, 0600)
	req := buildRequest{Lock: lock, EnvironmentID: "first", PackageID: "amiga.fixture", Tier: "environment", Output: "fixture.pkg", Receipt: "receipt.json"}
	b, _ = json.Marshal(req)
	p := filepath.Join(dir, "build.json")
	os.WriteFile(p, b, 0600)
	if e := run(context.Background(), []string{"build", "-request", p}, &bytes.Buffer{}); e == nil {
		t.Fatal("planned accepted")
	}
	if _, e := os.Stat(filepath.Join(dir, "fixture.pkg")); !os.IsNotExist(e) {
		t.Fatal("failure published output")
	}
}
func TestStrictJSON(t *testing.T) {
	for _, raw := range []string{`{"catalog":[],"requests":[],"shell":"bad"}`, `{"catalog":[],"requests":[]} {}`} {
		dir := t.TempDir()
		p := filepath.Join(dir, "bad.json")
		os.WriteFile(p, []byte(raw), 0600)
		if e := run(context.Background(), []string{"resolve", "-request", p}, &bytes.Buffer{}); e == nil {
			t.Fatal("accepted", raw)
		}
	}
}
func TestPublishPreflight(t *testing.T) {
	dir := t.TempDir()
	a, b := filepath.Join(dir, "app.pkg"), filepath.Join(dir, "receipt.json")
	os.WriteFile(b, []byte("original"), 0600)
	if e := publish([]string{a, b}, [][]byte{[]byte("app"), []byte("new")}); e == nil {
		t.Fatal("overwrite accepted")
	}
	if _, e := os.Stat(a); !os.IsNotExist(e) {
		t.Fatal("partial output")
	}
	data, _ := os.ReadFile(b)
	if string(data) != "original" {
		t.Fatal("changed receipt")
	}
}
