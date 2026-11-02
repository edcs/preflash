package tui

import (
	"fmt"
	"strings"
	"testing"

	tea "charm.land/bubbletea/v2"

	"github.com/edcs/preflash/internal/app"
	"github.com/edcs/preflash/internal/client"
)

func i2cRow(seq uint64, data string, injected bool) client.TraceRecord {
	return client.TraceRecord{
		Stream: "i2c", Seq: seq, Us: 251_204_000, Subject: "0x51", Device: "PCF85063", Op: "read",
		Detail: data, Injected: injected,
	}
}

func rows(m *Model, records ...client.TraceRecord) { feed(m, app.TraceRows{Rows: records}) }

func debugging(t *testing.T, f *fakeBackend) *Model {
	t.Helper()
	m := running(t, f)
	press(m, "ctrl+d")
	return m
}

func TestTheFooterOffersDebugAndHardware(t *testing.T) {
	m := running(t, newFake())
	wantScreen(t, m, "^R rebuild", "^D debug", "^H hardware", "^B board", "^L clear")
}

func TestDebugOpensOnI2cAndAsksForTheTrace(t *testing.T) {
	f := newFake()
	m := debugging(t, f)
	wantScreen(t, m, "console", "i2c", "gpio", "tasks", "nvs", "net",
		"time", "addr", "device", "op", "data",
		"tab next view", "/ filter", "space pause", "s save trace", "esc console", "^C quit")
	if len(f.traces) != 1 || !f.traces[0] {
		t.Fatalf("asked for %v", f.traces)
	}
	press(m, "esc")
	wantScreen(t, m, "^R rebuild")
	if len(f.traces) != 2 || f.traces[1] {
		t.Fatalf("asked for %v", f.traces)
	}
}

func TestI2cRowsShowTheMockupsColumns(t *testing.T) {
	m := debugging(t, newFake())
	rows(m, i2cRow(1, "04: 12 11 18 31 06 10 26", false))
	wantScreen(t, m, "04:11.204", "0x51", "PCF85063", "read", "04: 12 11 18 31 06 10 26")
	fills(t, m, 100, 30)
}

func TestInjectedRowsAreAmber(t *testing.T) {
	m := debugging(t, newFake())
	rows(m, i2cRow(1, "NACK (injected)", true))
	if !strings.Contains(m.View().Content, styleAmber.Render(fit("NACK (injected)", 15))) &&
		!strings.Contains(m.View().Content, "\x1b[38;2;242;184;75m") {
		t.Errorf("no amber on the screen:\n%q", m.View().Content)
	}
}

func TestTabWalksTheViews(t *testing.T) {
	m := debugging(t, newFake())
	rows(m, client.TraceRecord{Stream: "gpio", Us: 1000, Subject: "gpio4", Op: "level", Detail: "high"})
	refuseScreen(t, m, "gpio4")
	press(m, "tab")
	wantScreen(t, m, "gpio4", "pin", "high")
	press(m, "tab")
	press(m, "tab")
	press(m, "tab")
	press(m, "tab")
	serial(m, "hello from firmware\n")
	wantScreen(t, m, "hello from firmware")
	press(m, "tab")
	wantScreen(t, m, "addr")
}

func TestTasksShowsTheTableAndWhatHappened(t *testing.T) {
	f := newFake()
	m := debugging(t, f)
	press(m, "tab")
	press(m, "tab")
	feed(m, app.Tasks{Tasks: []client.Task{{Name: "main", State: "blocked", Stack: 3584}}})
	rows(m, client.TraceRecord{Stream: "task", Us: 2000, Subject: "worker", Op: "create", Detail: "stack 2048"})
	wantScreen(t, m, "state", "stack (declared)", "main", "blocked", "3584", "worker", "create")
	fills(t, m, 100, 30)
	before := f.tasks
	m.Update(tickMsg{})
	if f.tasks != before+1 {
		t.Error("the table wasn't refreshed")
	}
}

