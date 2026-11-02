package e2e

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	uv "github.com/charmbracelet/ultraviolet"
)

// A first build compiles LVGL.
const buildPatience = 10 * time.Minute

const (
	keyEnter     = "\r"
	keyUp        = "\x1b[A"
	keyDown      = "\x1b[B"
	keyBackspace = "\x7f"
	keyEsc       = "\x1b"
	ctrlB        = "\x02"
	ctrlC        = "\x03"
	ctrlL        = "\x0c"
	ctrlR        = "\x12"
)

// setup finds the binary and the project tests/integration/tui.sh prepared.
func setup(t *testing.T) (string, string) {
	t.Helper()
	binary, project := os.Getenv("PREFLASH_E2E_BINARY"), os.Getenv("PREFLASH_E2E_PROJECT")
	if binary == "" || project == "" {
		t.Skip("tests/integration/tui.sh runs these")
	}
	return binary, project
}

func pid(t *testing.T, binary, project string) int {
	t.Helper()
	output, err := exec.Command(binary, "status", "--json", "--project", project).Output()
	if err != nil {
		t.Fatal(err)
	}
	var status struct {
		PID int `json:"pid"`
	}
	if err := json.Unmarshal(output, &status); err != nil {
		t.Fatal(err)
	}
	return status.PID
}

