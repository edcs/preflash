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
	"slices"
	"strings"
	"time"

	"github.com/edcs/preflash/internal/board"
	"github.com/edcs/preflash/internal/crash"
	"github.com/edcs/preflash/internal/rc"
)

// defaultBoard runs a project that hasn't named one.
const defaultBoard = "ESP32-S3-Touch-LCD-4B"

// socketPathLimit is the size of a Unix socket's path on macOS, with its final NUL.
const socketPathLimit = 104

// Project is one checkout of an ESP-IDF project.
type Project struct {
	Dir string
	id  string // tells this checkout's folders from another copy's
	// sanitize names the sanitizer this build uses, if any.
	sanitize string
	// sdkconfig is the file this build's options come from, if it isn't the usual one.
	sdkconfig string
}

// Sanitizers are the kinds of build a project can have besides its plain one.
var Sanitizers = []string{"address", "thread"}

// CheckSanitizer fails on anything but a sanitizer's name, or none.
func CheckSanitizer(kind string) error {
	if kind != "" && !slices.Contains(Sanitizers, kind) {
		return fmt.Errorf("no sanitizer %q: use address or thread", kind)
	}
	return nil
}

// Sanitized is the same checkout built with a sanitizer. Each has a build folder of its own,
// because every object differs, while saved state and the socket stay the checkout's.
func (p Project) Sanitized(kind string) Project {
	p.sanitize = kind
	return p
}

// forBoard is the same checkout with the sdkconfig .preflashrc names for the board, for a
// project that builds differently per board. It keeps one build folder, because a build
// follows whichever sdkconfig it's given.
func (p Project) forBoard(b board.Board) (Project, error) {
	config, _, err := rc.Load(p.Dir)
	if err != nil {
		return p, err
	}
	p.sdkconfig = ""
	file, ok := config.Sdkconfigs[b.Name]
	if !ok {
		return p, nil
	}
	path := filepath.Join(p.Dir, file)
	if _, err := os.Stat(path); err != nil {
		return p, fmt.Errorf("%s names an sdkconfig for %s: %w", rc.FileName, b.Name, err)
	}
	p.sdkconfig = path
	return p, nil
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
func (p Project) StateDir() string { return filepath.Join(Home(), "state", p.id) }

// BuildDir is where this checkout is built, outside the project.
func (p Project) BuildDir() string {
	if p.sanitize != "" {
		return filepath.Join(Home(), "build", p.id+"-"+p.sanitize)
	}
	return filepath.Join(Home(), "build", p.id)
}

// CrashPath is where a crash of this build leaves its report.
func (p Project) CrashPath() string { return filepath.Join(p.BuildDir(), "crash.json") }

// LastCrash reads the newest report of any of this checkout's builds.
func (p Project) LastCrash() (*crash.Report, error) {
	newest := ""
	var when time.Time
	for _, kind := range append([]string{""}, Sanitizers...) {
		path := p.Sanitized(kind).CrashPath()
		if info, err := os.Stat(path); err == nil && info.ModTime().After(when) {
			newest, when = path, info.ModTime()
		}
	}
	if newest == "" {
		return nil, fmt.Errorf("no crash report for %s: %w", filepath.Base(p.Dir), fs.ErrNotExist)
	}
	return crash.Read(newest)
}

// SocketPath is where this checkout's running app listens. It's named by a short hash
// and sits under the cache folder, or the temporary folder if a long HOME leaves no room
// there. Nothing checks that it fits: checkSocketPath does that.
func (p Project) SocketPath() string {
	// Only fails with no HOME, and then a relative folder still works.
	home, _ := os.UserHomeDir()
	sum := sha256.Sum256([]byte(p.Dir))
	name := hex.EncodeToString(sum[:6]) + ".sock"
	path := filepath.Join(home, "Library", "Caches", "preflash", name)
	if len(path) < socketPathLimit {
		return path
	}
	return filepath.Join(os.TempDir(), name)
}

// PortPath is the link to this checkout's virtual serial port, which stays the same from
// one run to the next while the device behind it changes.
func (p Project) PortPath() string {
	return strings.TrimSuffix(p.SocketPath(), ".sock") + ".tty"
}

// checkSocketPath fails if neither of SocketPath's folders leaves room for the socket.
func (p Project) checkSocketPath() error {
	if path := p.SocketPath(); len(path) >= socketPathLimit {
		return fmt.Errorf("the socket's path is too long for macOS: %s: use a shorter HOME or TMPDIR", path)
	}
	return nil
}

// Home is the folder that holds what Preflash keeps between runs.
func Home() string {
	// Only fails with no HOME, and then a relative folder still works.
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".preflash")
}

// serialLink gives the full path of the link .preflashrc asks for, or nothing if it doesn't.
// It's relative to the project, as a board file is, and ~/ means the user's home.
func (p Project) serialLink() (string, error) {
	config, _, err := rc.Load(p.Dir)
	if err != nil || config.SerialLink == "" {
		return "", err
	}
	link := config.SerialLink
	if rest, ok := strings.CutPrefix(link, "~/"); ok {
		home, err := os.UserHomeDir()
		if err != nil {
			return "", err
		}
		return filepath.Join(home, rest), nil
	}
	if !filepath.IsAbs(link) {
		link = filepath.Join(p.Dir, link)
	}
	return link, nil
}

// Board loads the board named on the command line, or else in .preflashrc, or
// else the default. A name is a stock board or a file. A file in .preflashrc is
// relative to the project and one from the command line to the working folder.
func (p Project) Board(flag string) (board.Board, error) {
	if flag != "" {
		return loadNamed(flag, flag)
	}
	config, _, err := rc.Load(p.Dir)
	if err != nil {
		return board.Board{}, err
	}
	named := config.Board
	if named == "" {
		return board.LoadStock(defaultBoard)
	}
	path := named
	if !filepath.IsAbs(path) {
		path = filepath.Join(p.Dir, path)
	}
	return loadNamed(named, path)
}

// LoadBoard reads a stock board by name, or else a board file.
func LoadBoard(name string) (board.Board, error) { return loadNamed(name, name) }

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
