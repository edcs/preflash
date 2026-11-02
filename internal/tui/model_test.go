package tui

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	tea "charm.land/bubbletea/v2"
	"github.com/charmbracelet/x/ansi"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/edcs/preflash/internal/app"
	"github.com/edcs/preflash/internal/board"
	"github.com/edcs/preflash/internal/client"
	"github.com/edcs/preflash/internal/rc"
)

func TestTheToolbarReadsLikeTheMockup(t *testing.T) {
	f := newFake()
	m := running(t, f)
	m.opts.Config.Status = nil
	feed(m, app.Status{Status: client.Status{UptimeMs: 252_000, Backlight: 100, Rotation: 90, FPS: 60, WiFi: "connected"}})

	wantScreen(t, m, "preflash", "snoozel", "ESP32-S3-Touch-LCD-4B + backlight mod", "● running", "04:12",
		"backlight 100%", "rotation 90°", "wifi simulated", "60 fps")
}

func TestTheToolbarShowsTheSerialPort(t *testing.T) {
	t.Setenv("HOME", "/Users/someone")
	m := running(t, newFake())
	m.opts.Config.Status = nil
	feed(m, app.Status{Status: client.Status{SerialPort: "/Users/someone/ports/a.tty"}})
	wantScreen(t, m, "port ~/ports/a.tty")
}

func TestTheToolbarSkipsAPortTheAppDoesNotHave(t *testing.T) {
	m := running(t, newFake())
	feed(m, app.Status{Status: client.Status{}})
	assert.NotContains(t, ansi.Strip(m.toolbarBottom()), "port")
}

func TestAConfigCanPlaceThePort(t *testing.T) {
	m := running(t, newFake())
	m.opts.Config.Status = []rc.Field{{Name: "port"}}
	feed(m, app.Status{Status: client.Status{SerialPort: "/tmp/a.tty", WiFi: "off"}})
	x1, _ := find(t, m, "port /tmp/a.tty")
	x2, _ := find(t, m, "backlight")
	assert.Less(t, x1, x2)
}

func TestUptimePastAnHourShowsHours(t *testing.T) {
	m := running(t, newFake())
	feed(m, app.Status{Status: client.Status{UptimeMs: 3_725_000}})
	wantScreen(t, m, "1:02:05")
}

func TestWifiNamesTheOtherStates(t *testing.T) {
	m := running(t, newFake())
	feed(m, app.Status{Status: client.Status{WiFi: "connecting"}})
	wantScreen(t, m, "wifi connecting")
}

func TestAStockBoardShowsItsName(t *testing.T) {
	f := newFake()
	opts := options()
	opts.Board = &stock
	m := New(f, opts)
	resize(m, 100, 30)
	wantScreen(t, m, "snoozel  ESP32-S3-Touch-LCD-4B")
	refuseScreen(t, m, " mod")
}

func TestFieldsFollowConfigOrderWithLastCaptureWinning(t *testing.T) {
	f := newFake()
	m := running(t, f)
	c := "status:\n  - name: screen\n    pattern: 'screen=(\\w+)'\n  - name: backlight\n  - name: rotation\n    pattern: 'rot (\\d+)'\n"
	parsed, err := rc.Parse("rc", []byte(c))
	require.NoError(t, err)
	m.opts.Config.Status = parsed.Status

	wantScreen(t, m, "screen -", "rotation -")
	serial(m, "screen=clock\nscreen=me")
	wantScreen(t, m, "screen clock", "backlight -", "wifi -")
	serial(m, "nu\nrot 90\n")
	wantScreen(t, m, "screen menu", "rotation 90")
	x1, _ := find(t, m, "screen menu")
	x2, _ := find(t, m, "backlight -")
	x3, _ := find(t, m, "rotation 90")
	x4, _ := find(t, m, "wifi -")
	assert.Less(t, x1, x2)
	assert.Less(t, x2, x3)
	assert.Less(t, x3, x4)
}

func TestARestartForgetsCapturedFields(t *testing.T) {
	f := newFake()
	m := running(t, f)
	parsed, err := rc.Parse("rc", []byte("status:\n  - name: screen\n    pattern: 'screen=(\\w+)'\n"))
	require.NoError(t, err)
	m.opts.Config.Status = parsed.Status
	serial(m, "screen=menu\n")

	feed(m, app.Restarting{})

	wantScreen(t, m, "◐ restarting", "screen -")
	feed(m, app.Connected{})
	wantScreen(t, m, "● running")
}