func waitDead(t *testing.T, pid int) {
	t.Helper()
	deadline := time.Now().Add(patience)
	for syscall.Kill(pid, 0) == nil {
		if time.Now().After(deadline) {
			t.Fatalf("the app (pid %d) is still running", pid)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func edit(t *testing.T, path, old, replacement string) {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), old) {
		t.Fatalf("no %q in %s", old, path)
	}
	if err := os.WriteFile(path, []byte(strings.ReplaceAll(string(data), old, replacement)), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestTheWholeApp(t *testing.T) {
	binary, project := setup(t)
	source := filepath.Join(project, "main", "main.cpp")
	term := open(t, 100, 30, binary, "--project", project, "--headless")

	// The launch screen, from the project's own files.
	term.wait("○ not running", "From this project", "› mod, 4B + backlight mod", "Built in",
		"480×480 ST7701", "GT911", "backlight enable on GPIO43", "✓ ESP-IDF project", "✓ LVGL 9", "✓ .preflashrc, 6 shortcuts",
		"↑↓ choose", "enter build and run")
	term.snapshot("launch-100x30")

	term.key(keyEnter)
	term.waitLong(buildPatience, "● running", "fps", "wifi", "1 Ping", "2 Two", "fixture>", "^R rebuild", "^B board", "^L clear", "^C quit")
	term.snapshot("running-100x30")

	// A number key, with the empty input, runs a chip.
	term.typed("1")
	term.wait("fixture> ping", "fixture: pong", "last pong")

	// A real click on a chip, encoded by the terminal because the app asked for the mouse.
	x, y := term.find("2 Two")
	term.click(x+2, y)
	term.wait("fixture> brightness 40", "fixture> ping")

	// A shortcut that asks.
	term.typed("3")
	term.wait("Rotate degrees>", "enter send", "esc cancel")
	term.snapshot("ask-100x30")
	term.key(keyEnter)
	term.wait("fixture> rotate 90", "rotation 90°", "last rotated")
	term.waitGone("Rotate degrees>")

	// Typing, in which number keys are text, then history.
	term.typed("ping 1")
	term.wait("fixture> ping 1")
	for range 6 {
		term.key(keyBackspace)
	}
	term.typed("ping")
	term.key(keyEnter)
	term.key(keyUp)
	term.key(keyEnter)

	// Scrolling back and returning.
	term.typed("5")
	term.waitLong(buildPatience, "fixture: flood done")
	term.wheel(uv.MouseWheelUp)
	term.wait("scrolled back, PgDn returns")
	term.wheel(uv.MouseWheelDown)
	term.waitGone("scrolled back, PgDn returns")

	term.key(ctrlL)
	term.waitGone("fixture: flood done")

	// A restart comes back by itself, as a new boot.
	term.typed("4")
	term.wait("fixture> restart")
	term.wait("rotation 0°", "● running")
	term.snapshot("restarted-100x30")

	// A build that fails leaves the old app running.
	edit(t, source, "ESP_LOGI(kTag, \"pong\");", "ESP_LOGI(kTag, \"pong\"); #error broken on purpose")
	term.key(ctrlR)
	term.waitLong(buildPatience, "The old app keeps running.", "the build failed", "● running")
	term.snapshot("failed-build-100x30")
	term.typed("1")
	term.wait("fixture: pong")

	// One that works replaces it.
	edit(t, source, "ESP_LOGI(kTag, \"pong\"); #error broken on purpose", "ESP_LOGI(kTag, \"pang\");")
	term.key(ctrlR)
	term.waitLong(buildPatience, "◐ building")
	term.waitGone("◐ building")
	term.wait("● running")
	term.typed("1")
	term.wait("fixture: pang")

	// Another board, from the running screen.
	term.key(ctrlB)
	term.wait("From this project", "esc back", "● running")
	term.key(keyEsc)
	term.waitGone("From this project")
	term.key(ctrlB)
	term.wait("From this project")
	term.key(keyDown)
	term.key(keyEnter)
	term.waitGone("From this project")
	term.waitLong(buildPatience, "preflash  "+filepath.Base(project)+"  ESP32-S3-Touch-LCD-4B")
	term.waitGone("backlight mod")
	term.wait("● running", "backlight 100%")

	// A crash keeps what it said, and ^R recovers.
	term.typed("6")
	term.wait("■ crashed", "esp_console_repl_del", "The app died: abort trap.")
	term.snapshot("crashed-100x30")
	term.key(ctrlR)
	term.waitLong(buildPatience, "● running")

	// Nothing running, and back.
	term.typed("@sim quit")
	term.key(keyEnter)
	term.wait("○ stopped", "The app exited.")
	term.snapshot("stopped-100x30")
	term.typed("1")
	term.wait("Nothing is running. ^R builds and runs.")
	term.key(ctrlR)
	term.waitLong(buildPatience, "● running", "fps")

	// Quitting stops the app.
	app := pid(t, binary, project)
	term.key(ctrlC)
	if err := term.exited(); err != nil {
		t.Fatal(err)
	}
	waitDead(t, app)
	if output, err := exec.Command(binary, "status", "--project", project).CombinedOutput(); err == nil {
		t.Fatalf("something still answers: %s", output)
	}
}

func TestEverySize(t *testing.T) {
	binary, project := setup(t)
	term := open(t, 60, 16, binary, "--project", project, "--headless", "--board", "ESP32-S3-Touch-LCD-4B")
	term.waitLong(buildPatience, "● running", "fixture>", "1 Ping", "^C quit")
	term.snapshot("running-60x16")

	term.resize(59, 16)
	term.wait("too small")
	term.snapshot("small-59x16")
	term.resize(80, 24)
	term.waitRedrawn(80)
	term.wait("● running", "1 Ping")
	term.snapshot("running-80x24")
	term.resize(140, 40)
	term.waitRedrawn(140)
	term.wait("● running", "1 Ping")
	term.snapshot("running-140x40")

	term.key(ctrlB)
	term.wait("From this project")
	term.snapshot("launch-140x40")
	term.resize(60, 16)
	term.waitRedrawn(60)
	term.wait("From this project", "enter build and run")
	term.snapshot("launch-60x16")

	app := pid(t, binary, project)
	term.key(ctrlC)
	term.exited()
	waitDead(t, app)
}

func TestClosingTheTerminalStopsTheApp(t *testing.T) {
	binary, project := setup(t)
	term := open(t, 80, 24, binary, "--project", project, "--headless", "--board", "ESP32-S3-Touch-LCD-4B")
	term.waitLong(buildPatience, "● running", "fps")
	app := pid(t, binary, project)

	term.pty.Close()

	waitDead(t, app)
	term.exited()
}

func TestABrokenConfigStopsBeforeTheScreen(t *testing.T) {
	binary, project := setup(t)
	path := filepath.Join(project, ".preflashrc")
	good, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	defer os.WriteFile(path, good, 0o644)
	if err := os.WriteFile(path, append(good, []byte("colour: red\n")...), 0o644); err != nil {
		t.Fatal(err)
	}

	term := open(t, 300, 24, binary, "--project", project, "--headless")

	term.wait(".preflashrc:" + strconv.Itoa(strings.Count(string(good), "\n")+1) + ": unknown key")
	if term.exited() == nil {
		t.Fatal("that should have failed")
	}
}
