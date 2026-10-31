// Package project finds an ESP-IDF project, picks its board, then builds and runs it.
package project

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"

	"gopkg.in/yaml.v3"

	"github.com/edcs/preflash/internal/board"
)

// defaultBoard runs a project that hasn't named one.
const defaultBoard = "ESP32-S3-Touch-LCD-4B"

// Project is one checkout of an ESP-IDF project.
type Project struct {
	Dir string
	id  string // tells this checkout's folders from another copy's
}

// Find checks that path holds an ESP-IDF project. Symlinks are resolved so that
// two spellings of one folder share a build.
func Find(path string) (Project, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return Project{}, err
	}
	dir, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return Project{}, fmt.Errorf("%s is not an ESP-IDF project: %w", path, err)
	}
	cmake, err := os.ReadFile(filepath.Join(dir, "CMakeLists.txt"))
	if errors.Is(err, fs.ErrNotExist) {
		return Project{}, fmt.Errorf("%s is not an ESP-IDF project: it has no CMakeLists.txt", path)
	}
	if err != nil {
		return Project{}, err
	}
	if !strings.Contains(string(cmake), "project.cmake") {
		return Project{}, fmt.Errorf("%s is not an ESP-IDF project: its CMakeLists.txt doesn't include project.cmake", path)
	}
	sum := sha256.Sum256([]byte(dir))
	return Project{Dir: dir, id: filepath.Base(dir) + "-" + hex.EncodeToString(sum[:4])}, nil
}

// StateDir is where this checkout's saved state lives, outside the project.
func (p Project) StateDir() string { return filepath.Join(preflashHome(), "state", p.id) }

// BuildDir is where this checkout is built, outside the project.
func (p Project) BuildDir() string { return filepath.Join(preflashHome(), "build", p.id) }

func preflashHome() string {
	// Only fails with no HOME, and then a relative folder still works.
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".preflash")
}

// Board loads the board named on the command line, or else in .preflashrc, or
// else the default. A name is a stock board or a file. A file in .preflashrc is
// relative to the project and one from the command line to the working folder.
func (p Project) Board(flag string) (board.Board, error) {
	if flag != "" {
		return loadNamed(flag, flag)
	}
	named, err := p.rcBoard()
	if err != nil {
		return board.Board{}, err
	}
	if named == "" {
		return board.LoadStock(defaultBoard)
	}
	path := named
	if !filepath.IsAbs(path) {
		path = filepath.Join(p.Dir, path)
	}
	return loadNamed(named, path)
}

func (p Project) rcBoard() (string, error) {
	path := filepath.Join(p.Dir, ".preflashrc")
	data, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) {
		return "", nil
	}
	if err != nil {
		return "", err
	}
	var rc struct {
		Board string `yaml:"board"`
	}
	if err := yaml.Unmarshal(data, &rc); err != nil {
		return "", fmt.Errorf("%s: %w", path, err)
	}
	return rc.Board, nil
}

func loadNamed(name, path string) (board.Board, error) {
	b, err := board.LoadStock(name)
	if err == nil || !errors.Is(err, fs.ErrNotExist) {
		return b, err
	}
	b, err = board.Load(path)
	if errors.Is(err, fs.ErrNotExist) {
		return b, fmt.Errorf("no board %q: stock boards are %s", name, strings.Join(board.Stock(), ", "))
	}
	return b, err
}