func TestTheConsoleShowsSerial(t *testing.T) {
	m := running(t, newFake())
	serial(m, "I (snoozel) boot rotation 0 from memory\nphase line\n")
	wantScreen(t, m, "I (snoozel) boot rotation 0 from memory", "phase line")
}

func TestEnterSendsTheLineEchoesItAndClearsTheInput(t *testing.T) {
	f := newFake()
	m := running(t, f)
	typed(m, "phase")
	press(m, "enter")

	assert.Equal(t, [][]string{{"phase"}}, f.sent)
	wantScreen(t, m, "snoozel> phase")
	assert.Empty(t, m.input.Value())
}

func TestAnEmptyLineSendsNothing(t *testing.T) {
	f := newFake()
	m := running(t, f)
	typed(m, "   ")
	press(m, "enter")
	assert.Empty(t, f.sent)
}

func TestThePromptDefaultsToTheProject(t *testing.T) {
	f := newFake()
	opts := options()
	opts.Board = &modded
	opts.Config.Prompt = ""
	m := New(f, opts)
	resize(m, 100, 30)
	wantScreen(t, m, "snoozel> ")
}

func TestThePlaceholderCountsTheShortcuts(t *testing.T) {
	m := running(t, newFake())
	wantScreen(t, m, "type a command, or press 1-3")
	m = running(t, newFake())
	m.opts.Config.Shortcuts = nil
	m.setPlaceholder()
	wantScreen(t, m, "type a command")
	refuseScreen(t, m, "press")
}

func TestNumberKeysRunShortcutsOnlyWhileTheInputIsEmpty(t *testing.T) {
	f := newFake()
	m := running(t, f)

	press(m, "1")
	assert.Equal(t, [][]string{{"phase"}}, f.sent)
	wantScreen(t, m, "snoozel> phase")
	assert.Empty(t, m.input.Value())

	typed(m, "x")
	press(m, "1")
	assert.Equal(t, "x1", m.input.Value())
	assert.Len(t, f.sent, 1)
}

func TestNumberKeysPastTheShortcutsAreTyped(t *testing.T) {
	f := newFake()
	m := running(t, f)
	press(m, "9")
	assert.Equal(t, "9", m.input.Value())
	assert.Empty(t, f.sent)
}

func TestPastedDigitsAreText(t *testing.T) {
	f := newFake()
	m := running(t, f)
	m.Update(tea.PasteMsg{Content: "1"})
	assert.Equal(t, "1", m.input.Value())
	assert.Empty(t, f.sent)
}

func TestAShortcutSendsItsLinesWithTheDateFilledIn(t *testing.T) {
	f := newFake()
	m := running(t, f)
	press(m, "2")
	assert.Equal(t, [][]string{{"fake 2026-11-02T22:00:00Z", "speed 600"}}, f.sent)
	wantScreen(t, m, "snoozel> fake 2026-11-02T22:00:00Z", "snoozel> speed 600")
}

func TestAskShowsTheDefaultAndEnterSendsIt(t *testing.T) {
	f := newFake()
	m := running(t, f)

	press(m, "3")
	wantScreen(t, m, "Timer seconds>", "enter send", "esc cancel")
	assert.Equal(t, "120", m.input.Value())
	assert.Empty(t, f.sent)

	press(m, "enter")
	assert.Equal(t, [][]string{{"timer 120"}}, f.sent)
	wantScreen(t, m, "snoozel> timer 120")
	assert.Empty(t, m.input.Value())
}

func TestAskTakesATypedValue(t *testing.T) {
	f := newFake()
	m := running(t, f)
	press(m, "3")
	m.input.SetValue("")
	typed(m, "45")
	press(m, "enter")
	assert.Equal(t, [][]string{{"timer 45"}}, f.sent)
}

func TestEscCancelsAnAsk(t *testing.T) {
	f := newFake()
	m := running(t, f)
	press(m, "3")
	press(m, "esc")
	assert.Empty(t, f.sent)
	assert.Empty(t, m.input.Value())
	wantScreen(t, m, "snoozel> ")
	refuseScreen(t, m, "Timer seconds>")
}

