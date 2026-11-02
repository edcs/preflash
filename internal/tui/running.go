package tui

import (
	"fmt"
	"slices"
	"strconv"
	"strings"
	"time"

	"github.com/charmbracelet/x/ansi"
)

const (
	// toolbarRows sits above the console.
	toolbarRows = 2
	// fieldGap and footerGap are the spaces between toolbar fields and footer entries.
	fieldGap, footerGap = "   ", "   "
	// fieldValueWidth limits a captured value, so one field can't push the rest out.
	fieldValueWidth = 24
	// unknown fills a field nothing has said yet.
	unknown = "-"
)

// builtInFields are the toolbar values Preflash knows by itself, added after the config's
// fields unless the config places them.
var builtInFields = []string{"backlight", "rotation", "wifi"}

func (m *Model) runningView() string {
	rows := make([]string, 0, m.height)
	rows = append(rows, m.toolbarTop(), m.toolbarBottom())
	rows = append(rows, m.consoleView()...)
	rows = append(rows, m.ruleRow())
	rows = append(rows, m.chipRows()...)
	rows = append(rows, m.inputRow(), m.footerRow())
	return strings.Join(rows, "\n")
}

func (m *Model) toolbarTop() string {
	left := []seg{
		{styleKey, "preflash"}, {styleDim, "  "},
		{styleStrong, m.opts.Name}, {styleDim, "  "},
		{styleDim, boardSummary(m.board)},
	}
	right := []seg{m.stateMark()}
	if m.reported && m.appUp() {
		right = append(right, seg{styleDim, "  " + clock(m.status.UptimeMs)})
	}
	return bar(left, right, m.width)
}

func (m *Model) stateMark() seg {
	if m.building {
		return seg{styleAmber, "◐ building"}
	}
	switch m.state {
	case stateStarting:
		return seg{styleAmber, "◐ starting"}
	case stateRunning:
		return seg{styleAccent, "● running"}
	case stateRestarting:
		return seg{styleAmber, "◐ restarting"}
	case stateStopped:
		return seg{styleDim, "○ stopped"}
	case stateCrashed:
		return seg{styleBadBold, "■ crashed"}
	case stateBuildFailed:
		return seg{styleBadBold, "■ build failed"}
	}
	return seg{styleDim, "○ not running"}
}

// clock shows a time like the mockup's 04:12, with hours once there are any.
func clock(ms int64) string {
	d := time.Duration(ms) * time.Millisecond
	seconds := int(d.Seconds())
	if hours := seconds / 3600; hours > 0 {
		return fmt.Sprintf("%d:%02d:%02d", hours, seconds/60%60, seconds%60)
	}
	return fmt.Sprintf("%02d:%02d", seconds/60, seconds%60)
}

func (m *Model) toolbarBottom() string {
	if !m.appUp() {
		return bar([]seg{{styleDim, m.reason}}, nil, m.width)
	}
	var left []seg
	for i, name := range m.fieldNames() {
		if i > 0 {
			left = append(left, seg{styleDim, fieldGap})
		}
		left = append(left, seg{styleDim, name + " "}, seg{styleText, m.fieldValue(name)})
	}
	var right []seg
	if m.reported {
		right = []seg{{styleDim, fmt.Sprintf("%d fps", m.status.FPS)}}
	}
	return bar(left, right, m.width)
}

// fieldNames puts the config's fields first, in its order, then any built-in it didn't place.
func (m *Model) fieldNames() []string {
	var names []string
	for _, field := range m.opts.Config.Status {
		names = append(names, field.Name)
	}
	for _, name := range builtInFields {
		if !slices.Contains(names, name) {
			names = append(names, name)
		}
	}
	return names
}

func (m *Model) fieldValue(name string) string {
	for _, field := range m.opts.Config.Status {
		if field.Name == name && field.Pattern != nil {
			if value, ok := m.captures[name]; ok && value != "" {
				return ansi.Truncate(value, fieldValueWidth, "…")
			}
			return unknown
		}
	}
	if !m.reported {
		return unknown
	}
	switch name {
	case "backlight":
		return strconv.Itoa(m.status.Backlight) + "%"
	case "rotation":
		return strconv.Itoa(m.status.Rotation) + "°"
	case "wifi":
		// Wi-Fi here is the simulated kind, whatever the firmware believes.
		if m.status.WiFi == "connected" {
			return "simulated"
		}
		return m.status.WiFi
	}
	return unknown
}

func (m *Model) consoleWidth() int { return max(m.width-2*sidePad, 1) }

// consoleHeight is what's left once everything else has its rows.
func (m *Model) consoleHeight() int {
	return max(m.height-toolbarRows-1-len(m.chipRows())-2, 1)
}

