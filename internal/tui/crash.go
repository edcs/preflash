package tui

import (
	"os/exec"
	"strings"

	tea "charm.land/bubbletea/v2"
	"github.com/charmbracelet/x/ansi"

	"github.com/edcs/preflash/internal/app"
)

// Tools are what the crash screen hands the terminal to.
type Tools struct {
	Debugger func(pid int) *exec.Cmd
	Editor   func(file string, line int) *exec.Cmd
	Copy     func(text string) error
}

// toolDoneMsg says a program the terminal was handed to has finished.
type toolDoneMsg struct {
	name string
	err  error
}

// crashRows is the most of the stack shown at once, however tall the window.
const crashRows = 8

func (m *Model) crashed(e app.Crashed) {
	m.endLine()
	m.state = stateCrashed
	m.crash, m.crashPID, m.crashGone = e.Report, e.PID, false
	m.reason = ""
	m.cancelAsk()
	if m.debug != nil {
		m.closeDebug()
	}
	m.panel = nil
}

func (m *Model) crashKey(key string) tea.Cmd {
	switch key {
	case "g":
		return m.openDebugger()
	case "e":
		return m.openEditor()
	case "y":
		m.copyReport()
	case "ctrl+r":
		m.rebuild()
	case "pgup":
		m.scrollBy(max(m.consoleHeight()-1, 1))
	case "pgdown":
		m.scrollBy(-max(m.consoleHeight()-1, 1))
	}
	return nil
}

func runTool(name string, cmd *exec.Cmd) tea.Cmd {
	return tea.ExecProcess(cmd, func(err error) tea.Msg { return toolDoneMsg{name, err} })
}

func (m *Model) openDebugger() tea.Cmd {
	if m.crashGone {
		m.say("The app has gone, so there's nothing to attach to.", lineWarn)
		return nil
	}
	return runTool("lldb", m.opts.Tools.Debugger(m.crashPID))
}

func (m *Model) openEditor() tea.Cmd {
	if m.crash.At >= len(m.crash.Stack) || m.crash.Stack[m.crash.At].File == "" {
		m.say("This crash has no source line to open.", lineWarn)
		return nil
	}
	frame := m.crash.Stack[m.crash.At]
	return runTool("editor", m.opts.Tools.Editor(frame.File, frame.Line))
}

func (m *Model) copyReport() {
	if err := m.opts.Tools.Copy(m.crash.Text()); err != nil {
		m.say("Couldn't copy the report: "+err.Error(), lineBad)
		return
	}
	m.say("Report copied.", lineDim)
}

func (m *Model) crashHint() string {
	switch {
	case m.crashGone:
		return "The app has stopped. ^R builds and runs."
	case m.opts.Headless:
		return "Firmware state is held until you restart."
	}
	return "The device window keeps its last frame. Firmware state is held until you restart."
}

// crashBox is the report, as rows the width of the window.
func (m *Model) crashBox(height int) []string {
	inner := m.width - 2*sidePad - 2 - 2*boxPad
	line := func(s string) string {
		return strings.Repeat(" ", sidePad) + styleBadEdge.Render("│") + strings.Repeat(" ", boxPad) +
			fit(s, inner) + strings.Repeat(" ", boxPad) + styleBadEdge.Render("│")
	}
	rule := func(left, right string) string {
		return strings.Repeat(" ", sidePad) + styleBadEdge.Render(left+strings.Repeat("─", inner+2*boxPad)+right)
	}
	body := []string{styleBadBold.Render(ansi.Truncate(cleanText(m.crash.Title), inner, "…"))}
	if m.crash.Location != "" {
		body = append(body, styleDim.Render(ansi.Truncate(cleanText(m.crash.Location), inner, "…")))
	}
	// The stack gets what's left, down to its first row.
	rows := m.crash.Rows()
	room := min(crashRows, max(height-2-len(body)-1, 1))
	if len(rows) > 0 {
		body = append(body, "")
	}
	for i, row := range rows[:min(room, len(rows))] {
		mark := styleText.Render("  ")
		if i == 0 {
			mark = styleBad.Render("›") + " "
		}
		text := cleanText(row.Function)
		where := cleanText(row.Where)
		name := ansi.Truncate(text, max(inner-2-3-ansi.StringWidth(where), 1), "…")
		body = append(body, mark+styleText.Render(name)+"   "+styleDim.Render(where))
	}
	out := make([]string, 0, len(body)+2)
	out = append(out, rule("╭", "╮"))
	for _, row := range body {
		out = append(out, line(row))
	}
	return append(out, rule("╰", "╯"))
}

// crashBodyRows is what the crash screen has between its toolbar and its last two rows.
func (m *Model) crashBodyRows() int { return m.height - toolbarRows - 2 }

// fittedCrashBox leaves the console at least a row.
func (m *Model) fittedCrashBox() []string {
	room := m.crashBodyRows() - 1
	box := m.crashBox(room)
	return box[:min(len(box), room)]
}

func (m *Model) crashView() string {
	rows := make([]string, 0, m.height)
	rows = append(rows, m.toolbarTop(), m.toolbarBottom())
	rows = append(rows, m.consoleView()...)
	rows = append(rows, m.fittedCrashBox()...)
	rows = append(rows, indent(styleDim.Render(m.crashHint()), m.width), m.footerRow())
	return strings.Join(rows, "\n")
}