func TestAnAskWithNoDefaultWaitsForAValue(t *testing.T) {
	f := newFake()
	m := running(t, f)
	m.opts.Config.Shortcuts = []rc.Shortcut{{Label: "Set", Send: []string{"set {a} {b}"}, Ask: []rc.Ask{{Name: "a"}, {Name: "b", Default: "2"}}}}

	press(m, "1")
	press(m, "enter")
	assert.Empty(t, f.sent, "nothing to fill in yet")
	wantScreen(t, m, "Set a>")

	typed(m, "1")
	press(m, "enter")
	wantScreen(t, m, "Set b>")
	press(m, "enter")
	assert.Equal(t, [][]string{{"set 1 2"}}, f.sent)
}

func TestNumberKeysAreTextDuringAnAsk(t *testing.T) {
	f := newFake()
	m := running(t, f)
	m.opts.Config.Shortcuts = []rc.Shortcut{{Label: "Set", Send: []string{"set {a}"}, Ask: []rc.Ask{{Name: "a"}}}}
	press(m, "1")
	press(m, "1")
	assert.Equal(t, "1", m.input.Value())
}

func TestHistoryWalksBackAndForward(t *testing.T) {
	f := newFake()
	m := running(t, f)
	for _, line := range []string{"one", "two", "two"} {
		typed(m, line)
		press(m, "enter")
	}
	typed(m, "draft")

	press(m, "up")
	assert.Equal(t, "two", m.input.Value())
	press(m, "up")
	assert.Equal(t, "one", m.input.Value(), "a repeat counts once")
	press(m, "up")
	assert.Equal(t, "one", m.input.Value())
	press(m, "down")
	assert.Equal(t, "two", m.input.Value())
	press(m, "down")
	assert.Equal(t, "draft", m.input.Value())
}

func TestShortcutsJoinHistoryNot(t *testing.T) {
	f := newFake()
	m := running(t, f)
	press(m, "1")
	press(m, "up")
	assert.Empty(t, m.input.Value())
}

func TestClearEmptiesTheConsole(t *testing.T) {
	m := running(t, newFake())
	serial(m, "old line\n")
	press(m, "ctrl+l")
	refuseScreen(t, m, "old line")
}

func TestRebuildAsksForARunOnTheSameBoard(t *testing.T) {
	f := newFake()
	m := running(t, f)
	press(m, "ctrl+r")
	require.Len(t, f.runs, 2, "one from the start, one from the key")
	assert.Equal(t, modded.Name, f.runs[1].Name)
}

func TestRebuildWhileBuildingSaysSo(t *testing.T) {
	f := newFake()
	m := running(t, f)
	f.busy = true
	press(m, "ctrl+r")
	wantScreen(t, m, "A build is already running.")
}

func TestABuildShowsInTheConsoleAndTheToolbar(t *testing.T) {
	m := running(t, newFake())
	feed(m, app.Building{Board: modded}, app.BuildOutput{Line: "[ 12%] Building main.cpp"})
	wantScreen(t, m, "◐ building", "[ 12%] Building main.cpp", "Building snoozel for snoozel-4b")
}

func TestAFailedBuildKeepsTheOldAppAndShowsTheError(t *testing.T) {
	m := running(t, newFake())
	feed(m, app.Building{Board: modded}, app.BuildOutput{Line: "main.cpp:1: error: oops"},
		app.BuildFailed{Err: errors.New("the build failed: cmake"), AppRunning: true})

	wantScreen(t, m, "● running", "main.cpp:1: error: oops", "the build failed: cmake", "The old app keeps running.")
	assert.NotEqual(t, stateBuildFailed, m.state)
}

func TestAFailedFirstBuildSaysTheStateAndHowToRetry(t *testing.T) {
	f := newFake()
	opts := options()
	opts.Board = &modded
	m := New(f, opts)
	resize(m, 100, 30)
	feed(m, app.Building{Board: modded}, app.BuildFailed{Err: errors.New("the build failed: cmake")})

	wantScreen(t, m, "■ build failed", "^R tries again")
	refuseScreen(t, m, "The old app keeps running.")
}

func TestAChangeOfBoardShowsOnceItRuns(t *testing.T) {
	m := running(t, newFake())
	feed(m, app.Building{Board: stock})
	wantScreen(t, m, "ESP32-S3-Touch-LCD-4B + backlight mod")
	feed(m, app.Started{Board: stock, PID: 43})
	refuseScreen(t, m, " mod")
}

