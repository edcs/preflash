// Package e2e drives the real terminal app, in a real pseudo-terminal, through a virtual
// terminal, so these tests see what a person would. tests/integration/tui.sh sets them up.
package e2e

import (
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	uv "github.com/charmbracelet/ultraviolet"
	"github.com/charmbracelet/x/vt"
	"github.com/creack/pty"
)

const (
	patience     = 30 * time.Second
	quitPatience = 10 * time.Second
)

// terminal is a virtual terminal with a program running in it.
type terminal struct {
	t       *testing.T
	cmd     *exec.Cmd
	pty     *os.File
	screen  *vt.SafeEmulator
	ended   chan struct{} // closed once the program has gone
	waitErr error
}

// open starts the program on a terminal of this size.
func open(t *testing.T, width, height int, name string, args ...string) *terminal {
	t.Helper()
	cmd := exec.Command(name, args...)
	cmd.Env = append(os.Environ(), "TERM=xterm-256color", "COLORTERM=truecolor")
	ptmx, err := pty.StartWithSize(cmd, &pty.Winsize{Cols: uint16(width), Rows: uint16(height)})
	if err != nil {
		t.Fatal(err)
	}
	term := &terminal{
		t: t, cmd: cmd, pty: ptmx,
		screen: vt.NewSafeEmulator(width, height),
		ended:  make(chan struct{}),
	}
	// What the program prints goes to the screen, and what the screen answers, along with
	// any keys and mouse events sent to it, goes back to the program.
	go io.Copy(term.screen, ptmx)
	go io.Copy(ptmx, term.screen)
	go func() {
		term.waitErr = cmd.Wait()
		close(term.ended)
	}()
	t.Cleanup(term.close)
	return term
}

// close quits as a person would, because a program killed outright can't stop its app, and
// the next test needs the project free.
func (term *terminal) close() {
	term.pty.WriteString("\x03")
	select {
	case <-term.ended:
	case <-time.After(quitPatience):
		term.cmd.Process.Kill()
		<-term.ended
	}
	term.pty.Close()
}

// text is what the screen shows, a line per row.
func (term *terminal) text() string {
	width, height := term.screen.Width(), term.screen.Height()
	var out strings.Builder
	for y := range height {
		var row strings.Builder
		for x := 0; x < width; {
			cell := term.screen.CellAt(x, y)
			if cell == nil || cell.Content == "" {
				if cell != nil && cell.Width == 0 {
					x++
					continue
				}
				row.WriteByte(' ')
				x++
				continue
			}
			row.WriteString(cell.Content)
			x += max(cell.Width, 1)
		}
		out.WriteString(strings.TrimRight(row.String(), " "))
		out.WriteByte('\n')
	}
	return out.String()
}

// wait returns once the screen shows every one of texts.
func (term *terminal) wait(texts ...string) string {
	term.t.Helper()
	return term.waitLong(patience, texts...)
}

func (term *terminal) waitLong(limit time.Duration, texts ...string) string {
	term.t.Helper()
	deadline := time.Now().Add(limit)
	for {
		shown := term.text()
		missing := ""
		for _, text := range texts {
			if !strings.Contains(shown, text) {
				missing = text
				break
			}
		}
		if missing == "" {
			return shown
		}
		if time.Now().After(deadline) {
			term.t.Fatalf("the screen never showed %q:\n%s", missing, shown)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

// waitGone returns once the screen shows none of texts.
func (term *terminal) waitGone(texts ...string) {
	term.t.Helper()
	deadline := time.Now().Add(patience)
	for {
		shown := term.text()
		found := ""
		for _, text := range texts {
			if strings.Contains(shown, text) {
				found = text
			}
		}
		if found == "" {
			return
		}
		if time.Now().After(deadline) {
			term.t.Fatalf("the screen still shows %q:\n%s", found, shown)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

// find is the cell where text starts.
func (term *terminal) find(text string) (int, int) {
	term.t.Helper()
	for y, line := range strings.Split(term.text(), "\n") {
		if at := strings.Index(line, text); at >= 0 {
			return len([]rune(line[:at])), y
		}
	}
	term.t.Fatalf("no %q on the screen:\n%s", text, term.text())
	return 0, 0
}

func (term *terminal) typed(text string) { term.screen.SendText(text) }

// key presses a key that isn't text, such as ctrl+r, as bytes a terminal would send.
func (term *terminal) key(bytes string) {
	term.t.Helper()
	if _, err := term.pty.WriteString(bytes); err != nil {
		term.t.Fatal(err)
	}
}

// click sends a left press and release through the emulator, which encodes them only if
// the program really turned on mouse reporting.
func (term *terminal) click(x, y int) {
	term.screen.SendMouse(uv.MouseClickEvent{X: x, Y: y, Button: uv.MouseLeft})
	term.screen.SendMouse(uv.MouseReleaseEvent{X: x, Y: y, Button: uv.MouseLeft})
}

func (term *terminal) wheel(button uv.MouseButton) {
	term.screen.SendMouse(uv.MouseWheelEvent{X: 5, Y: 5, Button: button})
}

// resize changes the terminal's size, as dragging its corner does.
func (term *terminal) resize(width, height int) {
	term.t.Helper()
	term.screen.Resize(width, height)
	if err := pty.Setsize(term.pty, &pty.Winsize{Cols: uint16(width), Rows: uint16(height)}); err != nil {
		term.t.Fatal(err)
	}
}

// waitRedrawn returns once a footer entry sits at the window's right edge, which an old
// frame's doesn't.
func (term *terminal) waitRedrawn(width int) {
	term.t.Helper()
	deadline := time.Now().Add(patience)
	for {
		for _, line := range strings.Split(term.text(), "\n") {
			if strings.HasSuffix(line, "^C quit") && len([]rune(line)) == width-2 {
				return
			}
		}
		if time.Now().After(deadline) {
			term.t.Fatalf("no redraw at width %d:\n%s", width, term.text())
		}
		time.Sleep(20 * time.Millisecond)
	}
}

// exited waits for the program to end and gives its error.
func (term *terminal) exited() error {
	term.t.Helper()
	select {
	case <-term.ended:
		return term.waitErr
	case <-time.After(patience):
		term.t.Fatalf("the program never ended:\n%s", term.text())
		return nil
	}
}

// snapshot prints the screen into the test's output, for a person to read, and saves it
// with its colours if PREFLASH_E2E_SHOTS names a folder.
func (term *terminal) snapshot(name string) {
	term.t.Helper()
	term.t.Logf("--- %s ---\n%s", name, term.text())
	if dir := os.Getenv("PREFLASH_E2E_SHOTS"); dir != "" {
		path := filepath.Join(dir, name+".ansi")
		if err := os.WriteFile(path, []byte(term.screen.Render()), 0o644); err != nil {
			term.t.Fatal(err)
		}
	}
}
