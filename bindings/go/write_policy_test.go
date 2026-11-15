package fengqudb

import (
	"errors"
	"path/filepath"
	"testing"
)

func TestWritePolicy(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "players")
	players, err := Open(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = players.Close() }()
	world, err := Open(filepath.Join(root, "world"), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer world.Close()
	check := func(db *Store, want WriteOptions) {
		t.Helper()
		got, err := db.WriteOptions()
		if err != nil || got != want {
			t.Fatalf("write options: %+v %v, want %+v", got, err, want)
		}
	}
	defaults := DefaultWriteOptions()
	check(players, defaults)
	policy := WriteOptions{Mode: WritePerformance, PeriodMs: 75, ThresholdBytes: 2 << 20}
	if err := players.ConfigureWrite(policy); err != nil {
		t.Fatal(err)
	}
	if err := players.Put([]byte("name"), []byte("Alice")); err != nil {
		t.Fatal(err)
	}
	if value, err := players.Get([]byte("name")); err != nil || string(value) != "Alice" {
		t.Fatalf("memory read: %q %v", value, err)
	}
	policy.Mode = WriteReliable
	if err := players.ConfigureWrite(policy); err != nil {
		t.Fatal(err)
	}
	if err := players.Put([]byte("paid"), []byte("yes")); err != nil {
		t.Fatal(err)
	}
	info, err := players.Info()
	if err != nil || info.Version != info.DurableVersion {
		t.Fatalf("not durable: %+v %v", info, err)
	}
	check(world, defaults)
	for _, bad := range []WriteOptions{
		{Mode: WriteMode(99), PeriodMs: 50, ThresholdBytes: 1 << 20},
		{Mode: WritePerformance, PeriodMs: 0, ThresholdBytes: 1 << 20},
		{Mode: WritePerformance, PeriodMs: 50, ThresholdBytes: 65 << 20},
	} {
		if err := players.ConfigureWrite(bad); err == nil {
			t.Fatal("invalid policy accepted")
		}
		check(players, policy)
	}
	if err := players.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := players.WriteOptions(); !errors.Is(err, ErrClosed) {
		t.Fatalf("closed get: %v", err)
	}
	if err := players.ConfigureWrite(defaults); !errors.Is(err, ErrClosed) {
		t.Fatalf("closed configure: %v", err)
	}
	if err := players.ResetWriteOptions(); !errors.Is(err, ErrClosed) {
		t.Fatalf("closed reset: %v", err)
	}
	players, err = Open(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	check(players, policy)
	if value, err := players.Get([]byte("paid")); err != nil || string(value) != "yes" {
		t.Fatalf("reopen read: %q %v", value, err)
	}
	if err := players.ResetWriteOptions(); err != nil {
		t.Fatal(err)
	}
	check(players, defaults)
	if err := players.Close(); err != nil {
		t.Fatal(err)
	}
	players, err = Open(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	check(players, defaults)
}
