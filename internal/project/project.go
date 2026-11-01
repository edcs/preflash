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
		return Project{}, fmt.Errorf("%s is not an ESP-IDF project: %w", abs, err)
	}
	cmake, err := os.ReadFile(filepath.Join(dir, "CMakeLists.txt"))
	if errors.Is(err, fs.ErrNotExist) {
		return Project{}, fmt.Errorf("%s is not an ESP-IDF project: it has no CMakeLists.txt", abs)
	}
	if err != nil {
		return Project{}, err
	}
	if !strings.Contains(withoutComments(string(cmake)), "project.cmake") {
		return Project{}, fmt.Errorf("%s is not an ESP-IDF project: its CMakeLists.txt doesn't include project.cmake", abs)
	}
	sum := sha256.Sum256([]byte(dir))
	return Project{Dir: dir, id: filepath.Base(dir) + "-" + hex.EncodeToString(sum[:4])}, nil
}

// withoutComments drops CMake's line comments and bracket comments, so that a
// commented-out include doesn't count. A # inside a quoted or bracket argument
// is text, so those arguments are copied through whole.
func withoutComments(src string) string {
	var out strings.Builder
	for i := 0; i < len(src); {
		switch src[i] {
		case '"':
			end := quotedEnd(src[i:])
			out.WriteString(src[i : i+end])
			i += end
		case '[':
			end := bracketEnd(src[i:])
			if end == 0 {
				end = 1
			}
			out.WriteString(src[i : i+end])
			i += end
		case '#':
			if end := bracketEnd(src[i+1:]); end > 0 {
				i += 1 + end
				continue
			}
			end := strings.IndexByte(src[i:], '\n')
			if end < 0 {
				end = len(src) - i
			}
			i += end
		default:
			out.WriteByte(src[i])
			i++
		}
	}
	return out.String()
}

// quotedEnd gives the length of the quoted argument that opens s, or all of s
// when it never closes.
func quotedEnd(s string) int {
	for i := 1; i < len(s); i++ {
		switch s[i] {
		case '\\':
			i++
		case '"':
			return i + 1
		}
	}
	return len(s)
}

// bracketEnd gives the length of the bracket that opens s, such as [==[ ... ]==],
// all of s when it never closes, or 0 when s doesn't open one.
func bracketEnd(s string) int {
	if !strings.HasPrefix(s, "[") {
		return 0
	}
	level := len(s[1:]) - len(strings.TrimLeft(s[1:], "="))
	if !strings.HasPrefix(s[1+level:], "[") {
		return 0
	}
	closing := "]" + strings.Repeat("=", level) + "]"
	body := 2 + level
	at := strings.Index(s[body:], closing)
	if at < 0 {
		return len(s)
	}
	return body + at + len(closing)
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