func TestNvsShowsKeysAndSizes(t *testing.T) {
	f := newFake()
	m := debugging(t, f)
	press(m, "tab")
	press(m, "tab")
	press(m, "tab")
	feed(m, app.NVS{Namespaces: []client.Namespace{{Name: "wifi", Keys: []client.NVSKey{{Key: "ssid", Size: 6}}}}})
	wantScreen(t, m, "namespace", "wifi", "ssid", "6")
	before := f.nvs
	m.Update(tickMsg{})
	if f.nvs != before+1 {
		t.Error("the table wasn't refreshed")
	}
}

func TestFilterKeepsMatchingRows(t *testing.T) {
	m := debugging(t, newFake())
	rows(m, i2cRow(1, "first", false), client.TraceRecord{Stream: "i2c", Subject: "0x6b", Device: "QMI8658", Op: "read", Detail: "second"})
	press(m, "/")
	typed(m, "qmi")
	press(m, "enter")
	wantScreen(t, m, "second", "filter qmi")
	refuseScreen(t, m, "first")
	press(m, "/")
	press(m, "esc")
	wantScreen(t, m, "first", "second")
	wantScreen(t, m, "esc console")
}

func TestFilterKeysDontActOnTheView(t *testing.T) {
	m := debugging(t, newFake())
	press(m, "/")
	typed(m, "s ")
	if m.debug.paused {
		t.Error("a space in the filter paused the view")
	}
	wantScreen(t, m, "s ")
}

func TestPauseHoldsTheViewAndCatchesUp(t *testing.T) {
	m := debugging(t, newFake())
	rows(m, i2cRow(1, "first", false))
	press(m, " ")
	wantScreen(t, m, "paused")
	rows(m, i2cRow(2, "second", false))
	refuseScreen(t, m, "second")
	press(m, " ")
	wantScreen(t, m, "first", "second")
	refuseScreen(t, m, "paused")
}

func TestSavingSaysWhereItWent(t *testing.T) {
	f := newFake()
	m := debugging(t, f)
	press(m, "s")
	if f.saves != 1 {
		t.Fatal("nothing was saved")
	}
	feed(m, app.TraceSaved{Path: "/state/trace.jsonl", Records: 12})
	wantScreen(t, m, "12 records saved to /state/trace.jsonl")
	feed(m, app.SendFailed{Err: fmt.Errorf("disk full")})
	wantScreen(t, m, "disk full")
}

func TestANewConnectionReplacesTheRows(t *testing.T) {
	m := debugging(t, newFake())
	rows(m, i2cRow(1, "old boot", false))
	feed(m, app.TraceRows{Rows: []client.TraceRecord{i2cRow(1, "new boot", false)}, Reset: true})
	wantScreen(t, m, "new boot")
	refuseScreen(t, m, "old boot")
}

func TestRowsAreBounded(t *testing.T) {
	m := debugging(t, newFake())
	for i := range keptRows + 10 {
		rows(m, i2cRow(uint64(i), "x", false))
	}
	if got := len(m.debug.rows["i2c"]); got != keptRows {
		t.Errorf("%d rows kept", got)
	}
}

func TestPgUpScrollsTheRows(t *testing.T) {
	m := debugging(t, newFake())
	for i := range 100 {
		rows(m, i2cRow(uint64(i), fmt.Sprintf("row-%03d", i), false))
	}
	wantScreen(t, m, "row-099")
	press(m, "pgup")
	refuseScreen(t, m, "row-099")
	press(m, "pgdown")
	wantScreen(t, m, "row-099")
}

func TestFirmwareTextCantMoveTheCursor(t *testing.T) {
	m := debugging(t, newFake())
	rows(m, i2cRow(1, "bad\x1b[2Jtext", false))
	if strings.Contains(m.View().Content, "\x1b[2J") {
		t.Error("an escape got through")
	}
}

