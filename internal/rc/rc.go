// Package rc reads a project's .preflashrc: its board, console settings, shortcuts and
// status fields. Anything it doesn't know is an error naming the line, so a typo can't
// quietly do nothing.
package rc

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"gopkg.in/yaml.v3"
)

// FileName is the config's name inside a project.
const FileName = ".preflashrc"

// today fills in as the local date, so no shortcut can ask for it.
const today = "today"

// Config is one project's settings. The zero value is a project with none.
type Config struct {
	Board     string
	Prompt    string
	Shortcuts []Shortcut
	Status    []Field
}

// Shortcut sends lines to the console, once any values it asks for are in.
type Shortcut struct {
	Label string
	Send  []string
	Ask   []Ask
}

// Ask is one value a shortcut asks for before it sends.
type Ask struct {
	Name    string
	Default string
}

// Field is a toolbar value. With no Pattern it's one Preflash knows by itself.
type Field struct {
	Name    string
	Pattern *regexp.Regexp
}

// builtIn is what a status field can show with no pattern.
var builtIn = map[string]bool{"backlight": true, "rotation": true, "wifi": true}

var (
	placeholder = regexp.MustCompile(`\{([A-Za-z_][A-Za-z0-9_]*)\}`)
	valueName   = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)
)

// Load reads the config in a project's folder, saying whether there was one.
func Load(dir string) (Config, bool, error) {
	path := filepath.Join(dir, FileName)
	data, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) {
		return Config{}, false, nil
	}
	if err != nil {
		return Config{}, false, err
	}
	c, err := Parse(path, data)
	return c, err == nil, err
}

// Parse reads a config. The name opens every error, so it should say where data came from.
func Parse(name string, data []byte) (Config, error) {
	var doc yaml.Node
	if err := yaml.Unmarshal(data, &doc); err != nil {
		return Config{}, fmt.Errorf("%s: %w", name, err)
	}
	if doc.Kind == 0 {
		return Config{}, nil
	}
	p := parser{name: name}
	root := target(doc.Content[0])
	if root.Kind == yaml.ScalarNode && root.Tag == "!!null" {
		return Config{}, nil
	}
	return p.config(root)
}

// parser keeps the file's name for its errors.
type parser struct{ name string }

func (p parser) fail(n *yaml.Node, format string, args ...any) error {
	return fmt.Errorf("%s:%d: %s", p.name, n.Line, fmt.Sprintf(format, args...))
}

// target follows an alias to what it names.
func target(n *yaml.Node) *yaml.Node {
	for n.Kind == yaml.AliasNode {
		n = n.Alias
	}
	return n
}

func isNull(n *yaml.Node) bool { return n.Kind == yaml.ScalarNode && n.Tag == "!!null" }

// entries calls visit for each key of a map, stopping a key that comes twice.
func (p parser) entries(n *yaml.Node, visit func(key string, keyNode, value *yaml.Node) error) error {
	seen := map[string]bool{}
	for i := 0; i+1 < len(n.Content); i += 2 {
		key := target(n.Content[i])
		if seen[key.Value] {
			return p.fail(key, "%s is set twice", key.Value)
		}
		seen[key.Value] = true
		if err := visit(key.Value, key, target(n.Content[i+1])); err != nil {
			return err
		}
	}
	return nil
}

func (p parser) config(root *yaml.Node) (Config, error) {
	if root.Kind != yaml.MappingNode {
		return Config{}, p.fail(root, "%s must be a map", FileName)
	}
	var c Config
	err := p.entries(root, func(key string, keyNode, value *yaml.Node) error {
		var err error
		switch key {
		case "board":
			c.Board, err = p.text(value, "board")
		case "console":
			c.Prompt, err = p.console(value)
		case "shortcuts":
			c.Shortcuts, err = p.shortcuts(value)
		case "status":
			c.Status, err = p.status(value)
		default:
			err = p.fail(keyNode, "unknown key %q", key)
		}
		return err
	})
	return c, err
}

// text reads a key that holds one value. A null reads as nothing set.
func (p parser) text(n *yaml.Node, what string) (string, error) {
	if isNull(n) {
		return "", nil
	}
	if n.Kind != yaml.ScalarNode {
		return "", p.fail(n, "%s must be text", what)
	}
	return n.Value, nil
}

func (p parser) console(n *yaml.Node) (string, error) {
	if n.Kind != yaml.MappingNode {
		return "", p.fail(n, "console must be a map")
	}
	var prompt string
	err := p.entries(n, func(key string, keyNode, value *yaml.Node) error {
		if key != "prompt" {
			return p.fail(keyNode, "unknown key %q in console", key)
		}
		var err error
		prompt, err = p.text(value, "console.prompt")
		return err
	})
	return prompt, err
}

func (p parser) shortcuts(n *yaml.Node) ([]Shortcut, error) {
	if n.Kind != yaml.SequenceNode {
		return nil, p.fail(n, "shortcuts must be a list")
	}
	var out []Shortcut
	for _, item := range n.Content {
		s, err := p.shortcut(target(item))
		if err != nil {
			return nil, err
		}
		out = append(out, s)
	}
	return out, nil
}