func TestAnExitGoesToStopped(t *testing.T) {
	m := running(t, newFake())
	serial(m, "last words")
	feed(m, app.Exited{Reason: "The app exited."})

	wantScreen(t, m, "○ stopped", "last words", "The app exited.", "^R builds and runs")
}

func TestACrashGoesToCrashedAndKeepsTheOutput(t *testing.T) {
	m := running(t, newFake())
	serial(m, "preflash: esp_foo is not simulated\n")
	feed(m, app.Exited{Crashed: true, Reason: "The app died: abort trap."})

	wantScreen(t, m, "■ crashed", "preflash: esp_foo is not simulated", "The app died: abort trap.")
}

func TestAnAppThatNeverConnectedShowsItsLog(t *testing.T) {
	m := running(t, newFake())
	feed(m, app.Exited{Crashed: true, Reason: "The app exited with code 1.", Tail: []string{"preflash: can't open a window"}})
	wantScreen(t, m, "preflash: can't open a window")
}

func TestSendingWithNothingRunningSaysHowToStart(t *testing.T) {
	f := newFake()
	m := running(t, f)
	feed(m, app.Exited{Reason: "The app exited."})

	typed(m, "phase")
	press(m, "enter")
	press(m, "1")

	assert.Empty(t, f.sent)
	wantScreen(t, m, "Nothing is running. ^R builds and runs.")
}

func TestASendThatFailsSaysSo(t *testing.T) {
	m := running(t, newFake())
	feed(m, app.SendFailed{Line: "phase", Err: errors.New("the console queue is full")})
	wantScreen(t, m, "phase", "the console queue is full")
}

func TestDroppedOutputIsShown(t *testing.T) {
	m := running(t, newFake())
	feed(m, app.Dropped{Bytes: 4096})
	wantScreen(t, m, "4096 bytes of serial were dropped")
}

func TestControlCQuits(t *testing.T) {
	m := running(t, newFake())
	cmd := press(m, "ctrl+c")
	require.NotNil(t, cmd)
	_, quits := cmd().(tea.QuitMsg)
	assert.True(t, quits)
}

func TestTheTickAsksForStatusWhileAnAppIsUp(t *testing.T) {
	f := newFake()
	m := running(t, f)
	before := f.statuses
	m.Update(tickMsg{})
	assert.Equal(t, before+1, f.statuses)

	feed(m, app.Exited{Reason: "The app exited."})
	m.Update(tickMsg{})
	assert.Equal(t, before+1, f.statuses)
}

func TestAConnectionAsksForStatusAtOnce(t *testing.T) {
	f := newFake()
	m := running(t, f)
	assert.Equal(t, 1, f.statuses)
	feed(m, app.Connected{})
	assert.Equal(t, 2, f.statuses)
}

func TestPageKeysScrollAndNewOutputStaysPut(t *testing.T) {
	m := running(t, newFake())
	for i := range 200 {
		serial(m, strings.Repeat("line ", 2)+string(rune('a'+i%26))+"\n")
	}
	bottom := screen(m)
	press(m, "pgup")
	assert.NotEqual(t, bottom, screen(m))
	wantScreen(t, m, "PgDn")
	held := screen(m)

	serial(m, "a new line\n")
	assert.Equal(t, held, screen(m), "new output mustn't move what's being read")

	press(m, "pgdown")
	press(m, "pgdown")
	wantScreen(t, m, "a new line")
	refuseScreen(t, m, "PgDn")
}

func TestSendingReturnsToTheBottom(t *testing.T) {
	m := running(t, newFake())
	for range 200 {
		serial(m, "line\n")
	}
	press(m, "pgup")
	typed(m, "x")
	press(m, "enter")
	assert.Zero(t, m.scroll)
}

func TestTheWheelScrolls(t *testing.T) {
	m := running(t, newFake())
	for range 200 {
		serial(m, "line\n")
	}
	m.Update(tea.MouseWheelMsg{X: 5, Y: 5, Button: tea.MouseWheelUp})
	assert.Equal(t, wheelRows, m.scroll)
	m.Update(tea.MouseWheelMsg{X: 5, Y: 5, Button: tea.MouseWheelDown})
	assert.Zero(t, m.scroll)
}

