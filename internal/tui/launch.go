package tui

import (
	"fmt"
	"slices"
	"strconv"
	"strings"

	tea "charm.land/bubbletea/v2"
	"github.com/charmbracelet/x/ansi"

	"github.com/edcs/preflash/internal/board"
	"github.com/edcs/preflash/internal/project"
)

// projectChoice is short for a row's data.
type projectChoice = project.Choice

const (
	// columnGap sits between the launch screen's two columns.
	columnGap = 4
	// roomyHeight is the height from which the launch screen gets its blank rows.
	roomyHeight = 24
	// detailLabelWidth is the width of the labels beside a board's details.
	detailLabelWidth = 11
	// stockFamily is left off the name of a board's base where space is tight.
	stockFamily = "ESP32-S3-Touch-LCD-"
)

// chipRoles names what a chip is for, in the order the details list them.
var chipRoles = []struct{ label, chips string }{
	{"touch", "gt911 axs15231b"},
	{"rtc", "pcf85063"},
	{"motion", "qmi8658"},
	{"power", "axp2101"},
}

// boardSummary is a board as the toolbar names it.
func boardSummary(b board.Board) string {
	if b.Base == "" {
		return b.Name
	}
	var parts []string
	for _, change := range b.Changes {
		part, _, _ := strings.Cut(change.Path, ".")
		if !slices.Contains(parts, part) {
			parts = append(parts, part)
		}
	}
	if len(parts) == 0 {
		return b.Base
	}
	return b.Base + " + " + strings.Join(parts, ", ") + " mod"
}

func describeChange(change board.Change) string {
	words := strings.ReplaceAll(change.Path, ".", " ")
	if strings.HasPrefix(change.Value, "gpio") {
		return words + " on " + strings.ToUpper(change.Value)
	}
	return words + " " + change.Value
}

func (m *Model) choiceLabel(i int) string {
	choice := m.opts.Choices[i]
	switch {
	case choice.Err != nil:
		return choice.File
	case choice.Board.Base == "":
		return choice.Board.Name
	}
	return choice.Board.Name + ", " + strings.TrimPrefix(boardSummary(choice.Board), stockFamily)
}

// listRow is a row of the board list. Heading rows have no choice.
type listRow struct {
	text   string
	style  segStyle
	choice int
}

const noChoice = -1

func (m *Model) listRows() []listRow {
	roomy := m.height >= roomyHeight
	rows := []listRow{{"Board", styleStrong, noChoice}}
	if roomy {
		rows = append(rows, listRow{"", styleDim, noChoice})
	}
	project := slices.IndexFunc(m.opts.Choices, func(c projectChoice) bool { return c.FromProject })
	stock := slices.IndexFunc(m.opts.Choices, func(c projectChoice) bool { return !c.FromProject })
	for i := range m.opts.Choices {
		switch {
		case i == project:
			rows = append(rows, listRow{"From this project", styleDim, noChoice})
		case i == stock:
			if roomy && project >= 0 {
				rows = append(rows, listRow{"", styleDim, noChoice})
			}
			rows = append(rows, listRow{"Built in", styleDim, noChoice})
		}
		rows = append(rows, listRow{m.choiceLabel(i), styleText, i})
	}
	return rows
}

// listWindow is the first row shown, so that the chosen row stays on screen.
func (m *Model) listWindow(rows []listRow, height int) int {
	if len(rows) <= height {
		return 0
	}
	at := slices.IndexFunc(rows, func(r listRow) bool { return r.choice == m.chosen })
	return min(max(at-height/2, 0), len(rows)-height)
}

func (m *Model) columnWidth() int { return (m.width - 2*sidePad - columnGap) / 2 }

func (m *Model) bodyTop() int {
	if m.height >= roomyHeight {
		return 2
	}
	return 1
}

func (m *Model) bodyHeight() int { return m.height - 1 - m.bodyTop() }

func (m *Model) listView(height int) []string {
	rows := m.listRows()
	first := m.listWindow(rows, height)
	width := m.columnWidth()
	out := make([]string, 0, height)
	for _, r := range rows[first:min(first+height, len(rows))] {
		switch {
		case r.choice == noChoice:
			out = append(out, r.style.Render(fit(r.text, width)))
		case r.choice == m.chosen:
			out = append(out, styleSelected.Render(fit("› "+r.text, width)))
		default:
			out = append(out, r.style.Render(fit("  "+r.text, width)))
		}
	}
	return out
}

func detail(label, value string, width int) string {
	return fit(styleDim.Render(fit(label, detailLabelWidth))+styleText.Render(value), width)
}

func upper(chips []string) string {
	out := make([]string, len(chips))
	for i, chip := range chips {
		out[i] = strings.ToUpper(chip)
	}
	return strings.Join(out, ", ")
}

