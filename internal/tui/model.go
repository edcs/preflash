// Package tui is the terminal app: a launch screen for choosing a board, and a running
// screen with a toolbar, the serial console and shortcut chips.
package tui

import (
	"fmt"
	"slices"
	"strings"
	"time"

	"charm.land/bubbles/v2/textinput"
	tea "charm.land/bubbletea/v2"
	"github.com/charmbracelet/x/ansi"

	"github.com/edcs/preflash/internal/app"
	"github.com/edcs/preflash/internal/board"
	"github.com/edcs/preflash/internal/client"
	"github.com/edcs/preflash/internal/project"
	"github.com/edcs/preflash/internal/rc"
)

const (
	// The smallest window worth drawing.
	minWidth, minHeight = 60, 16
	// numberedShortcuts is how many shortcuts get a number key.
	numberedShortcuts = 9
	// wheelRows is how far a notch of the wheel scrolls.
	wheelRows = 3
	// statusEvery is how often the toolbar refreshes. The app's fps counts one second.
	statusEvery = time.Second
	// dateFormat is what {today} fills in as.
	dateFormat = "2006-01-02"
)

// Backend is what the model drives. app.Supervisor is one.
type Backend interface {
	Events() <-chan app.Event
	Run(b board.Board) bool
	Send(lines ...string)
	Status()
}

// Options is what the app knows about a project before anything runs.
type Options struct {
	Name      string                  // the project's folder name
	Path      string                  // its folder, as shown
	Config    rc.Config               // its .preflashrc
	HasConfig bool                    // whether it has one
	LVGL      string                  // its LVGL version, if any
	Choices   []project.Choice        // the boards it can run on
	Board     *board.Board            // run this straight away, skipping the launch screen
	Boards    func() []project.Choice // reads the choices again, as files can change meanwhile
}

// appState is how the app stands, apart from any build.
type appState int

const (
	stateIdle appState = iota
	stateStarting
	stateRunning
	stateRestarting
	stateStopped
	stateCrashed
	stateBuildFailed
)

// Model is the whole app. It's only touched from Bubble Tea's loop.
type Model struct {
	b    Backend
	opts Options
	now  func() time.Time

	width, height int
	picking       bool // the launch screen is showing
	canGoBack     bool // the running screen has been shown, so esc can return to it
	chosen        int  // the row chosen there

	board    board.Board
	state    appState
	building bool
	reason   string // what the toolbar says when nothing runs
	status   client.Status
	reported bool // status holds a real answer
	captures map[string]string

	console *console
	scroll  int // rows above the newest

	input     textinput.Model
	history   []string
	historyAt int
	draft     string
	ask       *asking

	pressed string // the zone a press began on
}

// asking is a shortcut waiting for its values.
type asking struct {
	shortcut rc.Shortcut
	at       int
	values   map[string]string
	draft    string // what the input held, which comes back afterwards
}

type (
	eventMsg struct{ event app.Event }
	tickMsg  struct{}
)

// New makes the model. The launch screen shows unless opts names a board.
func New(b Backend, opts Options) *Model {
	in := textinput.New()
	in.Prompt = ""
	st := in.Styles()
	st.Focused.Text = styleText
	st.Blurred.Text = styleText
	st.Focused.Placeholder = styleDim
	st.Blurred.Placeholder = styleDim
	st.Cursor.Color = colourText
	st.Cursor.Blink = false
	in.SetStyles(st)
	in.Focus()

	m := &Model{
		b:         b,
		opts:      opts,
		now:       time.Now,
		picking:   opts.Board == nil,
		canGoBack: opts.Board != nil,
		console:   newConsole(),
		input:     in,
		captures:  map[string]string{},
	}
	if opts.Board != nil {
		m.board = *opts.Board
		for i, choice := range opts.Choices {
			if choice.Board.Name == opts.Board.Name {
				m.chosen = i
				break
			}
		}
	}
	m.setPlaceholder()
	return m
}

func (m *Model) setPlaceholder() {
	switch count := min(len(m.opts.Config.Shortcuts), numberedShortcuts); count {
	case 0:
		m.input.Placeholder = "type a command"
	case 1:
		m.input.Placeholder = "type a command, or press 1"
	default:
		m.input.Placeholder = fmt.Sprintf("type a command, or press 1-%d", count)
	}
}

// Init starts following the app, and the run a named board asks for.
func (m *Model) Init() tea.Cmd {
	if !m.picking {
		m.startRun(m.board)
	}
	return tea.Batch(m.listen(), tick())
}

func (m *Model) listen() tea.Cmd {
	events := m.b.Events()
	return func() tea.Msg { return eventMsg{<-events} }
}

func tick() tea.Cmd {
	return tea.Tick(statusEvery, func(time.Time) tea.Msg { return tickMsg{} })
}