func TestClickingAChipRunsIt(t *testing.T) {
	f := newFake()
	m := running(t, f)
	x, y := find(t, m, "Phase")

	click(m, x+1, y)

	assert.Equal(t, [][]string{{"phase"}}, f.sent)
}

func TestClickingTheChipsKeyWorksToo(t *testing.T) {
	f := newFake()
	m := running(t, f)
	x, y := find(t, m, "Fast night")
	click(m, x-2, y)
	assert.Len(t, f.sent, 1)
}

func TestAPressThatLeavesTheChipDoesNothing(t *testing.T) {
	f := newFake()
	m := running(t, f)
	x, y := find(t, m, "Phase")
	m.Update(tea.MouseClickMsg{X: x, Y: y, Button: tea.MouseLeft})
	m.Update(tea.MouseReleaseMsg{X: x, Y: 0, Button: tea.MouseLeft})
	assert.Empty(t, f.sent)
}

func TestARightClickDoesNothing(t *testing.T) {
	f := newFake()
	m := running(t, f)
	x, y := find(t, m, "Phase")
	m.Update(tea.MouseClickMsg{X: x, Y: y, Button: tea.MouseRight})
	m.Update(tea.MouseReleaseMsg{X: x, Y: y, Button: tea.MouseRight})
	assert.Empty(t, f.sent)
}

func TestClickingBesideTheChipsDoesNothing(t *testing.T) {
	f := newFake()
	m := running(t, f)
	_, y := find(t, m, "Phase")
	click(m, 90, y)
	assert.Empty(t, f.sent)
}

func TestClicksOnTheFooterWork(t *testing.T) {
	f := newFake()
	m := running(t, f)
	x, y := find(t, m, "^L clear")
	serial(m, "old line\n")
	click(m, x, y)
	refuseScreen(t, m, "old line")

	x, y = find(t, m, "rebuild")
	click(m, x, y)
	assert.Len(t, f.runs, 2)
}

func TestChipsWrapOnANarrowWindowAndStillClick(t *testing.T) {
	f := newFake()
	m := running(t, f)
	m.opts.Config.Shortcuts = nil
	for _, label := range strings.Fields("Phase Menu Bedtime Night Wake Timer Rotate Real") {
		m.opts.Config.Shortcuts = append(m.opts.Config.Shortcuts, rc.Shortcut{Label: label + "-long-name", Send: []string{label}})
	}
	resize(m, 60, 16)
	fills(t, m, 60, 16)

	x, y := find(t, m, "Real")
	click(m, x, y)
	assert.Equal(t, [][]string{{"Real"}}, f.sent)
}

func TestWindowsTooSmallSayAndKeepWorking(t *testing.T) {
	m := running(t, newFake())
	for _, size := range [][2]int{{59, 16}, {60, 15}, {20, 5}} {
		resize(m, size[0], size[1])
		wantScreen(t, m, "too small")
		refuseScreen(t, m, "running")
	}
	cmd := press(m, "ctrl+c")
	_, quits := cmd().(tea.QuitMsg)
	assert.True(t, quits)
}

func TestTheScreenFillsEverySize(t *testing.T) {
	m := running(t, newFake())
	serial(m, strings.Repeat("a long line of output ", 30)+"\nshort\n")
	for _, size := range [][2]int{{60, 16}, {80, 24}, {100, 30}, {120, 40}, {200, 60}} {
		resize(m, size[0], size[1])
		fills(t, m, size[0], size[1])
	}
}

func TestViewTakesTheScreenAndTheMouse(t *testing.T) {
	m := running(t, newFake())
	v := m.View()
	assert.True(t, v.AltScreen)
	assert.Equal(t, tea.MouseModeCellMotion, v.MouseMode)
}

func TestLongCapturesAreCut(t *testing.T) {
	m := running(t, newFake())
	parsed, err := rc.Parse("rc", []byte("status:\n  - name: a\n    pattern: 'a=(.*)'\n"))
	require.NoError(t, err)
	m.opts.Config.Status = parsed.Status
	serial(m, "a="+strings.Repeat("x", 300)+"\n")
	fills(t, m, 100, 30)
}

