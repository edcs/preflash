package project

import (
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"

	"gopkg.in/yaml.v3"

	"github.com/edcs/preflash/internal/board"
	"github.com/edcs/preflash/internal/rc"
)

// boardFolders are where a project keeps board files, as far as anything looks.
var boardFolders = []string{".", "sim", "boards"}

// Choice is a board a project can run on.
type Choice struct {
	Board       board.Board
	File        string // relative to the project; empty for a stock board
	FromProject bool
	Err         error // why a project's board file won't load
}

// Boards lists what the project can run on: the file its config names, then any other
// board files in the usual folders, then the stock boards.
func (p Project) Boards() []Choice {
	var files []string
	for _, folder := range boardFolders {
		for _, pattern := range []string{"*.yaml", "*.yml"} {
			found, _ := filepath.Glob(filepath.Join(p.Dir, folder, pattern))
			files = append(files, found...)
		}
	}
	files = slices.DeleteFunc(files, func(path string) bool { return !looksLikeBoard(path) })
	slices.Sort(files)
	if config, _, err := rc.Load(p.Dir); err == nil {
		files = p.withNamedFirst(files, config.Board)
	}

	var choices []Choice
	for _, path := range files {
		relative, err := filepath.Rel(p.Dir, path)
		if err != nil {
			relative = path
		}
		b, err := board.Load(path)
		choices = append(choices, Choice{Board: b, File: relative, FromProject: true, Err: err})
	}
	for _, name := range board.Stock() {
		if b, err := board.LoadStock(name); err == nil {
			choices = append(choices, Choice{Board: b})
		}
	}
	return choices
}

// withNamedFirst moves the file the config names to the front. A stock board's name
// names no file.
func (p Project) withNamedFirst(files []string, named string) []string {
	if named == "" || slices.Contains(board.Stock(), named) {
		return files
	}
	path := named
	if !filepath.IsAbs(path) {
		path = filepath.Join(p.Dir, path)
	}
	path = filepath.Clean(path)
	return append([]string{path}, slices.DeleteFunc(files, func(file string) bool { return file == path })...)
}

// looksLikeBoard tells a board file, even a broken one, from the other YAML in a project.
func looksLikeBoard(path string) bool {
	data, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	var doc map[string]any
	if yaml.Unmarshal(data, &doc) != nil {
		return false
	}
	_, extends := doc["extends"]
	_, display := doc["display"]
	return extends || display
}

var lvglVersionPart = regexp.MustCompile(`(?m)^#define\s+LVGL_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)`)

// LVGLVersion is the version of the project's LVGL, or empty if it has none.
func (p Project) LVGLVersion() string {
	data, err := os.ReadFile(filepath.Join(p.Dir, "managed_components", "lvgl__lvgl", "lv_version.h"))
	if err != nil {
		return ""
	}
	parts := map[string]string{}
	for _, m := range lvglVersionPart.FindAllStringSubmatch(string(data), -1) {
		parts[m[1]] = m[2]
	}
	if len(parts) != 3 {
		return ""
	}
	return strings.Join([]string{parts["MAJOR"], parts["MINOR"], parts["PATCH"]}, ".")
}
