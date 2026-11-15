// Development entry for the copyable Go SDK: generate, build/run and deploy the engine.
package main

import (
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

func execute(name string, args []string, native string) error {
	command := exec.Command(name, args...)
	command.Stdin, command.Stdout, command.Stderr = os.Stdin, os.Stdout, os.Stderr
	command.Env = os.Environ()
	key := "LD_LIBRARY_PATH"
	if runtime.GOOS == "windows" {
		key = "PATH"
	} else if runtime.GOOS == "darwin" {
		key = "DYLD_LIBRARY_PATH"
	}
	// Replace rather than append duplicate environment keys, including Windows Path.
	for i := len(command.Env) - 1; i >= 0; i-- {
		if strings.EqualFold(strings.SplitN(command.Env[i], "=", 2)[0], key) {
			command.Env = append(command.Env[:i], command.Env[i+1:]...)
		}
	}
	command.Env = append(command.Env, key+"="+native+string(os.PathListSeparator)+os.Getenv(key))
	return command.Run()
}

func run() error {
	if len(os.Args) < 2 || (os.Args[1] != "build" && os.Args[1] != "run") {
		return fmt.Errorf("use the SDK build or run script")
	}
	_, source, _, ok := runtime.Caller(0)
	if !ok {
		return fmt.Errorf("cannot locate the SDK")
	}
	sdk := filepath.Dir(filepath.Dir(source))
	native := filepath.Join(sdk, "Native")
	root, err := os.Getwd()
	if err != nil {
		return err
	}
	marked := false
	err = filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() {
			if path != root && (path == sdk || strings.HasPrefix(entry.Name(), ".") || entry.Name() == "Generated" || entry.Name() == "bin" || entry.Name() == "build" || entry.Name() == "vendor") {
				return filepath.SkipDir
			}
		} else if strings.HasSuffix(entry.Name(), ".go") {
			data, err := os.ReadFile(path)
			if err != nil {
				return err
			}
			marked = marked || strings.Contains(string(data), "fqdb:entity")
		}
		return nil
	})
	if err != nil {
		return err
	}
	if marked {
		tool := os.Getenv("FQDB_CODEGEN")
		if tool == "" {
			tool = filepath.Join(sdk, "Tools", "fqdb-codegen")
			if runtime.GOOS == "windows" {
				tool += ".exe"
			}
		}
		if err := execute(tool, []string{"generate", "--source", root, "--source-language", "go", "--output", filepath.Join(root, "Generated"), "--schema-lock", filepath.Join(root, "FengquDB.schema.lock.json")}, native); err != nil {
			return err
		}
	}
	args := os.Args[2:]
	if len(args) == 0 {
		args = []string{"."}
	}
	if os.Args[1] == "run" {
		return execute("go", append([]string{"run"}, args...), native)
	}
	for _, arg := range args {
		if arg == "-o" || strings.HasPrefix(arg, "-o=") {
			return fmt.Errorf("SDK build writes executables and runtime libraries to bin/; pass packages and other build flags")
		}
	}
	output := filepath.Join(root, "bin")
	if err := os.MkdirAll(output, 0755); err != nil {
		return err
	}
	if err := execute("go", append([]string{"build", "-o", output + string(os.PathSeparator)}, args...), native); err != nil {
		return err
	}
	filename := "libfengqudb.so.2"
	if runtime.GOOS == "windows" {
		filename = "fengqudb.dll"
	} else if runtime.GOOS == "darwin" {
		filename = "libfengqudb.2.dylib"
	}
	data, err := os.ReadFile(filepath.Join(native, filename))
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(output, filename), data, 0644)
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
