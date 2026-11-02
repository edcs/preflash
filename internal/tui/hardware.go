package tui

import (
	"slices"
	"strings"

	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"

	"github.com/edcs/preflash/internal/client"
)

const (
	// panelWidth is the mockup's, in cells, and panelTop how far down it starts.
	panelWidth, panelTop = 76, 3
	panelPad             = 2
	// panelLabelWidth is the column of names.
	panelLabelWidth = 18
	// frameRows is the panel's top and bottom edges.
	frameRows = 2

	appliedNow  = "Applied now."
	appliedBoot = "Applied on the next boot. ^R restarts now."
)

var styleFaint = lipgloss.NewStyle().Foreground(lipgloss.Color("#3F484F"))

// hardwarePanel is the ^H overlay, which exists while it shows.
type hardwarePanel struct {
	items  []client.HardwareItem
	chosen string // the item's name, which survives the list being read again
	failed string // why the last change was refused
}

func (m *Model) openPanel() {
	m.panel = &hardwarePanel{}
	if m.appUp() {
		m.b.Hardware()
	}
}

func (m *Model) closePanel() { m.panel = nil }

// choosable is what has more than one setting.
func (p *hardwarePanel) choosable() []client.HardwareItem {
	return slices.DeleteFunc(slices.Clone(p.items), func(item client.HardwareItem) bool {
		return len(item.Options) < 2
	})
}

// at is where the chosen item sits among those that can be chosen, the first if it's gone.
func (p *hardwarePanel) at() int {
	return max(slices.IndexFunc(p.choosable(), func(item client.HardwareItem) bool {
		return item.Name == p.chosen
	}), 0)
}

func (p *hardwarePanel) receive(items []client.HardwareItem) {
	p.items = items
	if choices := p.choosable(); len(choices) > 0 {
		p.chosen = choices[p.at()].Name
	}
}

func (p *hardwarePanel) move(step int) {
	p.failed = ""
	choices := p.choosable()
	if len(choices) == 0 {
		return
	}
	p.chosen = choices[(p.at()+step+len(choices))%len(choices)].Name
}

func (m *Model) changeHardware(step int) {
	p := m.panel
	p.failed = ""
	choices := p.choosable()
	if len(choices) == 0 {
		return
	}
	item := choices[p.at()]
	from := max(slices.IndexFunc(item.Options, func(o client.HardwareOption) bool { return o.Value == item.Next }), 0)
	count := len(item.Options)
	m.b.SetHardware(item.Name, item.Options[(from+step+count)%count].Value)
}

func (m *Model) panelKey(key string) {
	switch key {
	case "esc", "ctrl+h":
		m.closePanel()
	case "up":
		m.panel.move(-1)
	case "down":
		m.panel.move(1)
	case "left":
		m.changeHardware(-1)
	case "right":
		m.changeHardware(1)
	case "ctrl+r":
		m.rebuild()
	}
}

// panelLine is one row of the panel's content, and whether the arrows are on it.
type panelLine struct {
	segs   []seg
	chosen bool
}

func itemSegs(item client.HardwareItem) []seg {
	value := []seg{{styleText, item.Label(item.Value)}}
	if item.Name == "rotation" {
		var others []string
		for _, option := range item.Options {
			if option.Value != item.Value {
				others = append(others, option.Label)
			}
		}
		value = append(value, seg{styleDim, "  " + strings.Join(others, " ")})
	}
	if item.Waiting() {
		value = []seg{{styleAmber, item.Label(item.Next) + " (next boot)"}}
	}
	return append([]seg{{styleDim, fit(item.Title, panelLabelWidth)}}, value...)
}

func (p *hardwarePanel) lines() []panelLine {
	var lines []panelLine
	group := ""
	chosen := ""
	if choices := p.choosable(); len(choices) > 0 {
		chosen = choices[p.at()].Name
	}
	for _, item := range p.items {
		if item.Group != group {
			group = item.Group
			lines = append(lines, panelLine{}, panelLine{segs: []seg{{styleDim, group}}})
		}
		lines = append(lines, panelLine{segs: itemSegs(item), chosen: item.Name == chosen})
	}
	return lines
}