func TestDebugDrawsAtTheSmallestWindow(t *testing.T) {
	m := debugging(t, newFake())
	resize(m, minWidth, minHeight)
	rows(m, i2cRow(1, strings.Repeat("ab ", 40), false))
	fills(t, m, minWidth, minHeight)
	press(m, "tab")
	press(m, "tab")
	fills(t, m, minWidth, minHeight)
}

func TestAClickOnATabChoosesIt(t *testing.T) {
	m := debugging(t, newFake())
	x, y := find(t, m, "gpio")
	click(m, x+1, y)
	wantScreen(t, m, "pin")
}

func TestAClickOnTheFooterLeavesDebug(t *testing.T) {
	m := debugging(t, newFake())
	x, y := find(t, m, "esc console")
	click(m, x, y)
	wantScreen(t, m, "^R rebuild")
}

func hardwareItems() []client.HardwareItem {
	option := func(pairs ...string) []client.HardwareOption {
		var out []client.HardwareOption
		for i := 0; i < len(pairs); i += 2 {
			out = append(out, client.HardwareOption{Value: pairs[i], Label: pairs[i+1]})
		}
		return out
	}
	return []client.HardwareItem{
		{Name: "rotation", Title: "Rotation", Applies: "now", Value: "0", Next: "0", Options: option("0", "0°", "90", "90°", "180", "180°", "270", "270°")},
		{Name: "usb", Title: "USB power", Applies: "now", Value: "plugged", Next: "plugged", Options: option("plugged", "plugged in", "unplugged", "unplugged")},
		{Name: "wifi", Title: "Wi-Fi", Group: "Network", Applies: "now", Value: "mac", Next: "mac", Options: option("mac", "Mac's connection")},
		{Name: "internet", Title: "Internet", Group: "Network", Applies: "now", Value: "real", Next: "real", Options: option("real", "real", "offline", "offline")},
		{Name: "rtc", Title: "RTC", Group: "Break something", Applies: "boot", Value: "healthy", Next: "flat", Options: option("healthy", "healthy", "flat", "flat backup cell")},
		{Name: "wifi_join", Title: "Wi-Fi join", Group: "Break something", Applies: "now", Value: "succeeds", Next: "succeeds", Options: option("succeeds", "succeeds", "fails", "fails")},
	}
}

func panelOpen(t *testing.T, f *fakeBackend) *Model {
	t.Helper()
	m := running(t, f)
	press(m, "ctrl+h")
	feed(m, app.Hardware{Items: hardwareItems()})
	return m
}

func TestThePanelShowsTheMockupsCopy(t *testing.T) {
	f := newFake()
	m := panelOpen(t, f)
	if f.hardware != 1 {
		t.Errorf("asked for the hardware %d times", f.hardware)
	}
	wantScreen(t, m, "Hardware", "Rotation", "0°", "90° 180° 270°", "USB power", "plugged in",
		"Network", "Wi-Fi", "Mac's connection", "Internet", "real", "Break something", "RTC",
		"Wi-Fi join", "↑↓ choose", "←→ change", "esc close")
	fills(t, m, 100, 30)
	refuseScreen(t, m, "^C quit", "snoozel> ")
}

func TestThePanelSaysWhatWaitsForTheNextBoot(t *testing.T) {
	m := panelOpen(t, newFake())
	wantScreen(t, m, "flat backup cell (next boot)", "Applied on the next boot. ^R restarts now.")
}

func TestThePanelSaysWhenAChangeTookEffect(t *testing.T) {
	m := panelOpen(t, newFake())
	items := hardwareItems()
	items[4].Next = "healthy"
	feed(m, app.Hardware{Items: items})
	wantScreen(t, m, "Applied now.")
	refuseScreen(t, m, "Applied on the next boot")
	for range 4 {
		press(m, "down") // past Wi-Fi, which has nothing to choose
	}
	wantScreen(t, m, "Applied now.")
	press(m, "up")
	wantScreen(t, m, "Applied on the next boot. ^R restarts now.")
}