func (m *Model) consoleView() []string {
	height := m.consoleHeight()
	rows, _ := m.console.rows(m.consoleWidth(), height, m.scroll)
	out := make([]string, height-len(rows), height)
	for _, r := range rows {
		out = append(out, strings.Repeat(" ", sidePad)+rowStyle(r.kind).Render(r.text))
	}
	return out
}

func rowStyle(kind lineKind) (style segStyle) {
	switch kind {
	case lineDim:
		return styleDim
	case lineWarn:
		return styleAmber
	case lineBad:
		return styleBad
	case lineEcho:
		return styleStrong
	}
	return styleText
}

func (m *Model) ruleRow() string {
	if m.scroll == 0 {
		return styleRule.Render(strings.Repeat("─", m.width))
	}
	hint := " scrolled back, PgDn returns "
	return styleRule.Render(strings.Repeat("─", max(m.width-ansi.StringWidth(hint)-sidePad, 0))) +
		styleAmber.Render(hint) + styleRule.Render(strings.Repeat("─", sidePad))
}

// chip is where one shortcut sits, in cells across and rows down the chip area.
type chip struct {
	index, x0, x1, row int
	key, label         string
}

// chipLayout flows the chips across the window, wrapping as needed, and leaves out what
// doesn't fit a quarter of its height. Number keys still reach those.
func (m *Model) chipLayout() []chip {
	var out []chip
	x, row := sidePad, 0
	limit := m.width - sidePad
	for i, shortcut := range m.opts.Config.Shortcuts {
		label := ansi.Truncate(cleanText(shortcut.Label), max(limit-sidePad-4, 1), "…")
		key := ""
		if i < numberedShortcuts {
			key = strconv.Itoa(i + 1)
		}
		width := ansi.StringWidth(label) + 2
		if key != "" {
			width += len(key) + 1
		}
		if x > sidePad && x+width > limit {
			x = sidePad
			row++
		}
		if row >= max(m.height/4, 1) {
			break
		}
		out = append(out, chip{index: i, x0: x, x1: x + width, row: row, key: key, label: label})
		x += width + 1
	}
	return out
}

func (m *Model) chipRows() []string {
	var rows []strings.Builder
	x := 0
	for _, c := range m.chipLayout() {
		if c.row >= len(rows) {
			rows = append(rows, strings.Builder{})
			x = 0
		}
		current := &rows[c.row]
		current.WriteString(strings.Repeat(" ", c.x0-x))
		segs := []seg{{styleText, " "}, {styleKey, c.key}, {styleText, " " + c.label + " "}}
		if c.key == "" {
			segs = []seg{{styleText, " " + c.label + " "}}
		}
		current.WriteString(renderOn(segs, &surfaceStyle))
		x = c.x1
	}
	out := make([]string, len(rows))
	for i := range rows {
		out[i] = rows[i].String()
	}
	return out
}

func (m *Model) inputLabel() string {
	if m.ask != nil {
		return cleanText(m.ask.shortcut.Label) + " " + m.ask.shortcut.Ask[m.ask.at].Name + ">"
	}
	return cleanText(m.prompt())
}

func (m *Model) sizeInput() {
	m.input.SetWidth(max(m.width-2*sidePad-ansi.StringWidth(m.inputLabel())-2, 1))
}

func (m *Model) inputRow() string {
	return indent(styleKey.Render(m.inputLabel())+" "+m.input.View(), m.width)
}

// footerEntry is a key the footer shows, which a click also presses.
type footerEntry struct {
	key, label string
	act        func()
}

// footerEntries gives the entries at the left, and the one at the right.
func (m *Model) footerEntries() ([]footerEntry, footerEntry) {
	quit := footerEntry{key: "^C", label: "quit"}
	switch {
	case m.picking:
		left := []footerEntry{
			{key: "↑↓", label: "choose"},
			{key: "enter", label: "build and run", act: m.launchEnter},
		}
		if m.board.Name != "" {
			left = append(left, footerEntry{key: "esc", label: "back", act: m.closePicker})
		}
		return left, quit
	case m.ask != nil:
		return []footerEntry{
			{key: "enter", label: "send", act: m.enter},
			{key: "esc", label: "cancel", act: m.cancelAsk},
		}, quit
	}
	return []footerEntry{
		{key: "^R", label: "rebuild", act: m.rebuild},
		{key: "^B", label: "board", act: m.openPicker},
		{key: "^L", label: "clear", act: m.clear},
	}, quit
}

func entryWidth(e footerEntry) int {
	return ansi.StringWidth(e.key) + 1 + ansi.StringWidth(e.label)
}

func (m *Model) footerRow() string {
	entries, right := m.footerEntries()
	var left []seg
	for i, entry := range entries {
		if i > 0 {
			left = append(left, seg{styleDim, footerGap})
		}
		left = append(left, seg{styleText, entry.key}, seg{styleDim, " " + entry.label})
	}
	return bar(left, []seg{{styleText, right.key}, {styleDim, " " + right.label}}, m.width)
}
