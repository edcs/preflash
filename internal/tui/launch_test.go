package tui

import (
	"errors"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/edcs/preflash/internal/app"
	"github.com/edcs/preflash/internal/board"
	"github.com/edcs/preflash/internal/project"
)

func launch(f *fakeBackend) *Model {
	m := New(f, options())
	resize(m, 100, 30)
	return m
}

func TestLaunchReadsLikeTheMockup(t *testing.T) {
	m := launch(newFake())
	wantScreen(t, m, "preflash", "~/Code/edcs/snoozel", "○ not running",
		"Board", "From this project", "› snoozel-4b, 4B + backlight mod", "Built in", "ESP32-S3-Touch-LCD-4B",
		"screen", "480×480 ST7701", "touch", "GT911", "rtc", "PCF85063", "motion", "QMI8658", "power", "AXP2101",
		"audio", "ES8311", "based on", "changes", "backlight enable on GPIO43",
		"✓ ESP-IDF project", "✓ LVGL 9.3.0", "✓ .preflashrc, 3 shortcuts",
		"↑↓ choose", "enter build and run", "^C quit")
	refuseScreen(t, m, "esc back")
}

func TestOtherChipsStillShow(t *testing.T) {
	m := launch(newFake())
	wantScreen(t, m, "other", "TCA9554, ES7210")
}

func TestChecksSayWhatIsMissing(t *testing.T) {
	opts := options()
	opts.LVGL, opts.HasConfig = "", false
	opts.Config.Shortcuts = nil
	m := New(newFake(), opts)
	resize(m, 100, 30)
	wantScreen(t, m, "○ no LVGL", "○ no .preflashrc")
}

func TestOneShortcutIsSingular(t *testing.T) {
	opts := options()
	opts.Config.Shortcuts = opts.Config.Shortcuts[:1]
	m := New(newFake(), opts)
	resize(m, 100, 30)
	wantScreen(t, m, ".preflashrc, 1 shortcut")
	refuseScreen(t, m, "1 shortcuts")
}

func TestArrowsChooseAndTheDetailsFollow(t *testing.T) {
	m := launch(newFake())
	press(m, "down")
	wantScreen(t, m, "› ESP32-S3-Touch-LCD-4B")
	refuseScreen(t, m, "based on", "changes")
	press(m, "down")
	wantScreen(t, m, "› ESP32-S3-Touch-LCD-4B")
	press(m, "up")
	wantScreen(t, m, "› snoozel-4b")
	press(m, "up")
	wantScreen(t, m, "› snoozel-4b")
}

func TestEnterBuildsAndRuns(t *testing.T) {
	f := newFake()
	m := launch(f)
	press(m, "down")
	press(m, "enter")

	require.Len(t, f.runs, 1)
	assert.Equal(t, stock.Name, f.runs[0].Name)
	wantScreen(t, m, "snoozel>", "ESP32-S3-Touch-LCD-4B")
	feed(m, app.Building{Board: stock})
	wantScreen(t, m, "◐ building")
}

func TestEscDoesNothingBeforeARun(t *testing.T) {
	m := launch(newFake())
	press(m, "esc")
	wantScreen(t, m, "From this project")
}

func TestABoardFileThatWontLoadSaysWhy(t *testing.T) {
	f := newFake()
	opts := options()
	opts.Choices[0] = project.Choice{File: "sim/broken.yaml", FromProject: true, Err: errors.New("sim/broken.yaml: extends nothing")}
	m := New(f, opts)
	resize(m, 100, 30)

	wantScreen(t, m, "› sim/broken.yaml", "This board file won't load", "extends nothing")
	press(m, "enter")
	assert.Empty(t, f.runs)
	wantScreen(t, m, "From this project")
}

func TestOnlyStockBoardsLeaveOutTheProjectHeading(t *testing.T) {
	opts := options()
	opts.Choices = opts.Choices[1:]
	m := New(newFake(), opts)
	resize(m, 100, 30)
	wantScreen(t, m, "Built in", "› ESP32-S3-Touch-LCD-4B")
	refuseScreen(t, m, "From this project")
}

func TestClickingABoardChoosesItAndAgainRunsIt(t *testing.T) {
	f := newFake()
	m := launch(f)
	x, y := find(t, m, "  ESP32-S3-Touch-LCD-4B")

	click(m, x+3, y)
	wantScreen(t, m, "› ESP32-S3-Touch-LCD-4B")
	assert.Empty(t, f.runs)

	x, y = find(t, m, "› ESP32-S3-Touch-LCD-4B")
	click(m, x+3, y)
	require.Len(t, f.runs, 1)
}

func TestTheConfiguredBoardIsChosenWhenPickerOpens(t *testing.T) {
	f := newFake()
	opts := options()
	opts.Board = &stock
	m := New(f, opts)
	resize(m, 100, 30)
	press(m, "ctrl+b")
	wantScreen(t, m, "› ESP32-S3-Touch-LCD-4B")
}