func TestArrowsChooseAndChange(t *testing.T) {
	f := newFake()
	m := panelOpen(t, f)
	press(m, "right")
	press(m, "down")
	press(m, "right")
	want := [][2]string{{"rotation", "90"}, {"usb", "unplugged"}}
	if fmt.Sprint(f.changes) != fmt.Sprint(want) {
		t.Errorf("changes %v", f.changes)
	}
}

func TestChoosingSkipsWhatHasNoChoice(t *testing.T) {
	f := newFake()
	m := panelOpen(t, f)
	press(m, "down")
	press(m, "down")
	press(m, "right")
	if fmt.Sprint(f.changes) != fmt.Sprint([][2]string{{"internet", "offline"}}) {
		t.Errorf("changes %v", f.changes)
	}
}

func TestChangingWraps(t *testing.T) {
	f := newFake()
	m := panelOpen(t, f)
	press(m, "left")
	if fmt.Sprint(f.changes) != fmt.Sprint([][2]string{{"rotation", "270"}}) {
		t.Errorf("changes %v", f.changes)
	}
}

func TestChangingStartsFromWhatTheNextBootGets(t *testing.T) {
	f := newFake()
	m := panelOpen(t, f)
	for range 3 {
		press(m, "down")
	}
	press(m, "right")
	if fmt.Sprint(f.changes) != fmt.Sprint([][2]string{{"rtc", "healthy"}}) {
		t.Errorf("changes %v", f.changes)
	}
}

func TestARefusedChangeSaysWhy(t *testing.T) {
	m := panelOpen(t, newFake())
	feed(m, app.HardwareFailed{Err: fmt.Errorf("with no USB and no battery, the board has no power")})
	wantScreen(t, m, "with no USB and no battery")
	press(m, "down")
	refuseScreen(t, m, "with no USB and no battery")
}

func TestPanelKeysDontReachTheInput(t *testing.T) {
	m := panelOpen(t, newFake())
	press(m, "1")
	press(m, "a")
	press(m, "esc")
	if m.input.Value() != "" {
		t.Errorf("the input holds %q", m.input.Value())
	}
	wantScreen(t, m, "^R rebuild")
}

func TestTheControlRRestartsFromThePanel(t *testing.T) {
	f := newFake()
	m := panelOpen(t, f)
	before := len(f.runs)
	press(m, "ctrl+r")
	if len(f.runs) != before+1 {
		t.Error("^R did nothing")
	}
}

func TestThePanelRefreshes(t *testing.T) {
	f := newFake()
	m := panelOpen(t, f)
	m.Update(tickMsg{})
	if f.hardware != 2 {
		t.Errorf("asked %d times", f.hardware)
	}
}

func TestThePanelFitsTheSmallestWindow(t *testing.T) {
	m := panelOpen(t, newFake())
	resize(m, minWidth, minHeight)
	fills(t, m, minWidth, minHeight)
	for range 4 {
		press(m, "down")
	}
	wantScreen(t, m, "Wi-Fi join")
	fills(t, m, minWidth, minHeight)
}

func TestThePanelWithNothingRunningSaysSo(t *testing.T) {
	m := running(t, newFake())
	feed(m, app.Exited{Reason: "The app exited."})
	press(m, "ctrl+h")
	wantScreen(t, m, "Nothing is running. ^R builds and runs.")
	press(m, "esc")
	wantScreen(t, m, "^R rebuild")
}

func TestKeysStillGoToTheInputWithTheViewsShut(t *testing.T) {
	m := running(t, newFake())
	var _ tea.Model = m
	typed(m, "abc")
	if m.input.Value() != "abc" {
		t.Errorf("the input holds %q", m.input.Value())
	}
}