// detailsView is everything about the chosen board, then the project's checks.
func (m *Model) detailsView() []string {
	width := m.width - 2*sidePad - columnGap - m.columnWidth()
	gap := m.height >= roomyHeight
	choice := m.opts.Choices[m.chosen]
	var out []string
	if choice.Err != nil {
		out = append(out, styleBad.Render(fit("This board file won't load", width)))
		wrappedError := strings.Split(ansi.Wrap(cleanText(choice.Err.Error()), width, "/-"), "\n")
		for _, text := range wrappedError {
			out = append(out, styleDim.Render(fit(text, width)))
		}
	} else {
		b := choice.Board
		out = append(out, styleStrong.Render(fit(b.Name, width)))
		if gap {
			out = append(out, "")
		}
		screen := fmt.Sprintf("%d×%d %s", b.Display.Width, b.Display.Height, strings.ToUpper(b.Display.Controller))
		out = append(out, detail("screen", strings.TrimSpace(screen), width))
		left := make([]string, 0, len(b.I2C))
		for _, c := range b.I2C {
			left = append(left, c.Chip)
		}
		for _, role := range chipRoles {
			var found []string
			for _, chip := range strings.Fields(role.chips) {
				if slices.Contains(left, chip) {
					found = append(found, chip)
					left = slices.DeleteFunc(left, func(c string) bool { return c == chip })
				}
			}
			if len(found) > 0 {
				out = append(out, detail(role.label, upper(found), width))
			}
		}
		if b.AudioCodec != "" {
			out = append(out, detail("audio", strings.ToUpper(b.AudioCodec), width))
			left = slices.DeleteFunc(left, func(c string) bool { return c == b.AudioCodec })
		}
		if len(left) > 0 {
			out = append(out, detail("other", upper(left), width))
		}
		if b.Base != "" {
			out = append(out, detail("based on", b.Base, width))
		}
		if len(b.Changes) > 0 {
			changes := make([]string, len(b.Changes))
			for i, change := range b.Changes {
				changes[i] = describeChange(change)
			}
			out = append(out, detail("changes", strings.Join(changes, ", "), width))
		}
	}
	if gap {
		out = append(out, "")
	}
	return append(out, m.checks(width)...)
}

func check(ok bool, yes, no string, width int) string {
	if ok {
		return fit(styleAccent.Render("✓")+" "+styleText.Render(yes), width)
	}
	return fit(styleDim.Render("○ "+no), width)
}

func (m *Model) checks(width int) []string {
	count := len(m.opts.Config.Shortcuts)
	shortcuts := strconv.Itoa(count) + " shortcuts"
	if count == 1 {
		shortcuts = "1 shortcut"
	}
	return []string{
		check(true, "ESP-IDF project", "", width),
		check(m.opts.LVGL != "", "LVGL "+m.opts.LVGL, "no LVGL", width),
		check(m.opts.HasConfig, ".preflashrc, "+shortcuts, "no .preflashrc", width),
	}
}

func (m *Model) launchView() string {
	left := []seg{{styleKey, "preflash"}, {styleDim, "  "}, {styleDim, m.opts.Path}}
	rows := []string{bar(left, []seg{m.stateMark()}, m.width)}
	for range m.bodyTop() - 1 {
		rows = append(rows, "")
	}
	height := m.bodyHeight()
	list := m.listView(height)
	details := m.detailsView()
	width := m.columnWidth()
	for i := range height {
		cell := strings.Repeat(" ", width)
		if i < len(list) {
			cell = list[i]
		}
		text := strings.Repeat(" ", sidePad) + cell + strings.Repeat(" ", columnGap)
		if i < len(details) {
			text += details[i]
		}
		rows = append(rows, text)
	}
	return strings.Join(append(rows, m.footerRow()), "\n")
}

func (m *Model) launchKey(k tea.KeyPressMsg) {
	switch k.String() {
	case "up":
		m.chosen = max(m.chosen-1, 0)
	case "down":
		m.chosen = min(m.chosen+1, len(m.opts.Choices)-1)
	case "enter":
		m.launchEnter()
	case "esc":
		m.closePicker()
	}
}

// closePicker goes back to the running screen, if there's one to go back to.
func (m *Model) closePicker() {
	if m.board.Name != "" {
		m.picking = false
	}
}

func (m *Model) launchEnter() {
	choice := m.opts.Choices[m.chosen]
	if choice.Err != nil {
		return
	}
	m.picking = false
	if !m.appUp() {
		m.board = choice.Board
	}
	m.startRun(choice.Board)
}

// boardZones makes each board on the list clickable: a click chooses it, and a click on the
// chosen one runs it.
func (m *Model) boardZones() []zone {
	rows := m.listRows()
	height := m.bodyHeight()
	first := m.listWindow(rows, height)
	var zones []zone
	for i, r := range rows[first:min(first+height, len(rows))] {
		if r.choice == noChoice {
			continue
		}
		zones = append(zones, zone{
			id: fmt.Sprintf("board %d", r.choice),
			x0: sidePad, x1: sidePad + m.columnWidth(), y: m.bodyTop() + i,
			act: func() tea.Cmd {
				if m.chosen == r.choice {
					m.launchEnter()
				}
				m.chosen = r.choice
				return nil
			},
		})
	}
	return zones
}
