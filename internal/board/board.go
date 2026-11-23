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
	"slices"
	"strconv"
	"strings"

	"gopkg.in/yaml.v3"

	"github.com/edcs/preflash"
)

type Display struct {
	Controller string `yaml:"controller" json:"controller,omitempty"`
	Width      int    `yaml:"width" json:"width"`
	Height     int    `yaml:"height" json:"height"`
	// ByteOrder is how the panel takes RGB565 pixels from esp_lcd: le, the default, or be.
	ByteOrder string `yaml:"byte_order" json:"byte_order"`
}

type Chip struct {
	Chip    string `yaml:"chip" json:"chip"`
	Address int    `yaml:"address" json:"address"`
	// Bus is the i2c_port firmware reaches the chip on.
	Bus int `yaml:"bus" json:"bus"`
}

type Backlight struct {
	Pin       int  `yaml:"pin" json:"pin"`
	ActiveLow bool `yaml:"active_low" json:"active_low"`
	// Enable is a separate pin that must be driven for the backlight to light.
	Enable *int `yaml:"enable" json:"enable_pin,omitempty"`
	// EnableExpander is the same thing on a bit of the board's TCA9554. A board file
	// writes either as enable, like gpio43 or exio1.
	EnableExpander *int `yaml:"-" json:"enable_expander_bit,omitempty"`
}

type Board struct {
	Name       string    `yaml:"name" json:"name"`
	Display    Display   `yaml:"display" json:"display"`
	I2C        []Chip    `yaml:"i2c" json:"i2c"`
	Backlight  Backlight `yaml:"backlight" json:"backlight"`
	AudioCodec string    `yaml:"audio_codec" json:"audio_codec"`
	// PowerAmp is the expander bit that switches the speaker amplifier, if there is one.
	PowerAmp *int `yaml:"-" json:"power_amp_bit,omitempty"`
	// Buttons are the board's hardware buttons, in strip order.
	Buttons []Button `yaml:"-" json:"buttons,omitempty"`

	// Base is the board at the root of the extends chain, if there is one, and Changes
	// what the chain changed from it, in path order. Neither reaches the runtime.
	Base    string   `yaml:"-" json:"-"`
	Changes []Change `yaml:"-" json:"-"`

	// Source is the file this came from. A stock board has none.
	Source string `yaml:"-" json:"-"`
}

// Button is one hardware button, which does exactly one of what GPIO, Expander, AXP2101,
// Reset and Power say.
type Button struct {
	Name string
	// GPIO is a soft key on an ESP32 pin, and Expander the same on a TCA9554 input bit.
	GPIO      *int
	Expander  *int
	ActiveLow bool
	// AXP2101 is "pwrkey", the chip's PWRON key.
	AXP2101 string
	// Reset is "en", the chip's EN pin.
	Reset string
	// Power is a power key on a board without a power chip.
	Power *PowerKey
}

// PowerKey is a power key the firmware reads on Key, and keeps the battery on by driving
// the hold output, which is a pin (HoldGPIO) or an expander bit (HoldExpander).
type PowerKey struct {
	Key          int
	ActiveLow    bool
	HoldGPIO     *int
	HoldExpander *int
}

// MarshalJSON gives the runtime's form: a kind, and the pin it acts on if it has one.
func (b Button) MarshalJSON() ([]byte, error) {
	out := struct {
		Name       string `json:"name"`
		Kind       string `json:"kind"`
		Pin        *int   `json:"pin,omitempty"`
		ActiveLow  *bool  `json:"active_low,omitempty"`
		HoldPin    *int   `json:"hold_pin,omitempty"`
		HoldExpBit *int   `json:"hold_expander_bit,omitempty"`
	}{Name: b.Name}
	low := b.ActiveLow
	switch {
	case b.GPIO != nil:
		out.Kind, out.Pin, out.ActiveLow = "gpio", b.GPIO, &low
	case b.Expander != nil:
		out.Kind, out.Pin, out.ActiveLow = "expander", b.Expander, &low
	case b.AXP2101 != "":
		out.Kind = "axp2101"
	case b.Reset != "":
		out.Kind = "reset"
	case b.Power != nil:
		key, plow := b.Power.Key, b.Power.ActiveLow
		out.Kind, out.Pin, out.ActiveLow = "power", &key, &plow
		out.HoldPin, out.HoldExpBit = b.Power.HoldGPIO, b.Power.HoldExpander
	}
	return json.Marshal(out)
}

