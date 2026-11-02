package tui

import (
	"errors"
	"os/exec"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/edcs/preflash/internal/app"
	"github.com/edcs/preflash/internal/crash"
)

// recorder stands in for lldb, the editor and the clipboard.
type recorder struct {
	debugged []int
	edited   []string
	line     int
	copied   []string
	copyErr  error
}

func (r *recorder) tools() Tools {
	return Tools{
		Debugger: func(pid int) *exec.Cmd {
			r.debugged = append(r.debugged, pid)
			return exec.Command("true")
		},
		Editor: func(file string, line int) *exec.Cmd {
			r.edited = append(r.edited, file)
			r.line = line
			return exec.Command("true")
		},
		Copy: func(text string) error {
			r.copied = append(r.copied, text)
			return r.copyErr
		},
	}
}

func sampleCrash() *crash.Report {
	r := &crash.Report{
		PID: 42, Signal: "SIGSEGV", Address: "0x0", Access: "read", Task: "lvgl", UptimeMs: 3000,
		Executable: "/x/app", LoadAddress: "0x100000000", ImageSize: 0x10000,
		Frames: []string{"0x100001000", "0x100002000", "0x100003000"},
	}
	r.Resolve("/p", func(string, ...string) ([]byte, error) {
		return []byte(`show_lost_time_card(int) (in app) (/p/components/ui/cards.cpp:412)

clock_screen_update() (in app) (/p/components/ui/clock_screen.cpp:731)

lv_timer_handler (in app) (/p/managed_components/lvgl__lvgl/src/misc/lv_timer.c:90)

`), nil
	})
	return r
}

func crashed(t *testing.T, f *fakeBackend, r *recorder) *Model {
	t.Helper()
	m := running(t, f)
	m.opts.Tools = r.tools()
	serial(m, "I (boot) rotation 0\n")
	feed(m, app.Crashed{Report: sampleCrash(), PID: 42})
	return m
}

func TestTheCrashScreenFollowsTheMockup(t *testing.T) {
	m := crashed(t, newFake(), &recorder{})
	wantScreen(t, m,
		"preflash", "snoozel", "■ crashed", "00:03",
		"memory error on the lvgl task, frozen where it happened",
		"I (boot) rotation 0",
		"Null pointer read in show_lost_time_card",
		"components/ui/cards.cpp:412",
		"› show_lost_time_card", "cards.cpp:412",
		"clock_screen_update", "clock_screen.cpp:731",
		"lv_timer_handler", "lvgl",
		"The device window keeps its last frame. Firmware state is held until you restart.",
		"g open debugger", "e open in editor", "y copy report", "^R rebuild and restart", "^C quit",
	)
	refuseScreen(t, m, "snoozel>")
	fills(t, m, 100, 30)
}

func TestTheCrashScreenFitsTheSmallestWindow(t *testing.T) {
	m := crashed(t, newFake(), &recorder{})
	resize(m, minWidth, minHeight)
	wantScreen(t, m, "Null pointer read", "■ crashed")
	fills(t, m, minWidth, minHeight)
}

func TestAHeadlessCrashHasNoWindowToMention(t *testing.T) {
	f := newFake()
	m := running(t, f)
	m.opts.Headless = true
	feed(m, app.Crashed{Report: sampleCrash(), PID: 42})
	refuseScreen(t, m, "device window")
	wantScreen(t, m, "Firmware state is held until you restart.")
}

func TestGOpensTheDebuggerOnTheFrozenApp(t *testing.T) {
	r := &recorder{}
	m := crashed(t, newFake(), r)
	require.NotNil(t, press(m, "g"))
	assert.Equal(t, []int{42}, r.debugged)
}

func TestEOpensTheEditorAtTheLine(t *testing.T) {
	r := &recorder{}
	m := crashed(t, newFake(), r)
	require.NotNil(t, press(m, "e"))
	assert.Equal(t, []string{"/p/components/ui/cards.cpp"}, r.edited)
	assert.Equal(t, 412, r.line)
}

func TestEWithNoSourceSaysSo(t *testing.T) {
	r := &recorder{}
	m := crashed(t, newFake(), r)
	m.crash.Stack = nil
	assert.Nil(t, press(m, "e"))
	assert.Empty(t, r.edited)
	wantScreen(t, m, "This crash has no source line to open.")
}

