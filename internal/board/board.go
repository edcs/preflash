// Package board loads board files and resolves them into the JSON the runtime reads.
package board

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strconv"

	"gopkg.in/yaml.v3"

	"github.com/edcs/preflash"
)

type Display struct {
	Controller string `yaml:"controller" json:"controller,omitempty"`
	Width      int    `yaml:"width" json:"width"`
	Height     int    `yaml:"height" json:"height"`
}

type Chip struct {
	Chip    string `yaml:"chip" json:"chip"`
	Address int    `yaml:"address" json:"address"`
}

type Backlight struct {
	Pin       int  `yaml:"pin" json:"pin"`
	ActiveLow bool `yaml:"active_low" json:"active_low"`
	// Enable is a separate pin that must be driven for the backlight to light.
	Enable *int `yaml:"enable" json:"enable_pin,omitempty"`
}

type Board struct {
	Name       string    `yaml:"name" json:"name"`
	Display    Display   `yaml:"display" json:"display"`
	I2C        []Chip    `yaml:"i2c" json:"i2c"`
	Backlight  Backlight `yaml:"backlight" json:"backlight"`
	AudioCodec string    `yaml:"audio_codec" json:"audio_codec"`
}

var pinPattern = regexp.MustCompile(`^gpio(\d+)$`)

// Load reads a board file from disk and applies its extends chain.
func Load(path string) (Board, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return Board{}, err
	}
	merged, err := loadMap(source{path: abs}, map[string]bool{})
	if err != nil {
		return Board{}, err
	}
	if err := convertPins(merged); err != nil {
		return Board{}, fmt.Errorf("%s: %w", path, err)
	}
	if err := requireBacklightKeys(merged); err != nil {
		return Board{}, fmt.Errorf("%s: %w", path, err)
	}
	out, err := yaml.Marshal(merged)
	if err != nil {
		return Board{}, err
	}
	var b Board
	dec := yaml.NewDecoder(bytes.NewReader(out))
	dec.KnownFields(true)
	if err := dec.Decode(&b); err != nil {
		return Board{}, fmt.Errorf("%s: %w", path, err)
	}
	if b.Display.Width <= 0 || b.Display.Height <= 0 {
		return Board{}, fmt.Errorf("%s: display.width and display.height must be positive", path)
	}
	return b, nil
}

// requireBacklightKeys stops a board that leaves out the backlight pin or
// polarity, which would otherwise decode to pin 0 and active-high.
func requireBacklightKeys(merged map[string]any) error {
	backlight, _ := merged["backlight"].(map[string]any)
	for _, key := range []string{"pin", "active_low"} {
		if _, ok := backlight[key]; !ok {
			return fmt.Errorf("backlight.%s is missing", key)
		}
	}
	return nil
}

// Resolve returns the JSON that the runtime's parse_board reads.
func Resolve(b Board) ([]byte, error) {
	return json.Marshal(b)
}

// source is either a file on disk or a stock board embedded in the binary.
type source struct {
	path     string // disk path, or the embedded path when embedded is set
	embedded bool
}

func (s source) id() string {
	if s.embedded {
		return "stock:" + s.path
	}
	return s.path
}

func (s source) read() ([]byte, error) {
	if s.embedded {
		return fs.ReadFile(preflash.Boards, s.path)
	}
	return os.ReadFile(s.path)
}

func loadMap(src source, seen map[string]bool) (map[string]any, error) {
	if seen[src.id()] {
		return nil, fmt.Errorf("board %s extends itself through a loop", src.path)
	}
	seen[src.id()] = true

	data, err := src.read()
	if err != nil {
		return nil, err
	}
	var doc map[string]any
	if err := yaml.Unmarshal(data, &doc); err != nil {
		return nil, fmt.Errorf("%s: %w", src.path, err)
	}
	if doc == nil {
		return nil, fmt.Errorf("%s: empty board file", src.path)
	}
	ext, hasExt := doc["extends"]
	if !hasExt {
		if _, ok := doc["changes"]; ok {
			return nil, fmt.Errorf("%s: changes needs an extends", src.path)
		}
		return doc, nil
	}
	extName, ok := ext.(string)
	if !ok || extName == "" {
		return nil, fmt.Errorf("%s: extends must be a board name or a path", src.path)
	}
	for k := range doc {
		if k != "name" && k != "extends" && k != "changes" {
			return nil, fmt.Errorf("%s: with extends, put %q under changes", src.path, k)
		}
	}
	parentSrc, err := findParent(src, extName)
	if err != nil {
		return nil, err
	}
	parent, err := loadMap(parentSrc, seen)
	if err != nil {
		return nil, err
	}
	if changes, ok := doc["changes"]; ok {
		cm, ok := changes.(map[string]any)
		if !ok {
			return nil, fmt.Errorf("%s: changes must be a map", src.path)
		}
		deepMerge(parent, cm)
	}
	if name, ok := doc["name"]; ok {
		parent["name"] = name
	}
	return parent, nil
}

// findParent looks for a stock board by name first, then a file next to the child.
func findParent(child source, ext string) (source, error) {
	entries, _ := fs.ReadDir(preflash.Boards, "boards")
	for _, e := range entries {
		p := "boards/" + e.Name()
		data, err := fs.ReadFile(preflash.Boards, p)
		if err != nil {
			continue
		}
		var head struct {
			Name string `yaml:"name"`
		}
		if yaml.Unmarshal(data, &head) == nil && head.Name == ext {
			return source{path: p, embedded: true}, nil
		}
	}
	if !child.embedded {
		p := ext
		if !filepath.IsAbs(p) {
			p = filepath.Join(filepath.Dir(child.path), p)
		}
		if _, err := os.Stat(p); err == nil {
			return source{path: p}, nil
		} else if !errors.Is(err, fs.ErrNotExist) {
			return source{}, err
		}
	}
	return source{}, fmt.Errorf("%s: extends %q, which is neither a stock board name nor a file", child.path, ext)
}

func deepMerge(dst, src map[string]any) {
	for k, v := range src {
		sm, sIsMap := v.(map[string]any)
		dm, dIsMap := dst[k].(map[string]any)
		if sIsMap && dIsMap {
			deepMerge(dm, sm)
			continue
		}
		dst[k] = v
	}
}

// convertPins turns "gpioN" into N for the pin fields.
func convertPins(doc map[string]any) error {
	bl, ok := doc["backlight"].(map[string]any)
	if !ok {
		return nil
	}
	for _, key := range []string{"pin", "enable"} {
		s, isString := bl[key].(string)
		if !isString {
			continue
		}
		m := pinPattern.FindStringSubmatch(s)
		if m == nil {
			return fmt.Errorf("backlight.%s: %q is not a pin, write it like gpio4", key, s)
		}
		n, err := strconv.Atoi(m[1])
		if err != nil {
			return fmt.Errorf("backlight.%s: %q is out of range", key, s)
		}
		bl[key] = n
	}
	return nil
}