// Update handles one message.
func (m *Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width, m.height = msg.Width, msg.Height
		m.sizeInput()
	case eventMsg:
		m.event(msg.event)
		return m, m.listen()
	case tickMsg:
		if m.appUp() {
			m.b.Status()
		}
		return m, tick()
	case tea.KeyPressMsg:
		return m, m.key(msg)
	case tea.MouseClickMsg:
		m.pressed = ""
		if msg.Button == tea.MouseLeft {
			m.pressed = m.zoneAt(msg.X, msg.Y).id
		}
	case tea.MouseReleaseMsg:
		pressed := m.pressed
		m.pressed = ""
		if at := m.zoneAt(msg.X, msg.Y); pressed != "" && at.id == pressed && msg.Button == tea.MouseLeft {
			return m, at.act()
		}
	case tea.MouseWheelMsg:
		switch msg.Button {
		case tea.MouseWheelUp:
			m.scrollBy(wheelRows)
		case tea.MouseWheelDown:
			m.scrollBy(-wheelRows)
		}
	default:
		var cmd tea.Cmd
		m.input, cmd = m.input.Update(msg)
		return m, cmd
	}
	return m, nil
}

// appUp says whether an app process is up, or will be in a moment.
func (m *Model) appUp() bool {
	return m.state == stateStarting || m.state == stateRunning || m.state == stateRestarting
}

func (m *Model) event(e app.Event) {
	switch e := e.(type) {
	case app.Building:
		m.building = true
		m.say(fmt.Sprintf("Building %s for %s", m.opts.Name, e.Board.Name), lineDim)
	case app.BuildOutput:
		m.say(e.Line, lineDim)
	case app.BuildFailed:
		m.building = false
		m.say(e.Err.Error(), lineBad)
		if e.AppRunning {
			m.say("The old app keeps running.", lineDim)
			return
		}
		m.state = stateBuildFailed
		m.reason = "Nothing is running. ^R tries again."
	case app.Started:
		m.building = false
		m.board = e.Board
		m.state = stateStarting
		m.newBoot()
	case app.Connected:
		m.state = stateRunning
		m.newBoot()
		m.b.Status()
	case app.Serial:
		m.serial(e.Data)
	case app.Dropped:
		m.say(fmt.Sprintf("%d bytes of serial were dropped because this terminal fell behind", e.Bytes), lineWarn)
	case app.Restarting:
		m.state = stateRestarting
		m.reported = false
		m.captures = map[string]string{}
	case app.Exited:
		m.exited(e)
	case app.Status:
		m.status = e.Status
		m.reported = true
	case app.SendFailed:
		text := e.Err.Error()
		if e.Line != "" {
			text = fmt.Sprintf("%s: %s", e.Line, text)
		}
		m.say(text, lineBad)
	}
}

// newBoot closes the last boot's output, which the stream can keep sending after it says
// it's restarting, and forgets what it said.
func (m *Model) newBoot() {
	m.endLine()
	m.reported = false
	m.captures = map[string]string{}
}

func (m *Model) exited(e app.Exited) {
	m.endLine()
	for _, text := range e.Tail {
		m.say(text, lineText)
	}
	m.state = stateStopped
	kind := lineDim
	if e.Crashed {
		m.state, kind = stateCrashed, lineBad
	}
	m.say(e.Reason, kind)
	m.reason = e.Reason + " ^R builds and runs."
}

// serial takes bytes from the stream, keeping a scrolled view on the same text.
func (m *Model) serial(data []byte) {
	width := m.consoleWidth()
	before := m.console.partialRows(width)
	finished := m.console.feed(data)
	grown := m.console.partialRows(width) - before
	for _, text := range finished {
		grown += rowCount(text, width)
		m.capture(text)
	}
	if m.scroll > 0 {
		m.scroll += grown
	}
}

func (m *Model) capture(text string) {
	for _, field := range m.opts.Config.Status {
		if value, ok := field.Match(text); ok {
			m.captures[field.Name] = value
		}
	}
}

// endLine finishes a serial line that has no newline yet, as something else is about to
// follow it.
func (m *Model) endLine() {
	if text, ok := m.console.flush(); ok {
		m.capture(text)
	}
}

// say adds preflash's own line to the console.
func (m *Model) say(text string, kind lineKind) {
	m.endLine()
	m.console.add(text, kind)
	if m.scroll > 0 {
		m.scroll += rowCount(text, m.consoleWidth())
	}
}

func (m *Model) scrollBy(rows int) {
	_, scroll := m.console.rows(m.consoleWidth(), m.consoleHeight(), max(m.scroll+rows, 0))
	m.scroll = scroll
}

// startRun builds and runs b as its file stands now.
func (m *Model) startRun(b board.Board) {
	fresh, err := board.Reload(b)
	if err != nil {
		m.say(err.Error(), lineBad)
		return
	}
	if !m.b.Run(fresh) {
		m.say("A build is already running.", lineWarn)
	}
}

func (m *Model) key(k tea.KeyPressMsg) tea.Cmd {
	if k.String() == "ctrl+c" {
		return tea.Quit
	}
	if m.width < minWidth || m.height < minHeight {
		return nil
	}
	if m.picking {
		m.launchKey(k)
		return nil
	}
	switch k.String() {
	case "ctrl+r":
		m.rebuild()
	case "ctrl+b":
		m.openPicker()
	case "ctrl+l":
		m.clear()
	case "pgup":
		m.scrollBy(max(m.consoleHeight()-1, 1))
	case "pgdown":
		m.scrollBy(-max(m.consoleHeight()-1, 1))
	case "enter":
		m.enter()
	case "esc":
		m.cancelAsk()
	case "up":
		m.walkHistory(-1)
	case "down":
		m.walkHistory(1)
	default:
		if i, ok := m.shortcutKey(k); ok {
			m.useShortcut(i)
			return nil
		}
		var cmd tea.Cmd
		m.input, cmd = m.input.Update(k)
		return cmd
	}
	return nil
}