func (p parser) shortcut(n *yaml.Node) (Shortcut, error) {
	if n.Kind != yaml.MappingNode {
		return Shortcut{}, p.fail(n, "a shortcut must be a map")
	}
	var (
		s        Shortcut
		send     *yaml.Node
		askNodes = map[string]*yaml.Node{}
	)
	err := p.entries(n, func(key string, keyNode, value *yaml.Node) error {
		var err error
		switch key {
		case "label":
			s.Label, err = p.text(value, "label")
		case "send":
			send = value
		case "ask":
			s.Ask, err = p.asks(value, askNodes)
		default:
			err = p.fail(keyNode, "unknown key %q in a shortcut", key)
		}
		return err
	})
	if err != nil {
		return Shortcut{}, err
	}
	if s.Label == "" {
		return Shortcut{}, p.fail(n, "a shortcut needs a label")
	}
	if send == nil {
		return Shortcut{}, p.fail(n, "shortcut %q needs send", s.Label)
	}
	if s.Send, err = p.send(send, s.Label); err != nil {
		return Shortcut{}, err
	}
	return s, p.checkValues(s, send, askNodes)
}

func (p parser) asks(n *yaml.Node, nodes map[string]*yaml.Node) ([]Ask, error) {
	if n.Kind != yaml.MappingNode {
		return nil, p.fail(n, "ask must be a map of names to defaults")
	}
	var out []Ask
	err := p.entries(n, func(key string, keyNode, value *yaml.Node) error {
		if key == today {
			return p.fail(keyNode, "%s is always filled in, so it can't be asked for", today)
		}
		if !valueName.MatchString(key) {
			return p.fail(keyNode, "%q can't name a value", key)
		}
		fallback, err := p.text(value, "a default")
		if err != nil {
			return err
		}
		nodes[key] = keyNode
		out = append(out, Ask{Name: key, Default: fallback})
		return nil
	})
	return out, err
}

func (p parser) send(n *yaml.Node, label string) ([]string, error) {
	var items []*yaml.Node
	switch {
	case n.Kind == yaml.ScalarNode && !isNull(n):
		items = []*yaml.Node{n}
	case n.Kind == yaml.SequenceNode:
		items = n.Content
	case isNull(n):
	default:
		return nil, p.fail(n, "send must be a line or a list of lines")
	}
	if len(items) == 0 {
		return nil, p.fail(n, "shortcut %q needs send", label)
	}
	lines := make([]string, 0, len(items))
	for _, item := range items {
		item = target(item)
		if item.Kind != yaml.ScalarNode || isNull(item) {
			return nil, p.fail(item, "send must be a line or a list of lines")
		}
		if strings.ContainsAny(item.Value, "\r\n") {
			return nil, p.fail(item, "a line to send can't hold a line break")
		}
		lines = append(lines, item.Value)
	}
	return lines, nil
}

// checkValues stops a send that names a value nobody gives, and an ask nothing uses.
func (p parser) checkValues(s Shortcut, send *yaml.Node, asks map[string]*yaml.Node) error {
	used := map[string]bool{}
	for _, line := range s.Send {
		for _, m := range placeholder.FindAllStringSubmatch(line, -1) {
			name := m[1]
			if _, asked := asks[name]; !asked && name != today {
				return p.fail(send, "shortcut %q sends {%s}, which it doesn't ask for", s.Label, name)
			}
			used[name] = true
		}
	}
	for _, ask := range s.Ask {
		if !used[ask.Name] {
			return p.fail(asks[ask.Name], "shortcut %q asks for %s and doesn't send it", s.Label, ask.Name)
		}
	}
	return nil
}

func (p parser) status(n *yaml.Node) ([]Field, error) {
	if n.Kind != yaml.SequenceNode {
		return nil, p.fail(n, "status must be a list")
	}
	var out []Field
	seen := map[string]bool{}
	for _, item := range n.Content {
		item = target(item)
		f, err := p.field(item)
		if err != nil {
			return nil, err
		}
		if seen[f.Name] {
			return nil, p.fail(item, "status field %q is listed twice", f.Name)
		}
		seen[f.Name] = true
		out = append(out, f)
	}
	return out, nil
}

func (p parser) field(n *yaml.Node) (Field, error) {
	if n.Kind != yaml.MappingNode {
		return Field{}, p.fail(n, "a status field must be a map")
	}
	var (
		f       Field
		pattern *yaml.Node
	)
	err := p.entries(n, func(key string, keyNode, value *yaml.Node) error {
		var err error
		switch key {
		case "name":
			f.Name, err = p.text(value, "name")
		case "pattern":
			pattern = value
		default:
			err = p.fail(keyNode, "unknown key %q in a status field", key)
		}
		return err
	})
	if err != nil {
		return Field{}, err
	}
	if f.Name == "" {
		return Field{}, p.fail(n, "a status field needs a name")
	}
	if pattern == nil {
		if !builtIn[f.Name] {
			return Field{}, p.fail(n, "status field %q needs a pattern", f.Name)
		}
		return f, nil
	}
	text, err := p.text(pattern, "pattern")
	if err != nil {
		return Field{}, err
	}
	if f.Pattern, err = regexp.Compile(text); err != nil {
		return Field{}, p.fail(pattern, "status field %q: %v", f.Name, err)
	}
	return f, nil
}

// Lines gives what to send, with values and the date filled in.
func (s Shortcut) Lines(values map[string]string, date string) []string {
	out := make([]string, len(s.Send))
	for i, line := range s.Send {
		out[i] = placeholder.ReplaceAllStringFunc(line, func(found string) string {
			name := found[1 : len(found)-1]
			if name == today {
				return date
			}
			return values[name]
		})
	}
	return out
}

// Match gives the value a line of console output holds for this field: the first group
// that took part, or the whole match for a pattern with none.
func (f Field) Match(line string) (string, bool) {
	if f.Pattern == nil {
		return "", false
	}
	m := f.Pattern.FindStringSubmatch(line)
	if m == nil {
		return "", false
	}
	for _, group := range m[1:] {
		if group != "" {
			return group, true
		}
	}
	return m[0], true
}
