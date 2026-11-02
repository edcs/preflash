package rc

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

const full = `board: sim/snoozel-4b.yaml

console:
  prompt: "snoozel>"

shortcuts:
  - label: Phase
    send: phase
  - label: Fast night
    send:
      - fake {today}T22:00:00Z
      - speed 600
  - label: Timer
    send: timer {seconds}
    ask:
      seconds: 120

status:
  - name: screen
    pattern: 'screen=(\w+)'
  - name: backlight
  - name: clock
    pattern: fake clock at \S+
`

func TestParseReadsEveryPart(t *testing.T) {
	c, err := Parse("rc", []byte(full))
	require.NoError(t, err)

	assert.Equal(t, "sim/snoozel-4b.yaml", c.Board)
	assert.Equal(t, "snoozel>", c.Prompt)
	require.Len(t, c.Shortcuts, 3)
	assert.Equal(t, []string{"phase"}, c.Shortcuts[0].Send)
	assert.Equal(t, []string{"fake {today}T22:00:00Z", "speed 600"}, c.Shortcuts[1].Send)
	assert.Equal(t, []Ask{{Name: "seconds", Default: "120"}}, c.Shortcuts[2].Ask)
	require.Len(t, c.Status, 3)
	assert.Equal(t, "screen", c.Status[0].Name)
	assert.Nil(t, c.Status[1].Pattern)
}

func TestAnEmptyFileIsNoConfig(t *testing.T) {
	c, err := Parse("rc", []byte("# nothing yet\n"))
	require.NoError(t, err)
	assert.Equal(t, Config{}, c)
}

func TestAsksKeepTheirOrder(t *testing.T) {
	c, err := Parse("rc", []byte("shortcuts:\n  - label: Go\n    send: go {zebra} {apple}\n    ask:\n      zebra: 1\n      apple:\n"))
	require.NoError(t, err)
	assert.Equal(t, []Ask{{Name: "zebra", Default: "1"}, {Name: "apple"}}, c.Shortcuts[0].Ask)
}

func TestErrorsNameTheLine(t *testing.T) {
	for name, test := range map[string]struct{ text, want string }{
		"top key":        {"board: a\nboarf: b\n", `rc:2: unknown key "boarf"`},
		"console key":    {"console:\n  prompt: a\n  colour: red\n", `rc:3: unknown key "colour" in console`},
		"shortcut key":   {"shortcuts:\n  - label: a\n    send: b\n    keys: 1\n", `rc:4: unknown key "keys" in a shortcut`},
		"status key":     {"status:\n  - name: a\n    pattern: b\n    colour: c\n", `rc:4: unknown key "colour" in a status field`},
		"repeated key":   {"board: a\nboard: b\n", `rc:2: board is set twice`},
		"no label":       {"shortcuts:\n  - send: a\n", `rc:2: a shortcut needs a label`},
		"no send":        {"shortcuts:\n  - label: a\n", `rc:2: shortcut "a" needs send`},
		"empty send":     {"shortcuts:\n  - label: a\n    send: []\n", `rc:3: shortcut "a" needs send`},
		"null send":      {"shortcuts:\n  - label: a\n    send:\n", `rc:3: shortcut "a" needs send`},
		"two lines":      {"shortcuts:\n  - label: a\n    send: \"x\\ny\"\n", `rc:3: a line to send can't hold a line break`},
		"map send":       {"shortcuts:\n  - label: a\n    send: {x: 1}\n", `rc:3: send must be a line or a list of lines`},
		"unknown value":  {"shortcuts:\n  - label: a\n    send: go {size}\n", `rc:3: shortcut "a" sends {size}, which it doesn't ask for`},
		"unused ask":     {"shortcuts:\n  - label: a\n    send: go\n    ask:\n      size: 1\n", `rc:5: shortcut "a" asks for size and doesn't send it`},
		"ask for today":  {"shortcuts:\n  - label: a\n    send: go {today}\n    ask:\n      today: 1\n", `rc:5: today is always filled in, so it can't be asked for`},
		"bad ask name":   {"shortcuts:\n  - label: a\n    send: go\n    ask:\n      a b: 1\n", `rc:5: "a b" can't name a value`},
		"not a map":      {"- a\n", `rc:1: .preflashrc must be a map`},
		"bad shortcuts":  {"shortcuts: a\n", `rc:1: shortcuts must be a list`},
		"bad console":    {"console: a\n", `rc:1: console must be a map`},
		"bad board":      {"board: [a]\n", `rc:1: board must be text`},
		"no name":        {"status:\n  - pattern: a\n", `rc:2: a status field needs a name`},
		"no pattern":     {"status:\n  - name: a\n", `rc:2: status field "a" needs a pattern`},
		"bad pattern":    {"status:\n  - name: a\n    pattern: '('\n", `rc:3: status field "a": `},
		"repeated field": {"status:\n  - name: a\n    pattern: b\n  - name: a\n    pattern: c\n", `rc:4: status field "a" is listed twice`},
		"broken yaml":    {"board: [a\n", `rc: `},
	} {
		t.Run(name, func(t *testing.T) {
			_, err := Parse("rc", []byte(test.text))
			require.Error(t, err)
			assert.Contains(t, err.Error(), test.want)
		})
	}
}