func (m *Model) panelNote() seg {
	p := m.panel
	if p.failed != "" {
		return seg{styleAmber, cleanText(p.failed)}
	}
	waiting := slices.ContainsFunc(p.items, client.HardwareItem.Waiting)
	if choices := p.choosable(); !waiting && len(choices) > 0 && choices[p.at()].Applies != "boot" {
		return seg{styleDim, appliedNow}
	}
	return seg{styleDim, appliedBoot}
}

// window keeps the chosen line in view when the panel has more lines than room.
func window(lines []panelLine, room int) []panelLine {
	if len(lines) <= room {
		return lines
	}
	at := max(slices.IndexFunc(lines, func(l panelLine) bool { return l.chosen }), 0)
	start := min(max(at-room/2, 0), len(lines)-room)
	return lines[start : start+room]
}

// panelBox draws the panel as rows of exactly its width.
func (m *Model) panelBox(room int) []string {
	width := min(panelWidth, m.width-2*sidePad)
	inner := width - 2 - 2*panelPad
	surface := surfaceStyle
	edge := styleAccent.Background(surface.GetBackground())
	row := func(content string) string {
		pad := surface.Render(strings.Repeat(" ", panelPad))
		return edge.Render("│") + pad + content + pad + edge.Render("│")
	}
	plain := func(segs []seg) string {
		used := min(segsWidth(segs), inner)
		return ansi.Truncate(renderOn(segs, &surface), inner, "…") + surface.Render(strings.Repeat(" ", inner-used))
	}

	var body []panelLine
	if m.appUp() {
		body = m.panel.lines()
	} else {
		body = []panelLine{{segs: []seg{{styleText, "Nothing is running. ^R builds and runs."}}}}
	}
	// The title, the note and the two edges always show.
	body = window(body, max(room-frameRows-2, 1))

	rows := []string{edge.Render("╭" + strings.Repeat("─", width-2) + "╮")}
	rows = append(rows, row(plain([]seg{{styleStrong, "Hardware"}})))
	for _, line := range body {
		if line.chosen {
			text := ""
			for _, s := range line.segs {
				text += s.text
			}
			rows = append(rows, row(styleSelected.Render(fit(text, inner))))
			continue
		}
		rows = append(rows, row(plain(line.segs)))
	}
	if m.appUp() {
		rows = append(rows, row(plain([]seg{m.panelNote()})))
	}
	return append(rows, edge.Render("╰"+strings.Repeat("─", width-2)+"╯"))
}

// panelView is the running screen with the console faint and the panel over it, as the
// mockup has it, with no chips or input to take a key.
func (m *Model) panelView() string {
	width := m.width
	height := max(m.height-toolbarRows-1, 1)
	shown, _ := m.console.rows(max(width-2*sidePad, 1), height, 0)
	rows := make([]string, 0, m.height)
	rows = append(rows, m.toolbarTop(), m.toolbarBottom())
	blank := strings.Repeat(" ", width)
	for range height - len(shown) {
		rows = append(rows, blank)
	}
	for _, r := range shown {
		rows = append(rows, styleFaint.Render(fit(strings.Repeat(" ", sidePad)+r.text, width)))
	}
	rows = append(rows, m.footerRow())

	top := min(panelTop, toolbarRows)
	if m.height-1-panelTop >= frameRows+len(m.panel.lines())+2 {
		top = panelTop
	}
	box := m.panelBox(m.height - 1 - top)
	boxWidth := ansi.StringWidth(box[0])
	left := (width - boxWidth) / 2
	for i, line := range box {
		base := rows[top+i]
		rows[top+i] = ansi.Cut(base, 0, left) + line + ansi.Cut(base, left+boxWidth, width)
	}
	return strings.Join(rows, "\n")
}