// Change is one value a board file sets over the board it extends.
type Change struct {
	Path  string // such as backlight.enable
	Value string
}

var (
	pinPattern      = regexp.MustCompile(`^gpio(\d+)$`)
	expanderPattern = regexp.MustCompile(`^exio([0-7])$`)
)

// maxI2CBus is the highest i2c_port an ESP32-S3 has.
const maxI2CBus = 1

// Load reads a board file from disk and applies its extends chain.
func Load(path string) (Board, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return Board{}, err
	}
	b, err := load(source{path: abs}, abs)
	b.Source = abs
	return b, err
}

// Reload reads a board from its file again, so a run sees any edit. A stock board can't
// have changed.
func Reload(b Board) (Board, error) {
	if b.Source == "" {
		return b, nil
	}
	return Load(b.Source)
}

// LoadStock reads the stock board with this name. It reports fs.ErrNotExist if
// there isn't one.
func LoadStock(name string) (Board, error) {
	src, ok := stockBoard(name)
	if !ok {
		return Board{}, fmt.Errorf("no stock board called %q: %w", name, fs.ErrNotExist)
	}
	return load(src, name)
}

// Stock returns the stock boards' names.
func Stock() []string {
	var names []string
	eachStock(func(s source, name string) bool {
		names = append(names, name)
		return true
	})
	return names
}