func (m *Model) rebuild() { m.startRun(m.board) }

func (m *Model) clear() {
	m.console.clear()
	m.scroll = 0
}

func (m *Model) openPicker() {
	m.cancelAsk()
	m.refreshChoices()
	m.picking = true
}

// refreshChoices reads the boards again, staying on the same one.
func (m *Model) refreshChoices() {
	if m.opts.Boards == nil {
		return
	}
	was := m.opts.Choices[m.chosen]
	m.opts.Choices = m.opts.Boards()
	m.chosen = max(slices.IndexFunc(m.opts.Choices, func(c project.Choice) bool {
		return c.File == was.File && (c.File != "" || c.Board.Name == was.Board.Name)
	}), 0)
}

// shortcutKey says which shortcut a key runs. Number keys are only that while the input is
// empty, because otherwise they're text.
func (m *Model) shortcutKey(k tea.KeyPressMsg) (int, bool) {
	if m.ask != nil || m.input.Value() != "" || len(k.Text) != 1 || k.Mod != 0 {
		return 0, false
	}
	i := int(k.Text[0]) - '1'
	count := min(len(m.opts.Config.Shortcuts), numberedShortcuts)
	return i, i >= 0 && i < count
}

func (m *Model) useShortcut(i int) {
	if m.ask != nil || i >= len(m.opts.Config.Shortcuts) {
		return
	}
	s := m.opts.Config.Shortcuts[i]
	if len(s.Ask) == 0 {
		m.send(s.Lines(nil, m.now().Format(dateFormat)))
		return
	}
	m.ask = &asking{shortcut: s, values: map[string]string{}, draft: m.input.Value()}
	m.showAsk()
}

func (m *Model) showAsk() {
	m.input.SetValue(m.ask.shortcut.Ask[m.ask.at].Default)
	m.input.CursorEnd()
	m.sizeInput()
}

func (m *Model) cancelAsk() {
	if m.ask != nil {
		m.input.SetValue(m.ask.draft)
		m.input.CursorEnd()
		m.ask = nil
		m.sizeInput()
	}
}

func (m *Model) enter() {
	value := strings.TrimSpace(m.input.Value())
	if value == "" {
		return
	}
	if m.ask != nil {
		m.answerAsk(value)
		return
	}
	m.input.Reset()
	if len(m.history) == 0 || m.history[len(m.history)-1] != value {
		m.history = append(m.history, value)
	}
	m.historyAt = len(m.history)
	m.send([]string{value})
}

func (m *Model) answerAsk(value string) {
	a := m.ask
	a.values[a.shortcut.Ask[a.at].Name] = value
	a.at++
	if a.at < len(a.shortcut.Ask) {
		m.showAsk()
		return
	}
	m.cancelAsk()
	m.send(a.shortcut.Lines(a.values, m.now().Format(dateFormat)))
}

func (m *Model) send(lines []string) {
	if !m.appUp() {
		m.say("Nothing is running. ^R builds and runs.", lineWarn)
		return
	}
	m.scroll = 0
	for _, text := range lines {
		m.say(m.prompt()+" "+text, lineEcho)
	}
	m.b.Send(lines...)
}

func (m *Model) walkHistory(step int) {
	if m.ask != nil {
		return
	}
	at := m.historyAt + step
	if at < 0 || at > len(m.history) {
		return
	}
	if m.historyAt == len(m.history) {
		m.draft = m.input.Value()
	}
	m.historyAt = at
	if at == len(m.history) {
		m.input.SetValue(m.draft)
	} else {
		m.input.SetValue(m.history[at])
	}
	m.input.CursorEnd()
}

// prompt is what the input row and echoed lines start with.
func (m *Model) prompt() string {
	if m.opts.Config.Prompt != "" {
		return m.opts.Config.Prompt
	}
	return m.opts.Name + ">"
}

// View draws the screen.
func (m *Model) View() tea.View {
	v := tea.NewView(m.content())
	v.AltScreen = true
	v.MouseMode = tea.MouseModeCellMotion
	v.BackgroundColor = colourGround
	v.WindowTitle = "preflash " + m.opts.Name
	return v
}

func (m *Model) content() string {
	switch {
	case m.width == 0 || m.height == 0:
		return ""
	case m.width < minWidth || m.height < minHeight:
		return tooSmall(m.width)
	case m.picking:
		return m.launchView()
	}
	return m.runningView()
}

// tooSmall is what a window under the minimum shows instead.
func tooSmall(width int) string {
	return styleDim.Render(ansi.Wrap(fmt.Sprintf("Window too small: Preflash needs %d×%d.", minWidth, minHeight), width, ""))
}
