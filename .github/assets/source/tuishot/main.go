// Command tuishot runs the terminal app on examples/demo in a real pseudo-terminal, reads its
// screen through a virtual terminal, as tests/e2e does, and saves each screen as a script that
// writes it into a page. capture.sh runs it.
package main

import (
	"fmt"
	"html"
	"image/color"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	uv "github.com/charmbracelet/ultraviolet"
	"github.com/charmbracelet/x/vt"
	"github.com/creack/pty"
)

const (
	columns  = 90
	rows     = 32
	patience = 10 * time.Minute

	heldAt = "2026-07-14T09:41:00Z"
	ctrlC  = "\x03"
	ctrlD  = "\x04"
	ctrlL  = "\x0c"
)

type terminal struct {
	pty    *os.File
	screen *vt.SafeEmulator
}

func open(name string, args ...string) *terminal {
	cmd := exec.Command(name, args...)
	cmd.Env = append(os.Environ(), "TERM=xterm-256color", "COLORTERM=truecolor")
	ptmx, err := pty.StartWithSize(cmd, &pty.Winsize{Cols: columns, Rows: rows})
	if err != nil {
		log.Fatal(err)
	}
	term := &terminal{pty: ptmx, screen: vt.NewSafeEmulator(columns, rows)}
	go io.Copy(term.screen, ptmx)
	go io.Copy(ptmx, term.screen)
	return term
}

func (term *terminal) key(keys string) {
	if _, err := term.pty.WriteString(keys); err != nil {
		log.Fatal(err)
	}
}

func (term *terminal) line(text string) {
	term.key(text)
	time.Sleep(100 * time.Millisecond)
	term.key("\r")
}

func (term *terminal) text() string {
	var out strings.Builder
	for y := range rows {
		for x := range columns {
			if cell := term.screen.CellAt(x, y); cell != nil {
				out.WriteString(cell.Content)
			}
		}
		out.WriteByte('\n')
	}
	return out.String()
}

// wait returns once the screen shows every one of texts.
func (term *terminal) wait(texts ...string) {
	deadline := time.Now().Add(patience)
	for {
		shown := term.text()
		found := true
		for _, text := range texts {
			found = found && strings.Contains(shown, text)
		}
		if found {
			time.Sleep(600 * time.Millisecond)
			return
		}
		if time.Now().After(deadline) {
			log.Fatalf("the screen never showed %q:\n%s", texts, shown)
		}
		time.Sleep(50 * time.Millisecond)
	}
}

func hex(c color.Color, fallback string) string {
	if c == nil {
		return fallback
	}
	r, g, b, _ := c.RGBA()
	return fmt.Sprintf("#%02x%02x%02x", r>>8, g>>8, b>>8)
}

// css is a cell's look, with reverse video and faint text resolved.
func css(style uv.Style) string {
	fg, bg := hex(style.Fg, ""), hex(style.Bg, "")
	if style.Attrs&uv.AttrReverse != 0 {
		fg, bg = bg, fg
		if fg == "" {
			fg = "var(--surface)"
		}
		if bg == "" {
			bg = "var(--text)"
		}
	}
	var out []string
	if fg != "" {
		out = append(out, "color:"+fg)
	}
	if bg != "" {
		out = append(out, "background:"+bg)
	}
	if style.Attrs&uv.AttrBold != 0 {
		out = append(out, "font-weight:700")
	}
	if style.Attrs&uv.AttrFaint != 0 {
		out = append(out, "opacity:.6")
	}
	if style.Attrs&uv.AttrItalic != 0 {
		out = append(out, "font-style:italic")
	}
	return strings.Join(out, ";")
}

// save writes the screen as a script that puts a <pre> where the page loads it.
func (term *terminal) save(path string) {
	var out strings.Builder
	out.WriteString(`<pre class="screen">`)
	for y := range rows {
		look, run := "", ""
		flush := func() {
			if run == "" {
				return
			}
			if look == "" {
				out.WriteString(html.EscapeString(run))
			} else {
				fmt.Fprintf(&out, `<span style="%s">%s</span>`, look, html.EscapeString(run))
			}
			run = ""
		}
		for x := 0; x < columns; {
			cell := term.screen.CellAt(x, y)
			content, style, width := " ", uv.Style{}, 1
			if cell != nil {
				style = cell.Style
				width = max(cell.Width, 1)
				if cell.Content != "" {
					content = cell.Content
				}
			}
			if here := css(style); here != look {
				flush()
				look = here
			}
			run += content
			x += width
		}
		flush()
		out.WriteByte('\n')
	}
	out.WriteString(`</pre>`)
	script := fmt.Sprintf("document.currentScript.insertAdjacentHTML('beforebegin', %q);\n", out.String())
	if err := os.WriteFile(path, []byte(script), 0o644); err != nil {
		log.Fatal(err)
	}
	fmt.Println("wrote", path)
}

func main() {
	if len(os.Args) != 4 {
		log.Fatal("usage: tuishot <preflash> <project> <folder>")
	}
	binary, project, folder := os.Args[1], os.Args[2], os.Args[3]
	cli := func(args ...string) {
		args = append(args[:1:1], append([]string{"--project", project}, args[1:]...)...)
		if output, err := exec.Command(binary, args...).CombinedOutput(); err != nil {
			log.Fatalf("preflash %s: %v\n%s", strings.Join(args, " "), err, output)
		}
	}

	term := open(binary, "--project", project, "--headless")
	term.wait("demo>", "demo: ready on")

	// The running screen, with the clock held, and the panel in the same moment. The boot on
	// show is the second, because the first has already reached the internet.
	cli("hardware", "internet", "offline")
	term.wait("network: fetch")
	term.key(ctrlL)
	time.Sleep(200 * time.Millisecond)
	term.line("restart")
	term.wait("boot 2", "network: fetch")
	term.line("freeze " + heldAt)
	term.wait("clock=held")
	term.key("1")
	term.wait("sound=")
	term.save(filepath.Join(folder, "tui-running.js"))
	time.Sleep(time.Second)
	cli("shot", filepath.Join(folder, "4b-home.png"))

	// The I2C trace, with the RTC forced off the bus.
	term.key(ctrlD)
	term.wait("tab next view")
	cli("hardware", "i2c", "pcf85063")
	term.wait("NACK (injected)")
	time.Sleep(3 * time.Second) // fills the view
	term.key(" ")
	time.Sleep(500 * time.Millisecond)
	term.save(filepath.Join(folder, "tui-debug.js"))
	cli("hardware", "i2c", "healthy")
	term.key(" ")
	term.key("\x1b")
	time.Sleep(500 * time.Millisecond)

	// The crash screen, from a deliberate null pointer read.
	term.line("crash null")
	term.wait("Null pointer read", "mishaps.cpp")
	term.save(filepath.Join(folder, "tui-crash.js"))
	term.key(ctrlC)
	time.Sleep(2 * time.Second)
}
