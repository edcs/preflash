package tui

import (
	"fmt"
	"slices"
	"strconv"
	"strings"

	"charm.land/bubbles/v2/textinput"
	tea "charm.land/bubbletea/v2"
	"github.com/charmbracelet/x/ansi"

	"github.com/edcs/preflash/internal/client"
)

const (
	// keptRows is how many trace rows each stream keeps on screen's behalf.
	keptRows = 5000
	// cellGap sits between a row's columns.
	cellGap = "  "
	// debugChrome is the rows that aren't the body: toolbar, views, columns and footer.
	debugChrome = 4
	// timeWidth fits 04:11.204, with hours to spare.
	timeWidth = 12
)

// debugTab is one view. A tab with a stream shows that stream's trace rows, and one with
// a table shows that above them.
type debugTab struct {
	name    string
	stream  string
	columns []column
	table   []column
}

// column is a heading and its width. The last column takes what's left.
type column struct {
	title string
	width int
}

var debugTabs = []debugTab{
	{name: "console"},
	{name: "i2c", stream: "i2c", columns: []column{{"time", timeWidth}, {"addr", 9}, {"device", 14}, {"op", 8}, {"data", 0}}},
	{name: "gpio", stream: "gpio", columns: []column{{"time", timeWidth}, {"pin", 9}, {"op", 11}, {"data", 0}}},
	{
		name: "tasks", stream: "task",
		columns: []column{{"time", timeWidth}, {"task", 18}, {"op", 8}, {"data", 0}},
		table:   []column{{"task", 18}, {"state", 10}, {"stack (declared)", 0}},
	},
	{
		name: "nvs", stream: "nvs",
		columns: []column{{"time", timeWidth}, {"key", 32}, {"op", 8}, {"data", 0}},
		table:   []column{{"namespace", 18}, {"key", 18}, {"size", 0}},
	},
	{name: "net", stream: "net", columns: []column{{"time", timeWidth}, {"subject", 9}, {"op", 13}, {"data", 0}}},
}

// firstDebugTab is i2c, where the mockup opens.
const firstDebugTab = 1

// debugView is the ^D screen, which exists while it shows.
type debugView struct {
	tab       int
	rows      map[string][]client.TraceRecord
	held      []client.TraceRecord // what arrived during a pause
	paused    bool
	filter    string
	filtering bool
	input     textinput.Model
	scroll    int
	note      string
	noteStyle segStyle
	tasks     []client.Task
	spaces    []client.Namespace
}

// debugRow is one row of the body.
type debugRow struct {
	cells []string
	style segStyle
}

func (m *Model) openDebug() {
	m.debug = &debugView{
		tab:   firstDebugTab,
		rows:  map[string][]client.TraceRecord{},
		input: newInput(),
	}
	m.b.Trace(true)
	m.pollDebug()
}

func (m *Model) closeDebug() {
	m.debug = nil
	m.b.Trace(false)
}

// pollDebug refreshes the table of the view showing.
func (m *Model) pollDebug() {
	if !m.appUp() {
		return
	}
	switch debugTabs[m.debug.tab].name {
	case "tasks":
		m.b.Tasks()
	case "nvs":
		m.b.NVS()
	}
}

func (m *Model) setDebugTab(tab int) {
	m.debug.tab = (tab + len(debugTabs)) % len(debugTabs)
	m.debug.scroll = 0
	m.pollDebug()
}

func (m *Model) debugKey(k tea.KeyPressMsg) tea.Cmd {
	d := m.debug
	if d.filtering {
		switch k.String() {
		case "enter":
			d.filtering = false
		case "esc":
			d.filtering = false
			d.filter = ""
			d.input.Reset()
		default:
			var cmd tea.Cmd
			d.input, cmd = d.input.Update(k)
			d.filter = d.input.Value()
			d.scroll = 0
			return cmd
		}
		return nil
	}
	switch k.String() {
	case "esc", "ctrl+d":
		m.closeDebug()
	case "tab":
		m.setDebugTab(d.tab + 1)
	case "shift+tab":
		m.setDebugTab(d.tab - 1)
	case "/":
		return m.startFilter()
	case "space", " ":
		m.togglePause()
	case "s":
		m.b.SaveTrace()
	case "pgup":
		m.scrollDebug(max(m.debugBodyHeight()-1, 1))
	case "pgdown":
		m.scrollDebug(-max(m.debugBodyHeight()-1, 1))
	case "ctrl+r":
		m.rebuild()
	}
	return nil
}