func TestBoardKeyOpensThePickerAndEscReturns(t *testing.T) {
	f := newFake()
	m := running(t, f)
	press(m, "ctrl+b")
	wantScreen(t, m, "From this project", "Built in", "● running", "esc back")
	press(m, "esc")
	wantScreen(t, m, "snoozel>")
}

func TestPickingABoardFromRunningBuildsForIt(t *testing.T) {
	f := newFake()
	m := running(t, f)
	press(m, "ctrl+b")
	press(m, "down")
	press(m, "enter")
	assert.Equal(t, stock.Name, f.runs[len(f.runs)-1].Name)
	wantScreen(t, m, "snoozel>")
}

func TestClickingAnAskingChipKeepsWhatWasTyped(t *testing.T) {
	f := newFake()
	m := running(t, f)
	typed(m, "half a comm")
	x, y := find(t, m, "3 Timer")

	click(m, x, y)
	assert.Equal(t, "120", m.input.Value())
	press(m, "esc")
	assert.Equal(t, "half a comm", m.input.Value())

	click(m, x, y)
	press(m, "enter")
	assert.Equal(t, [][]string{{"timer 120"}}, f.sent)
	assert.Equal(t, "half a comm", m.input.Value())
}

func TestClickingAChipDuringAnAskDoesNothing(t *testing.T) {
	f := newFake()
	m := running(t, f)
	typed(m, "draft")
	x, y := find(t, m, "3 Timer")
	click(m, x, y)
	x, y = find(t, m, "1 Phase")

	click(m, x, y)

	assert.Empty(t, f.sent)
	wantScreen(t, m, "Timer seconds>")
	press(m, "esc")
	assert.Equal(t, "draft", m.input.Value())
}

func TestRebuildReadsTheBoardFileAgain(t *testing.T) {
	path := filepath.Join(t.TempDir(), "b.yaml")
	require.NoError(t, os.WriteFile(path, []byte("extends: ESP32-S3-Touch-LCD-4B\nname: first\n"), 0o644))
	b, err := board.Load(path)
	require.NoError(t, err)
	f := newFake()
	opts := options()
	opts.Board = &b
	m := New(f, opts)
	resize(m, 100, 30)
	m.Init()

	require.NoError(t, os.WriteFile(path, []byte("extends: ESP32-S3-Touch-LCD-4B\nname: second\n"), 0o644))
	press(m, "ctrl+r")
	assert.Equal(t, "second", f.runs[len(f.runs)-1].Name)

	require.NoError(t, os.WriteFile(path, []byte("extends: nothing-at-all\n"), 0o644))
	press(m, "ctrl+r")
	assert.Len(t, f.runs, 2, "a broken file builds nothing")
	wantScreen(t, m, "nothing-at-all")
}

func TestAnUnfinishedLineStillCountsForStatusFields(t *testing.T) {
	m := running(t, newFake())
	parsed, err := rc.Parse("rc", []byte("status:\n  - name: a\n    pattern: 'a=(\\w+)'\n"))
	require.NoError(t, err)
	m.opts.Config.Status = parsed.Status

	serial(m, "a=first")
	typed(m, "x")
	press(m, "enter")
	wantScreen(t, m, "a first")

	serial(m, "a=last")
	feed(m, app.Exited{Reason: "The app exited."})
	assert.Equal(t, "last", m.captures["a"])
}

func TestANewBootStartsOnItsOwnLine(t *testing.T) {
	m := running(t, newFake())
	parsed, err := rc.Parse("rc", []byte("status:\n  - name: a\n    pattern: 'a=(\\w+)'\n"))
	require.NoError(t, err)
	m.opts.Config.Status = parsed.Status

	serial(m, "half a li")
	feed(m, app.Restarting{})
	serial(m, "ne, a=old")
	feed(m, app.Connected{})
	serial(m, "E (1) first\n")

	wantScreen(t, m, "half a line, a=old")
	x, _ := find(t, m, "E (1) first")
	assert.Equal(t, sidePad, x)
	wantScreen(t, m, "a -")
}

func TestFieldNamesCantBreakTheLayout(t *testing.T) {
	m := running(t, newFake())
	parsed, err := rc.Parse("rc", []byte("status:\n  - name: \"a\\nb\"\n    pattern: x\n"))
	require.NoError(t, err)
	m.opts.Config.Status = parsed.Status
	fills(t, m, 100, 30)
}