func TestYCopiesTheReport(t *testing.T) {
	r := &recorder{}
	m := crashed(t, newFake(), r)
	press(m, "y")
	require.Len(t, r.copied, 1)
	assert.Contains(t, r.copied[0], "Null pointer read in show_lost_time_card")
	wantScreen(t, m, "Report copied.")
}

func TestACopyThatFailsSaysSo(t *testing.T) {
	r := &recorder{copyErr: errors.New("no clipboard")}
	m := crashed(t, newFake(), r)
	press(m, "y")
	wantScreen(t, m, "no clipboard")
}

func TestToolFailuresReachTheScreen(t *testing.T) {
	m := crashed(t, newFake(), &recorder{})
	m.Update(toolDoneMsg{name: "lldb", err: errors.New("no permission")})
	wantScreen(t, m, "lldb: no permission")
}

func TestCtrlRRebuildsFromACrash(t *testing.T) {
	f := newFake()
	m := crashed(t, f, &recorder{})
	before := len(f.runs)
	press(m, "ctrl+r")
	assert.Len(t, f.runs, before+1)
}

func TestTheNextBootLeavesTheCrashBehind(t *testing.T) {
	m := crashed(t, newFake(), &recorder{})
	feed(m, app.Started{Board: modded, PID: 43})
	refuseScreen(t, m, "cards.cpp:412", "g open debugger")
	wantScreen(t, m, "snoozel>")
}

func TestTheFooterIsClickableOnTheCrashScreen(t *testing.T) {
	r := &recorder{}
	m := crashed(t, newFake(), r)
	x, y := find(t, m, "g open debugger")
	require.NotNil(t, click(m, x, y))
	assert.Len(t, r.debugged, 1)
}

func TestAFailedRebuildStaysOnTheCrash(t *testing.T) {
	m := crashed(t, newFake(), &recorder{})
	feed(m, app.Building{Board: modded}, app.BuildOutput{Line: "cc: boom"}, app.BuildFailed{Err: errors.New("the build failed"), AppRunning: true})
	wantScreen(t, m, "■ crashed", "Null pointer read", "the build failed", "The crashed app stays frozen.")
	refuseScreen(t, m, "The old app keeps running.")
}

func TestACrashClosesWhatsOpen(t *testing.T) {
	m := running(t, newFake())
	press(m, "ctrl+d")
	feed(m, app.Crashed{Report: sampleCrash(), PID: 42})
	wantScreen(t, m, "Null pointer read")
	press(m, "ctrl+h")
	wantScreen(t, m, "Null pointer read")
}

func TestAKilledHeldAppCantBeDebugged(t *testing.T) {
	r := &recorder{}
	m := crashed(t, newFake(), r)
	feed(m, app.Exited{Reason: "The app was killed."})
	wantScreen(t, m, "■ crashed", "Null pointer read", "The app was killed.")
	assert.Nil(t, press(m, "g"))
	assert.Empty(t, r.debugged)
	wantScreen(t, m, "The app has gone, so there's nothing to attach to.")
}

func TestTheConsoleScrollsOnTheCrashScreen(t *testing.T) {
	m := crashed(t, newFake(), &recorder{})
	for i := range 80 {
		serial(m, "line "+string(rune('a'+i%26))+"\n")
	}
	press(m, "pgup")
	assert.Positive(t, m.scroll)
}

func TestTheLaunchScreenChoosesASanitizer(t *testing.T) {
	f := newFake()
	m := New(f, options())
	resize(m, 100, 30)
	m.Init()
	wantScreen(t, m, "s sanitizer off")
	press(m, "s")
	wantScreen(t, m, "s sanitizer address")
	press(m, "s")
	wantScreen(t, m, "s sanitizer thread")
	press(m, "s")
	wantScreen(t, m, "s sanitizer off")
	assert.Equal(t, []string{"address", "thread", ""}, f.sanitize)
}

func TestTheToolbarSaysWhichSanitizerRan(t *testing.T) {
	m := running(t, newFake())
	feed(m, app.Started{Board: modded, PID: 43, Sanitizer: "thread"})
	wantScreen(t, m, "thread sanitizer")
}

func TestASanitizerFlagStartsChosen(t *testing.T) {
	opts := options()
	opts.Sanitize = "address"
	m := New(newFake(), opts)
	resize(m, 100, 30)
	wantScreen(t, m, "s sanitizer address")
}
