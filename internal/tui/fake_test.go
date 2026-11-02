package tui

import (
	"strings"
	"testing"
	"time"

	tea "charm.land/bubbletea/v2"
	"github.com/charmbracelet/x/ansi"

	"github.com/edcs/preflash/internal/app"
	"github.com/edcs/preflash/internal/board"
	"github.com/edcs/preflash/internal/client"
	"github.com/edcs/preflash/internal/project"
	"github.com/edcs/preflash/internal/rc"
)

// fakeBackend records what the model asks for. Events reach the model through feed.
type fakeBackend struct {
	events   chan app.Event
	sent     [][]string
	runs     []board.Board
	statuses int
	busy     bool
	traces   []bool
	tasks    int
	nvs      int
	hardware int
	changes  [][2]string
	saves    int
}

func newFake() *fakeBackend { return &fakeBackend{events: make(chan app.Event)} }

func (f *fakeBackend) Events() <-chan app.Event { return f.events }
func (f *fakeBackend) Send(lines ...string)     { f.sent = append(f.sent, lines) }
func (f *fakeBackend) Status()                  { f.statuses++ }
func (f *fakeBackend) Trace(on bool)            { f.traces = append(f.traces, on) }
func (f *fakeBackend) Tasks()                   { f.tasks++ }
func (f *fakeBackend) NVS()                     { f.nvs++ }
func (f *fakeBackend) Hardware()                { f.hardware++ }
func (f *fakeBackend) SaveTrace()               { f.saves++ }
func (f *fakeBackend) SetHardware(name, value string) {
	f.changes = append(f.changes, [2]string{name, value})
}
func (f *fakeBackend) Run(b board.Board) bool {
	if f.busy {
		return false
	}
	f.runs = append(f.runs, b)
	return true
}

var modded = board.Board{
	Name:       "snoozel-4b",
	Display:    board.Display{Controller: "st7701", Width: 480, Height: 480},
	I2C:        []board.Chip{{Chip: "tca9554"}, {Chip: "gt911"}, {Chip: "pcf85063"}, {Chip: "qmi8658"}, {Chip: "axp2101"}, {Chip: "es7210"}},
	AudioCodec: "es8311",
	Base:       "ESP32-S3-Touch-LCD-4B",
	Changes:    []board.Change{{Path: "backlight.enable", Value: "gpio43"}},
}

var stock = board.Board{
	Name:    "ESP32-S3-Touch-LCD-4B",
	Display: board.Display{Controller: "st7701", Width: 480, Height: 480},
	I2C:     []board.Chip{{Chip: "gt911"}},
}

var sampleConfig = rc.Config{
	Prompt: "snoozel>",
	Shortcuts: []rc.Shortcut{
		{Label: "Phase", Send: []string{"phase"}},
		{Label: "Fast night", Send: []string{"fake {today}T22:00:00Z", "speed 600"}},
		{Label: "Timer", Send: []string{"timer {seconds}"}, Ask: []rc.Ask{{Name: "seconds", Default: "120"}}},
	},
}

func options() Options {
	return Options{
		Name:      "snoozel",
		Path:      "~/Code/edcs/snoozel",
		Config:    sampleConfig,
		HasConfig: true,
		LVGL:      "9.3.0",
		Choices: []project.Choice{
			{Board: modded, File: "sim/snoozel-4b.yaml", FromProject: true},
			{Board: stock},
		},
	}
}

// running is a model past the launch screen, with an app up.
func running(t *testing.T, f *fakeBackend) *Model {
	t.Helper()
	opts := options()
	opts.Board = &modded
	m := New(f, opts)
	m.now = func() time.Time { return time.Date(2026, 11, 2, 9, 30, 0, 0, time.UTC) }
	resize(m, 100, 30)
	m.Init()
	feed(m, app.Started{Board: modded, PID: 42}, app.Connected{Hello: client.Hello{Project: "snoozel"}})
	return m
}

func resize(m *Model, width, height int) {
	m.Update(tea.WindowSizeMsg{Width: width, Height: height})
}

func feed(m *Model, events ...app.Event) {
	for _, e := range events {
		m.Update(eventMsg{e})
	}
}

func serial(m *Model, text string) { feed(m, app.Serial{Data: []byte(text)}) }

func screen(m *Model) string { return ansi.Strip(m.View().Content) }

func press(m *Model, key string) tea.Cmd {
	k := tea.KeyPressMsg{Text: key}
	switch key {
	case "enter":
		k = tea.KeyPressMsg{Code: tea.KeyEnter}
	case "esc":
		k = tea.KeyPressMsg{Code: tea.KeyEsc}
	case "up":
		k = tea.KeyPressMsg{Code: tea.KeyUp}
	case "down":
		k = tea.KeyPressMsg{Code: tea.KeyDown}
	case "left":
		k = tea.KeyPressMsg{Code: tea.KeyLeft}
	case "right":
		k = tea.KeyPressMsg{Code: tea.KeyRight}
	case "tab":
		k = tea.KeyPressMsg{Code: tea.KeyTab}
	case " ":
		k = tea.KeyPressMsg{Code: tea.KeySpace, Text: " "}
	case "pgup":
		k = tea.KeyPressMsg{Code: tea.KeyPgUp}
	case "pgdown":
		k = tea.KeyPressMsg{Code: tea.KeyPgDown}
	default:
		if name, ok := strings.CutPrefix(key, "ctrl+"); ok {
			k = tea.KeyPressMsg{Code: rune(name[0]), Mod: tea.ModCtrl}
		}
	}
	_, cmd := m.Update(k)
	return cmd
}

func typed(m *Model, text string) {
	for _, r := range text {
		press(m, string(r))
	}
}

func click(m *Model, x, y int) tea.Cmd {
	m.Update(tea.MouseClickMsg{X: x, Y: y, Button: tea.MouseLeft})
	_, cmd := m.Update(tea.MouseReleaseMsg{X: x, Y: y, Button: tea.MouseLeft})
	return cmd
}

// find is where text starts on the screen.
func find(t *testing.T, m *Model, text string) (int, int) {
	t.Helper()
	for y, line := range strings.Split(screen(m), "\n") {
		if at := strings.Index(line, text); at >= 0 {
			return ansi.StringWidth(line[:at]), y
		}
	}
	t.Fatalf("%q isn't on the screen:\n%s", text, screen(m))
	return 0, 0
}

func wantScreen(t *testing.T, m *Model, texts ...string) {
	t.Helper()
	s := screen(m)
	for _, text := range texts {
		if !strings.Contains(s, text) {
			t.Errorf("the screen lacks %q:\n%s", text, s)
		}
	}
}

func refuseScreen(t *testing.T, m *Model, texts ...string) {
	t.Helper()
	s := screen(m)
	for _, text := range texts {
		if strings.Contains(s, text) {
			t.Errorf("the screen shows %q:\n%s", text, s)
		}
	}
}

// fills checks that every line fits and the screen is exactly the window's size.
func fills(t *testing.T, m *Model, width, height int) {
	t.Helper()
	lines := strings.Split(screen(m), "\n")
	if len(lines) != height {
		t.Errorf("%d lines at %dx%d:\n%s", len(lines), width, height, screen(m))
	}
	for _, line := range lines {
		if ansi.StringWidth(line) > width {
			t.Errorf("a line is wider than %d: %q", width, line)
		}
	}
}