func (m *Model) startFilter() tea.Cmd {
	d := m.debug
	d.filtering = true
	d.input.SetValue(d.filter)
	d.input.CursorEnd()
	d.input.SetWidth(max(m.width/3, 8))
	return d.input.Focus()
}

func (m *Model) togglePause() {
	d := m.debug
	d.paused = !d.paused
	if !d.paused {
		d.keep(d.held)
		d.held = nil
	}
}

func (m *Model) scrollDebug(rows int) {
	m.debug.scroll = max(m.debug.scroll+rows, 0)
}

func (d *debugView) keep(records []client.TraceRecord) {
	for _, r := range records {
		rows := append(d.rows[r.Stream], r)
		if len(rows) > keptRows {
			rows = rows[len(rows)-keptRows:]
		}
		d.rows[r.Stream] = rows
	}
}

// traceRows takes rows off the socket, holding them back during a pause.
func (d *debugView) traceRows(records []client.TraceRecord, reset bool) {
	if reset {
		d.rows = map[string][]client.TraceRecord{}
		d.held = nil
		d.scroll = 0
	}
	if d.paused {
		d.held = append(d.held, records...)
		if len(d.held) > keptRows {
			d.held = d.held[len(d.held)-keptRows:]
		}
		return
	}
	d.keep(records)
	if d.scroll > 0 {
		stream := debugTabs[d.tab].stream
		d.scroll += len(slices.DeleteFunc(slices.Clone(records), func(r client.TraceRecord) bool { return r.Stream != stream }))
	}
}

func (d *debugView) say(text string, style segStyle) {
	d.note = text
	d.noteStyle = style
}

func (d *debugView) matches(cells []string) bool {
	if d.filter == "" {
		return true
	}
	return strings.Contains(strings.ToLower(strings.Join(cells, " ")), strings.ToLower(d.filter))
}

func traceCells(tab debugTab, r client.TraceRecord) []string {
	if tab.stream == "i2c" {
		return []string{r.Time(), r.Subject, r.Device, r.Op, r.Detail}
	}
	return []string{r.Time(), r.Subject, r.Op, r.Detail}
}

func (d *debugView) traceBody(tab debugTab) []debugRow {
	var out []debugRow
	for _, r := range d.rows[tab.stream] {
		cells := traceCells(tab, r)
		if !d.matches(cells) {
			continue
		}
		style := styleText
		if r.Injected {
			style = styleAmber
		}
		out = append(out, debugRow{cells: cells, style: style})
	}
	return out
}

func (d *debugView) tableBody(tab debugTab) []debugRow {
	var out []debugRow
	add := func(cells ...string) {
		if d.matches(cells) {
			out = append(out, debugRow{cells: cells, style: styleText})
		}
	}
	switch tab.name {
	case "tasks":
		for _, task := range d.tasks {
			add(task.Name, task.State, strconv.Itoa(task.Stack))
		}
	case "nvs":
		for _, space := range d.spaces {
			if len(space.Keys) == 0 {
				add(space.Name, unknown, unknown)
			}
			for _, key := range space.Keys {
				add(space.Name, key.Key, strconv.Itoa(key.Size))
			}
		}
	}
	return out
}

func (m *Model) debugBodyHeight() int { return max(m.height-debugChrome, 1) }

func (m *Model) debugView() string {
	rows := make([]string, 0, m.height)
	rows = append(rows, m.toolbarTop(), m.debugNav(), m.debugHeading())
	rows = append(rows, m.debugBody()...)
	rows = append(rows, m.footerRow())
	return strings.Join(rows, "\n")
}

// tabPosition is where a view's name sits along the nav row, x1 exclusive.
func tabPosition(tab int) (x0, x1 int) {
	x := sidePad
	for i, t := range debugTabs {
		width := ansi.StringWidth(t.name)
		if i == tab {
			return x, x + width
		}
		x += width + len(tabGap)
	}
	return x, x
}

const tabGap = "   "

func (m *Model) debugNav() string {
	var left []seg
	for i, tab := range debugTabs {
		if i > 0 {
			left = append(left, seg{styleDim, tabGap})
		}
		style := styleDim
		if i == m.debug.tab {
			style = styleKey
		}
		left = append(left, seg{style, tab.name})
	}
	return bar(left, nil, m.width)
}