func load(src source, path string) (Board, error) {
	var history lineage
	merged, err := loadMap(src, map[string]bool{}, &history)
	if err != nil {
		return Board{}, err
	}
	// Before the pins, which leaves those a plain pin to convert.
	backlight, _ := merged["backlight"].(map[string]any)
	enableBit, err := takeExpanderBit(backlight, "enable", "backlight.enable", false)
	if err != nil {
		return Board{}, fmt.Errorf("%s: %w", path, err)
	}
	ampBit, err := takeExpanderBit(merged, "power_amp", "power_amp", true)
	if err != nil {
		return Board{}, fmt.Errorf("%s: %w", path, err)
	}
	buttons, err := takeButtons(merged)
	if err != nil {
		return Board{}, fmt.Errorf("%s: %w", path, err)
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
	b.Backlight.EnableExpander, b.PowerAmp, b.Buttons = enableBit, ampBit, buttons
	if err := check(&b); err != nil {
		return Board{}, fmt.Errorf("%s: %w", path, err)
	}
	if history.extended {
		b.Base, b.Changes = history.root, history.list()
	}
	return b, nil
}

// lineage collects what an extends chain did, from its root down.
type lineage struct {
	root     string
	extended bool
	changes  map[string]string
}

func (l *lineage) record(prefix string, changes map[string]any) {
	if l.changes == nil {
		l.changes = map[string]string{}
	}
	for key, value := range changes {
		if nested, ok := value.(map[string]any); ok {
			l.record(prefix+key+".", nested)
			continue
		}
		if _, list := value.([]any); list {
			l.changes[prefix+key] = "replaced"
			continue
		}
		l.changes[prefix+key] = fmt.Sprint(value)
	}
}

func (l *lineage) list() []Change {
	var out []Change
	for path, value := range l.changes {
		out = append(out, Change{Path: path, Value: value})
	}
	slices.SortFunc(out, func(a, b Change) int { return strings.Compare(a.Path, b.Path) })
	return out
}

// check stops a board whose values make no sense, and fills in the byte order.
func check(b *Board) error {
	switch b.Display.ByteOrder {
	case "":
		b.Display.ByteOrder = "le"
	case "le", "be":
	default:
		return fmt.Errorf("display.byte_order: %q is neither le nor be", b.Display.ByteOrder)
	}
	hasExpander, hasAXP := false, false
	for i, chip := range b.I2C {
		if chip.Bus < 0 || chip.Bus > maxI2CBus {
			return fmt.Errorf("i2c[%d].bus: %d is not a bus, it must be 0 or %d", i, chip.Bus, maxI2CBus)
		}
		hasExpander = hasExpander || chip.Chip == "tca9554"
		hasAXP = hasAXP || chip.Chip == "axp2101"
	}
	if b.Backlight.EnableExpander != nil && !hasExpander {
		return errors.New("backlight.enable: an exio bit needs a tca9554 under i2c")
	}
	seen := map[string]bool{}
	for _, btn := range b.Buttons {
		if seen[btn.Name] {
			return fmt.Errorf("buttons: two named %s", btn.Name)
		}
		seen[btn.Name] = true
		if btn.AXP2101 != "" && !hasAXP {
			return fmt.Errorf("buttons: %s uses axp2101 but the board has no axp2101", btn.Name)
		}
		usesExpander := btn.Expander != nil || (btn.Power != nil && btn.Power.HoldExpander != nil)
		if usesExpander && !hasExpander {
			return fmt.Errorf("buttons: %s uses a tca9554 but the board has none under i2c", btn.Name)
		}
	}
	return nil
}

// takeExpanderBit removes a value like exio3 from the map and returns its bit, which
// leaves the map free of what the board's YAML fields don't take. Only a key that must be
// an expander bit fails on anything else.
func takeExpanderBit(doc map[string]any, key, field string, only bool) (*int, error) {
	value, present := doc[key]
	if !present {
		return nil, nil
	}
	text, _ := value.(string)
	m := expanderPattern.FindStringSubmatch(text)
	if m == nil {
		if only || strings.HasPrefix(text, "exio") {
			return nil, fmt.Errorf("%s: %v is not an expander bit, write it like exio3", field, value)
		}
		return nil, nil
	}
	delete(doc, key)
	bit, err := strconv.Atoi(m[1])
	if err != nil {
		return nil, fmt.Errorf("%s: %q is out of range", field, text)
	}
	return &bit, nil
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

func loadMap(src source, seen map[string]bool, history *lineage) (map[string]any, error) {
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
	history.extended = history.extended || hasExt
	if !hasExt {
		if _, ok := doc["changes"]; ok {
			return nil, fmt.Errorf("%s: changes needs an extends", src.path)
		}
		if name, ok := doc["name"].(string); ok {
			history.root = name
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
	parent, err := loadMap(parentSrc, seen, history)
	if err != nil {
		return nil, err
	}
	if changes, ok := doc["changes"]; ok {
		cm, ok := changes.(map[string]any)
		if !ok {
			return nil, fmt.Errorf("%s: changes must be a map", src.path)
		}
		history.record("", cm)
		deepMerge(parent, cm)
	}
	if name, ok := doc["name"]; ok {
		parent["name"] = name
	}
	return parent, nil
}

// findParent looks for a stock board by name first, then a file next to the child.
func findParent(child source, ext string) (source, error) {
	if src, ok := stockBoard(ext); ok {
		return src, nil
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

func stockBoard(name string) (found source, ok bool) {
	eachStock(func(s source, n string) bool {
		if n == name {
			found, ok = s, true
		}
		return !ok
	})
	return found, ok
}

// eachStock visits the stock boards until visit returns false.
func eachStock(visit func(s source, name string) bool) {
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
		if yaml.Unmarshal(data, &head) != nil || head.Name == "" {
			continue
		}
		if !visit(source{path: p, embedded: true}, head.Name) {
			return
		}
	}
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

var expanderRefPattern = regexp.MustCompile(`^tca9554\.([0-7])$`)

// buttonNamePattern keeps a name to what the runtime can carry and the window can show: no
// "," or "@", which PREFLASH_HELD uses between entries, and no more than fits a 52 px button
// at 8 px a glyph with room either side.
var buttonNamePattern = regexp.MustCompile(`^[A-Za-z0-9_-]{1,5}$`)

// takeButtons removes the buttons list from the map and reads it.
func takeButtons(doc map[string]any) ([]Button, error) {
	raw, present := doc["buttons"]
	if !present {
		return nil, nil
	}
	delete(doc, "buttons")
	list, ok := raw.([]any)
	if !ok {
		return nil, errors.New("buttons: must be a list")
	}
	var out []Button
	for i, item := range list {
		entry, ok := item.(map[string]any)
		if !ok {
			return nil, fmt.Errorf("buttons[%d]: must be a map with a name", i)
		}
		btn, err := readButton(i, entry)
		if err != nil {
			return nil, err
		}
		out = append(out, btn)
	}
	return out, nil
}

func readButton(i int, entry map[string]any) (Button, error) {
	name, _ := entry["name"].(string)
	if name == "" {
		return Button{}, fmt.Errorf("buttons[%d].name: is missing", i)
	}
	if !buttonNamePattern.MatchString(name) {
		return Button{}, fmt.Errorf("buttons: %q must be 1-5 letters, digits, - or _", name)
	}
	btn := Button{Name: name}
	kinds := 0
	for key := range entry {
		switch key {
		case "name", "active_low":
			continue
		case "gpio", "expander", "axp2101", "reset", "power":
			kinds++
		default:
			return Button{}, fmt.Errorf("buttons: %s has an unknown key %q", name, key)
		}
	}
	if kinds != 1 {
		return Button{}, fmt.Errorf("buttons: %s needs exactly one of gpio, expander, axp2101, reset or power", name)
	}
	needLow := func() error {
		low, ok := entry["active_low"].(bool)
		if !ok {
			return fmt.Errorf("buttons: %s needs active_low: true or false", name)
		}
		btn.ActiveLow = low
		return nil
	}
	var err error
	switch {
	case entry["gpio"] != nil:
		var pin int
		if pin, err = parsePin(name, "gpio", entry["gpio"]); err == nil {
			btn.GPIO, err = &pin, needLow()
		}
	case entry["expander"] != nil:
		var bit int
		if bit, err = parseExpanderRef(name, "expander", entry["expander"]); err == nil {
			btn.Expander, err = &bit, needLow()
		}
	case entry["axp2101"] != nil:
		if entry["axp2101"] != "pwrkey" {
			err = fmt.Errorf("buttons: %s: axp2101 must be pwrkey", name)
		}
		btn.AXP2101 = "pwrkey"
	case entry["reset"] != nil:
		if entry["reset"] != "en" {
			err = fmt.Errorf("buttons: %s: reset must be en", name)
		}
		btn.Reset = "en"
	default:
		btn.Power, err = readPowerKey(name, entry["power"])
	}
	return btn, err
}

func readPowerKey(name string, raw any) (*PowerKey, error) {
	m, ok := raw.(map[string]any)
	if !ok {
		return nil, fmt.Errorf("buttons: %s: power must be a map with key, active_low and hold", name)
	}
	for key := range m {
		if key != "key" && key != "active_low" && key != "hold" {
			return nil, fmt.Errorf("buttons: %s: power has an unknown key %q", name, key)
		}
	}
	key, err := parsePin(name, "power.key", m["key"])
	if err != nil {
		return nil, err
	}
	low, ok := m["active_low"].(bool)
	if !ok {
		return nil, fmt.Errorf("buttons: %s needs power.active_low: true or false", name)
	}
	p := &PowerKey{Key: key, ActiveLow: low}
	hold, _ := m["hold"].(string)
	if g := pinPattern.FindStringSubmatch(hold); g != nil {
		pin, _ := strconv.Atoi(g[1])
		p.HoldGPIO = &pin
	} else if e := expanderRefPattern.FindStringSubmatch(hold); e != nil {
		bit, _ := strconv.Atoi(e[1])
		p.HoldExpander = &bit
	} else {
		return nil, fmt.Errorf("buttons: %s: power.hold %v is not a pin, write it like gpio5 or tca9554.6", name, m["hold"])
	}
	return p, nil
}

func parsePin(name, field string, v any) (int, error) {
	s, _ := v.(string)
	m := pinPattern.FindStringSubmatch(s)
	if m == nil {
		return 0, fmt.Errorf("buttons: %s: %s %v is not a pin, write it like gpio4", name, field, v)
	}
	n, err := strconv.Atoi(m[1])
	if err != nil || n > 48 {
		return 0, fmt.Errorf("buttons: %s: %s %q is out of range", name, field, s)
	}
	return n, nil
}

func parseExpanderRef(name, field string, v any) (int, error) {
	s, _ := v.(string)
	m := expanderRefPattern.FindStringSubmatch(s)
	if m == nil {
		return 0, fmt.Errorf("buttons: %s: %s %v is not an expander input, write it like tca9554.3", name, field, v)
	}
	return int(m[1][0] - '0'), nil
}
