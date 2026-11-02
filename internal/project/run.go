package project

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/edcs/preflash/internal/board"
	"github.com/edcs/preflash/internal/client"
)

// buildGrace is how long a cancelled build's tools get to stop.
const buildGrace = 3 * time.Second

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
		"PREFLASH_SOCKET=" + p.SocketPath(),
	}
	if headless {
		set = append(set, "PREFLASH_HEADLESS=1")
	}
	env := make([]string, 0, len(base)+len(set))
	for _, entry := range base {
		name, _, _ := strings.Cut(entry, "=")
		switch name {
		case "IDF_PATH", "PREFLASH_BOARD", "PREFLASH_STATE", "PREFLASH_PROJECT", "PREFLASH_SOCKET", "PREFLASH_HEADLESS":
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
	if err := p.checkSocketPath(); err != nil {
		return err
	}
	if err := p.CheckNoApp(); err != nil {
		return err
	}
	resolved, err := board.Resolve(b)
	if err != nil {
		return err
	}
	if err := p.makeDirs(); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(p.BuildDir(), "board.json"), resolved, 0o644); err != nil {
		return err
	}

	env := p.environment(os.Environ(), root, headless)
	fmt.Fprintf(os.Stderr, "preflash: building %s for %s\n", filepath.Base(p.Dir), b.Name)
	ctx, cancel := signal.NotifyContext(context.Background(), stopSignals()...)
	defer cancel()
	if err := p.buildQuietly(ctx, env, os.Stderr); err != nil {
		return err
	}
	cancel()

	app := filepath.Join(p.BuildDir(), "app")
	if err := syscall.Exec(app, []string{app}, env); err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return fmt.Errorf("the build made no app at %s", app)
		}
		return fmt.Errorf("starting %s: %w", app, err)
	}
	return nil
}

// stopSignals are what end a build, leaving out any already ignored, as under nohup. Go's
// signal package would otherwise replace that ignore with a handler.
func stopSignals() []os.Signal {
	stops := []os.Signal{os.Interrupt}
	for _, stop := range []os.Signal{syscall.SIGTERM, syscall.SIGHUP} {
		if !signal.Ignored(stop) {
			stops = append(stops, stop)
		}
	}
	return stops
}

// buildQuietly keeps the build's output back unless it fails, so the terminal shows only
// the firmware's console. A build someone stopped has failed at nothing.
func (p Project) buildQuietly(ctx context.Context, env []string, failures io.Writer) error {
	var output bytes.Buffer
	err := p.Build(ctx, env, &output)
	if ctx.Err() != nil {
		return errors.New("the build was stopped")
	}
	if err != nil {
		failures.Write(output.Bytes())
	}
	return err
}

// CheckNoApp fails if an app answers on this checkout's socket. Building would change
// what's under it, so this comes first. The app makes the same check when it starts, which
// settles any race.
func (p Project) CheckNoApp() error {
	c, err := client.Dial(p.SocketPath())
	if errors.Is(err, client.ErrNotRunning) {
		return nil
	}
	if err != nil {
		return err
	}
	defer c.Close()
	return fmt.Errorf("%s (pid %d) already holds the socket at %s", c.Hello.Project, c.Hello.PID, p.SocketPath())
}

func (p Project) makeDirs() error {
	if err := os.MkdirAll(p.BuildDir(), 0o755); err != nil {
		return err
	}
	// Saved state can hold Wi-Fi credentials, so only the user may read it.
	if err := os.MkdirAll(p.StateDir(), 0o700); err != nil {
		return err
	}
	if err := os.Chmod(p.StateDir(), 0o700); err != nil {
		return err
	}
	return os.MkdirAll(filepath.Dir(p.SocketPath()), 0o755)
}

// Build compiles the project, writing what the tools print to output. Cancelling ctx stops
// them. They get a group of their own for that, so a compiler goes too, and a chance to
// clean up, so a half-written object can't pass for a finished one in the next build.
func (p Project) Build(ctx context.Context, env []string, output io.Writer) error {
	jobs := strconv.Itoa(runtime.NumCPU())
	steps := [][]string{
		{"-S", p.Dir, "-B", p.BuildDir()},
		{"--build", p.BuildDir(), "--parallel", jobs},
	}
	for _, args := range steps {
		cmd := exec.CommandContext(ctx, "cmake", args...)
		cmd.Env = env
		cmd.Stdout = output
		cmd.Stderr = output
		cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
		cmd.Cancel = func() error { return syscall.Kill(-cmd.Process.Pid, syscall.SIGTERM) }
		cmd.WaitDelay = buildGrace
		if err := cmd.Run(); err != nil {
			return fmt.Errorf("the build failed: cmake %s: %w", strings.Join(args, " "), err)
		}
	}
	return nil
}

// Environment gets the folders ready and gives the variables a build and its app run with.
func (p Project) Environment(headless bool) ([]string, error) {
	root, err := Root()
	if err != nil {
		return nil, err
	}
	if err := p.checkSocketPath(); err != nil {
		return nil, err
	}
	if err := p.makeDirs(); err != nil {
		return nil, err
	}
	return p.environment(os.Environ(), root, headless), nil
}

// LogPath is where a supervised app's stdout and stderr go.
func (p Project) LogPath() string { return filepath.Join(p.BuildDir(), "app.log") }

// Install sets up a copy of the built app to run on this board, giving its path and
// environment. A supervised app keeps running through the next build, and a restart runs
// its executable and board file again, so neither can be the ones a build or a change of
// board writes to.
func (p Project) Install(b board.Board, env []string) (string, []string, error) {
	resolved, err := board.Resolve(b)
	if err != nil {
		return "", nil, err
	}
	boardFile := filepath.Join(p.BuildDir(), "board.running.json")
	if err := os.WriteFile(boardFile, resolved, 0o644); err != nil {
		return "", nil, err
	}
	app := filepath.Join(p.BuildDir(), "app")
	running := filepath.Join(p.BuildDir(), "app.running")
	if err := copyExecutable(app, running); err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return "", nil, fmt.Errorf("the build made no app at %s", app)
		}
		return "", nil, err
	}
	runEnv := make([]string, 0, len(env))
	for _, entry := range env {
		if !strings.HasPrefix(entry, "PREFLASH_BOARD=") {
			runEnv = append(runEnv, entry)
		}
	}
	return running, append(runEnv, "PREFLASH_BOARD="+boardFile), nil
}

// copyExecutable copies through a temporary name, so that to is never half a program.
func copyExecutable(from, to string) error {
	in, err := os.Open(from)
	if err != nil {
		return err
	}
	defer in.Close()
	next := to + ".next"
	out, err := os.OpenFile(next, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o755)
	if err != nil {
		return err
	}
	_, err = io.Copy(out, in)
	if closed := out.Close(); err == nil {
		err = closed
	}
	if err != nil {
		os.Remove(next)
		return err
	}
	return os.Rename(next, to)
}