func TestANamedBoardSkipsLaunch(t *testing.T) {
	f := newFake()
	opts := options()
	opts.Board = &modded
	m := New(f, opts)
	resize(m, 100, 30)
	cmd := m.Init()
	require.NotNil(t, cmd)
	wantScreen(t, m, "snoozel>", "^R rebuild")
	refuseScreen(t, m, "From this project")
}

func TestLaunchFitsEverySize(t *testing.T) {
	m := launch(newFake())
	for _, size := range [][2]int{{60, 16}, {80, 24}, {100, 30}, {120, 40}, {200, 60}} {
		resize(m, size[0], size[1])
		fills(t, m, size[0], size[1])
		wantScreen(t, m, "enter build and run")
	}
}

func TestManyBoardsScrollToKeepTheChoiceInView(t *testing.T) {
	opts := options()
	for range 30 {
		opts.Choices = append(opts.Choices, opts.Choices[1])
	}
	m := New(newFake(), opts)
	resize(m, 80, 16)
	for range 31 {
		press(m, "down")
	}
	fills(t, m, 80, 16)
	wantScreen(t, m, "› ESP32-S3-Touch-LCD-4B")
	x, y := find(t, m, "› ESP32-S3-Touch-LCD-4B")
	click(m, x, y)
	assert.Len(t, m.b.(*fakeBackend).runs, 1)
}

func TestAFixedBoardFileRunsWithoutRestarting(t *testing.T) {
	f := newFake()
	opts := options()
	broken := project.Choice{File: "sim/mod.yaml", FromProject: true, Err: errors.New("no")}
	fixed := project.Choice{Board: modded, File: "sim/mod.yaml", FromProject: true}
	opts.Choices = []project.Choice{broken}
	current := []project.Choice{broken}
	opts.Boards = func() []project.Choice { return current }
	m := New(f, opts)
	resize(m, 100, 30)

	press(m, "enter")
	assert.Empty(t, f.runs)
	wantScreen(t, m, "This board file won't load")

	current = []project.Choice{fixed}
	press(m, "enter")
	require.Len(t, f.runs, 1)
	assert.Equal(t, modded.Name, f.runs[0].Name)
}

func TestOpeningThePickerRereadsTheBoardsAndKeepsTheChoice(t *testing.T) {
	f := newFake()
	opts := options()
	opts.Board = &stock
	extra := project.Choice{Board: modded, File: "sim/new.yaml", FromProject: true}
	opts.Boards = func() []project.Choice { return append([]project.Choice{extra}, options().Choices...) }
	m := New(f, opts)
	resize(m, 100, 30)

	press(m, "ctrl+b")

	assert.Len(t, m.opts.Choices, 3)
	wantScreen(t, m, "› ESP32-S3-Touch-LCD-4B")
}

func TestClickingAChosenBrokenBoardSurvivesTheListShrinking(t *testing.T) {
	f := newFake()
	opts := options()
	broken := project.Choice{File: "sim/z.yaml", FromProject: true, Err: errors.New("no")}
	opts.Choices = []project.Choice{opts.Choices[0], opts.Choices[1], broken}
	shorter := []project.Choice{opts.Choices[1]}
	opts.Boards = func() []project.Choice { return shorter }
	m := New(f, opts)
	resize(m, 100, 30)
	press(m, "down")
	press(m, "down")
	x, y := find(t, m, "› sim/z.yaml")

	click(m, x, y)

	assert.Zero(t, m.chosen)
	screen(m) // an index past the end would panic here
}

func TestBoardTextCantBreakTheLayout(t *testing.T) {
	odd := modded
	odd.Name = "two\nlines"
	odd.Base = "base\x1b[31m\nname"
	odd.Changes = []board.Change{{Path: "a", Value: "b\nc"}}
	opts := options()
	opts.Choices[0].Board = odd
	m := New(newFake(), opts)
	resize(m, 100, 30)
	fills(t, m, 100, 30)

	opts = options()
	opts.Board = &odd
	m = New(newFake(), opts)
	resize(m, 100, 30)
	fills(t, m, 100, 30)
}

func TestABoardWithNoNameStillWorksInThePicker(t *testing.T) {
	nameless := modded
	nameless.Name = ""
	f := newFake()
	opts := options()
	opts.Choices[0] = project.Choice{Board: nameless, File: "sim/plain.yaml", FromProject: true}
	m := New(f, opts)
	resize(m, 100, 30)
	wantScreen(t, m, "sim/plain.yaml")

	press(m, "enter")
	press(m, "ctrl+b")
	wantScreen(t, m, "esc back")
	press(m, "esc")
	refuseScreen(t, m, "From this project")
}

func TestEnterOnADeletedBoardRunsNothing(t *testing.T) {
	f := newFake()
	opts := options()
	broken := project.Choice{File: "sim/gone.yaml", FromProject: true, Err: errors.New("no")}
	opts.Choices = []project.Choice{opts.Choices[1], broken}
	rest := opts.Choices[:1]
	opts.Boards = func() []project.Choice { return rest }
	m := New(f, opts)
	resize(m, 100, 30)
	press(m, "down")

	press(m, "enter")

	assert.Empty(t, f.runs)
	wantScreen(t, m, "› ESP32-S3-Touch-LCD-4B")
}