func headingOf(columns []column, width int) string {
	cells := make([]string, len(columns))
	for i, c := range columns {
		cells[i] = c.title
	}
	return renderCells(columns, cells, width)
}

// renderCells lays cells out in the columns, cut to fit, in plain text.
func renderCells(columns []column, cells []string, width int) string {
	var out strings.Builder
	used := 0
	for i, c := range columns {
		text := ""
		if i < len(cells) {
			text = cleanText(cells[i])
		}
		cell := c.width
		if i == len(columns)-1 {
			cell = max(width-used, 1)
		}
		out.WriteString(fit(text, cell))
		used += cell
		if i < len(columns)-1 {
			out.WriteString(cellGap)
			used += len(cellGap)
		}
	}
	return out.String()
}

func (m *Model) debugHeading() string {
	d := m.debug
	tab := debugTabs[d.tab]
	columns := tab.columns
	if tab.table != nil {
		columns = tab.table
	}
	inner := m.width - 2*sidePad
	state := d.stateSegs()
	room := max(inner-segsWidth(state), 1)
	left := ""
	if columns != nil {
		left = strings.TrimRight(headingOf(columns, room), " ")
	}
	return strings.Repeat(" ", sidePad) + styleDim.Render(fit(left, room)) + renderOn(state, nil)
}

// stateSegs is what the view has to say for itself, at the right of the heading.
func (d *debugView) stateSegs() []seg {
	var state []seg
	add := func(s seg) {
		if len(state) > 0 {
			state = append(state, seg{styleDim, "  "})
		}
		state = append(state, s)
	}
	switch {
	case d.filtering:
		add(seg{styleKey, "/"})
		add(seg{styleText, d.input.Value() + "▏"})
	case d.filter != "":
		add(seg{styleDim, "filter " + cleanText(d.filter)})
	}
	if d.paused {
		add(seg{styleAmber, "paused"})
	}
	if d.note != "" {
		add(seg{d.noteStyle, ansi.Truncate(cleanText(d.note), 72, "…")})
	}
	return state
}

func (m *Model) debugBody() []string {
	d := m.debug
	height := m.debugBodyHeight()
	width := m.width - 2*sidePad
	tab := debugTabs[d.tab]
	if tab.stream == "" {
		return m.debugConsole(height, width)
	}
	if tab.table == nil {
		return bottomRows(d.traceBody(tab), tab.columns, height, width, &d.scroll)
	}

	// The table takes what it needs up to half the height, and the rows below it the rest.
	table := d.tableBody(tab)
	shown := min(len(table), max(height/2, 1))
	out := make([]string, 0, height)
	for _, row := range table[:shown] {
		out = append(out, indent(row.style.Render(renderCells(tab.table, row.cells, width)), m.width))
	}
	rest := height - shown
	if rest >= 2 {
		out = append(out, strings.Repeat(" ", sidePad)+styleDim.Render(fit(strings.TrimRight(headingOf(tab.columns, width), " "), width)))
		rest--
	}
	return append(out, bottomRows(d.traceBody(tab), tab.columns, rest, width, &d.scroll)...)
}

// bottomRows gives exactly height rows, newest last and scroll rows above the newest, with
// empty ones on top as the console has. It holds scroll to what there is.
func bottomRows(body []debugRow, columns []column, height, width int, scroll *int) []string {
	*scroll = min(*scroll, max(len(body)-height, 0))
	end := len(body) - *scroll
	start := max(end-height, 0)
	out := make([]string, height-(end-start), height)
	for _, row := range body[start:end] {
		out = append(out, strings.Repeat(" ", sidePad)+row.style.Render(renderCells(columns, row.cells, width)))
	}
	return out
}

// debugConsole is the serial console, with the filter's say over which lines show.
func (m *Model) debugConsole(height, width int) []string {
	d := m.debug
	var body []debugRow
	for _, l := range m.console.lines {
		if d.matches([]string{l.text}) {
			body = append(body, debugRow{cells: []string{l.text}, style: rowStyle(l.kind)})
		}
	}
	return bottomRows(body, []column{{"", 0}}, height, width, &d.scroll)
}

func (m *Model) debugZones() []zone {
	var zones []zone
	for i := range debugTabs {
		x0, x1 := tabPosition(i)
		zones = append(zones, zone{
			id: fmt.Sprintf("tab %d", i),
			x0: x0, x1: x1, y: 1,
			act: func() tea.Cmd {
				m.setDebugTab(i)
				return nil
			},
		})
	}
	return zones
}
