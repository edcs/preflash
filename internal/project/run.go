package project

import (
	"bytes"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"

	"github.com/edcs/preflash/internal/board"
)

// Root finds the Preflash checkout that holds runtime/. The runtime isn't embedded
// yet, so a binary works only beside its source unless PREFLASH_ROOT says otherwise.
func Root() (string, error) {
	root := os.Getenv("PREFLASH_ROOT")
	if root == "" {
		_, file, _, ok := runtime.Caller(0)
		if !ok {
			return "", errors.New("can't tell where the runtime is: set PREFLASH_ROOT to the Preflash folder")
		}
		root = filepath.Join(filepath.Dir(file), "..", "..")
	}
	root, err := filepath.Abs(root)
	if err != nil {
		return "", err
	}
	if _, err := os.Stat(filepath.Join(root, "runtime", "idf", "tools", "cmake", "project.cmake")); err != nil {
		return "", fmt.Errorf("no runtime in %s: set PREFLASH_ROOT to the Preflash folder", root)
	}
	return root, nil
}

// environment is the base environment with the app's variables replacing any
// already there: a real ESP-IDF's IDF_PATH must not reach the build.
func (p Project) environment(base []string, root string, headless bool) []string {
	set := []string{
		"IDF_PATH=" + filepath.Join(root, "runtime", "idf"),
		"PREFLASH_BOARD=" + filepath.Join(p.BuildDir(), "board.json"),
		"PREFLASH_STATE=" + p.StateDir(),
		"PREFLASH_PROJECT=" + filepath.Base(p.Dir),
	}
	if headless {
		set = append(set, "PREFLASH_HEADLESS=1")
	}
	env := make([]string, 0, len(base)+len(set))
	for _, entry := range base {
		name, _, _ := strings.Cut(entry, "=")
		switch name {
		case "IDF_PATH", "PREFLASH_BOARD", "PREFLASH_STATE", "PREFLASH_PROJECT", "PREFLASH_HEADLESS":
		default:
			env = append(env, entry)
		}
	}
	return append(env, set...)
}

// Run builds the project for the board, then replaces this process with the app,
// so that stdin and signals go straight to the firmware. It returns only on failure.
func (p Project) Run(b board.Board, headless bool) error {
	root, err := Root()
	if err != nil {
		return err
	}
	resolved, err := board.Resolve(b)
	if err != nil {
		return err
	}
	for _, dir := range []string{p.BuildDir(), p.StateDir()} {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}
	if err := os.WriteFile(filepath.Join(p.BuildDir(), "board.json"), resolved, 0o644); err != nil {
		return err
	}

	env := p.environment(os.Environ(), root, headless)
	fmt.Fprintf(os.Stderr, "preflash: building %s for %s\n", filepath.Base(p.Dir), b.Name)
	if err := p.build(env); err != nil {
		return err
	}

	app := filepath.Join(p.BuildDir(), "app")
	if err := syscall.Exec(app, []string{app}, env); err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return fmt.Errorf("the build made no app at %s", app)
		}
		return fmt.Errorf("starting %s: %w", app, err)
	}
	return nil
}

func (p Project) build(env []string) error {
	jobs := strconv.Itoa(runtime.NumCPU())
	steps := [][]string{
		{"-S", p.Dir, "-B", p.BuildDir()},
		{"--build", p.BuildDir(), "--parallel", jobs},
	}
	for _, args := range steps {
		// The build's output stays quiet unless it fails, so the terminal
		// shows only the firmware's console.
		var output bytes.Buffer
		cmd := exec.Command("cmake", args...)
		cmd.Env = env
		cmd.Stdout = &output
		cmd.Stderr = &output
		if err := cmd.Run(); err != nil {
			os.Stderr.Write(output.Bytes())
			return fmt.Errorf("the build failed: cmake %s: %w", strings.Join(args, " "), err)
		}
	}
	return nil
}