func TestLinesFillInValues(t *testing.T) {
	s := Shortcut{Send: []string{"fake {today}T22:00:00Z", "timer {seconds}", `json {"a":1}`}}
	assert.Equal(t,
		[]string{"fake 2026-11-02T22:00:00Z", "timer 90", `json {"a":1}`},
		s.Lines(map[string]string{"seconds": "90"}, "2026-11-02"))
}

func TestFieldsMatch(t *testing.T) {
	c, err := Parse("rc", []byte("status:\n  - name: a\n    pattern: 'rotation (\\d+)'\n  - name: b\n    pattern: fake \\S+\n  - name: c\n    pattern: '(x)|(y)'\n"))
	require.NoError(t, err)

	value, ok := c.Status[0].Match("I (orient) rotation 90")
	assert.True(t, ok)
	assert.Equal(t, "90", value, "the capture, not the whole match")

	value, ok = c.Status[1].Match("a fake clock")
	assert.True(t, ok)
	assert.Equal(t, "fake clock", value, "no capture means the whole match")

	value, ok = c.Status[2].Match("y")
	assert.True(t, ok)
	assert.Equal(t, "y", value, "the first group that took part")

	_, ok = c.Status[0].Match("nothing here")
	assert.False(t, ok)
}

func TestBuiltInFieldsDoNotMatchLines(t *testing.T) {
	c, err := Parse("rc", []byte("status:\n  - name: wifi\n  - name: rotation\n    pattern: 'r(\\d)'\n"))
	require.NoError(t, err)
	_, ok := c.Status[0].Match("anything")
	assert.False(t, ok)
	assert.True(t, c.Status[1].Pattern != nil, "a pattern replaces the built-in value")
}

func TestLoad(t *testing.T) {
	dir := t.TempDir()
	c, found, err := Load(dir)
	require.NoError(t, err)
	assert.False(t, found)
	assert.Equal(t, Config{}, c)

	require.NoError(t, os.WriteFile(filepath.Join(dir, ".preflashrc"), []byte("board: a\nx: 1\n"), 0o644))
	_, _, err = Load(dir)
	require.Error(t, err)
	assert.Contains(t, err.Error(), filepath.Join(dir, ".preflashrc")+":2:")

	require.NoError(t, os.WriteFile(filepath.Join(dir, ".preflashrc"), []byte("board: a\n"), 0o644))
	c, found, err = Load(dir)
	require.NoError(t, err)
	assert.True(t, found)
	assert.Equal(t, "a", c.Board)
}

func TestAGroupThatMatchedNothingIsAnEmptyValue(t *testing.T) {
	c, err := Parse("rc", []byte("status:\n  - name: a\n    pattern: 'x=(\\d*)'\n"))
	require.NoError(t, err)
	value, ok := c.Status[0].Match("x=")
	assert.True(t, ok)
	assert.Empty(t, value)
}
