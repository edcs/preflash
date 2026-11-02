// Package crash reads the report a crashed app leaves behind and ties it to source. The app
// can only write addresses from a signal handler, so naming them happens here, with atos.
package crash

import (
	"bytes"
	"debug/macho"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
)

const (
	// nullReach is how far past null a struct's field can be and still mean a null pointer.
	nullReach = 0x1000
	// uuidCommand is LC_UUID, which debug/macho hands over as raw bytes.
	uuidCommand    = 0x1b
	uuidCommandLen = 24
	// managedFolder holds a project's downloaded components, which aren't its own code.
	managedFolder = "managed_components"
)

// Report is what the app wrote to crash.json, and what Resolve makes of it.
type Report struct {
	PID         int      `json:"pid"`
	Time        int64    `json:"time"`
	UptimeMs    int64    `json:"uptime_ms"`
	Signal      string   `json:"signal"`
	Code        int      `json:"code"`
	Address     string   `json:"address"`
	Access      string   `json:"access"`
	Task        string   `json:"task"`
	Kind        string   `json:"kind"`
	Message     string   `json:"message"`
	Executable  string   `json:"executable"`
	UUID        string   `json:"uuid"`
	LoadAddress string   `json:"load_address"`
	ImageSize   uint64   `json:"image_size"`
	LR          string   `json:"lr"`
	Frames      []string `json:"frames"`
	Sanitizer   string   `json:"sanitizer"`

	// Stack is the call stack, innermost first, and At is the frame the crash belongs to.
	Stack []Frame `json:"-"`
	At    int     `json:"-"`
	// Title and Location are the crash in a line each, the second from the project's folder.
	Title    string `json:"-"`
	Location string `json:"-"`
	// Rebuilt says the app isn't the one that crashed, so addresses can't be named.
	Rebuilt bool `json:"-"`

	project string
	summary string
}

// Frame is one call. Function is the address if nothing better is known.
type Frame struct {
	Function string
	File     string
	Line     int
}

// Runner runs a command and gives its standard output.
type Runner func(name string, args ...string) ([]byte, error)

// RunCommand is the Runner that really runs things.
func RunCommand(name string, args ...string) ([]byte, error) {
	return exec.Command(name, args...).Output()
}

// Read loads a report.
func Read(path string) (*Report, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	r := &Report{}
	if err := json.Unmarshal(data, r); err != nil {
		return nil, fmt.Errorf("the crash report at %s is unreadable: %w", path, err)
	}
	return r, nil
}

// Resolve fills in the source of each frame and the words for the crash. Whatever can't be
// named stays an address, because a poor report still beats none.
func (r *Report) Resolve(project string, run Runner) {
	r.project = project
	r.Rebuilt = r.UUID != "" && !strings.EqualFold(r.UUID, fileUUID(r.Executable))
	// A sanitizer stops on the thread it caught, so the handler's frames are the error's own,
	// and atos gives them whole paths where a sanitizer's own stack has bare file names.
	r.Stack = r.symbolise(run)
	if !hasSource(r.Stack) {
		if stack := parseSanitizerStack(r.Sanitizer); len(stack) > 0 {
			r.Stack = stack
		}
	}
	r.At = r.crashFrame()
	r.describe()
}

func hasSource(stack []Frame) bool {
	return slices.ContainsFunc(stack, func(f Frame) bool { return f.File != "" })
}

// fileUUID reads an executable's build ID, or nothing if it has gone.
func fileUUID(path string) string {
	file, err := macho.Open(path)
	if err != nil {
		return ""
	}
	defer file.Close()
	for _, load := range file.Loads {
		raw := load.Raw()
		if len(raw) >= uuidCommandLen && binary.LittleEndian.Uint32(raw) == uuidCommand {
			id := raw[8:uuidCommandLen]
			return strings.ToUpper(fmt.Sprintf("%x-%x-%x-%x-%x", id[:4], id[4:6], id[6:8], id[8:10], id[10:]))
		}
	}
	return ""
}

func parseAddress(text string) (uint64, bool) {
	value, err := strconv.ParseUint(strings.TrimPrefix(text, "0x"), 16, 64)
	return value, err == nil
}

func (r *Report) inApp(address uint64) bool {
	load, ok := parseAddress(r.LoadAddress)
	return ok && address >= load && address < load+r.ImageSize
}

// symbolise names the frames that are in the app. System code above and between them says
// nothing a firmware developer can act on.
func (r *Report) symbolise(run Runner) []Frame {
	var kept []string
	var asked []string
	lrAt := -1
	for i, text := range r.Frames {
		address, ok := parseAddress(text)
		if !ok || !r.inApp(address) {
			continue
		}
		if i > 0 {
			address-- // a return address sits after its call, maybe on the next line
		}
		kept = append(kept, text)
		asked = append(asked, fmt.Sprintf("0x%x", address))
		if i == 0 {
			if lr, ok := parseAddress(r.LR); ok && lr != 0 && r.inApp(lr) {
				lrAt = len(asked)
				asked = append(asked, fmt.Sprintf("0x%x", lr-1))
				kept = append(kept, r.LR)
			}
		}
	}
	if len(asked) == 0 {
		return nil
	}
	if !r.Rebuilt {
		args := append([]string{"-o", r.symbols(run), "-l", r.LoadAddress, "-i", "-fullPath"}, asked...)
		if output, err := run("atos", args...); err == nil {
			return withoutThreadStart(dropStaleLink(parseAtos(string(output)), lrAt))
		}
	}
	stack := make([]Frame, 0, len(kept))
	for _, text := range kept {
		if lrAt < 0 || text != r.LR {
			stack = append(stack, Frame{Function: text})
		}
	}
	return stack
}

// symbols gives the file for atos to read. The build leaves debug information in its objects,
// and atos only reads a dSYM, so this gathers one the first time an app crashes. That takes
// seconds, which nobody should pay on every build.
func (r *Report) symbols(run Runner) string {
	dsym := r.Executable + ".dSYM"
	dwarf := filepath.Join(dsym, "Contents", "Resources", "DWARF", filepath.Base(r.Executable))
	built, err := os.Stat(r.Executable)
	if err != nil {
		return r.Executable
	}
	if gathered, err := os.Stat(dwarf); err == nil && !gathered.ModTime().Before(built.ModTime()) {
		return dwarf
	}
	if _, err := run("dsymutil", r.Executable, "-o", dsym); err != nil {
		return r.Executable
	}
	if _, err := os.Stat(dwarf); err != nil {
		return r.Executable
	}
	return dwarf
}

// withoutThreadStart drops what every thread begins with: the C++ library's frames, and the
// wrapper that starts a task.
func withoutThreadStart(stack []Frame) []Frame {
	end := len(stack)
	for end > 1 {
		function := stack[end-1].Function
		if !strings.Contains(function, "std::__1::") && !strings.Contains(function, "xTaskCreatePinnedToCore::") {
			break
		}
		end--
	}
	return stack[:end]
}

// atosLine is atos's answer for an address it knows the source of, and one it doesn't.
var (
	atosSource = regexp.MustCompile(`^(.+) \(in [^)]*\) \((.+):(\d+)\)$`)
	atosSymbol = regexp.MustCompile(`^(.+) \(in [^)]*\)`)
)

// parseAtos gives each address's frames. With -i, one address can be several, for inlining,
// and a blank line ends each.
func parseAtos(output string) [][]Frame {
	var groups [][]Frame
	for _, group := range strings.Split(output, "\n\n") {
		var frames []Frame
		for _, line := range strings.Split(group, "\n") {
			if line = strings.TrimSpace(line); line != "" {
				frames = append(frames, parseAtosLine(line))
			}
		}
		if len(frames) > 0 {
			groups = append(groups, frames)
		}
	}
	return groups
}

func parseAtosLine(line string) Frame {
	if m := atosSource.FindStringSubmatch(line); m != nil {
		number, _ := strconv.Atoi(m[3])
		return Frame{Function: functionName(m[1]), File: m[2], Line: number}
	}
	if m := atosSymbol.FindStringSubmatch(line); m != nil {
		return Frame{Function: functionName(m[1])}
	}
	return Frame{Function: line}
}

// dropStaleLink flattens the groups. The link register only adds a caller for a leaf function:
// anywhere else it holds what's left from the last call, which came from the same function.
func dropStaleLink(groups [][]Frame, lrAt int) []Frame {
	var stack []Frame
	for i, frames := range groups {
		if i == lrAt {
			before := groups[i-1]
			if frames[0].Function == before[len(before)-1].Function {
				continue
			}
			if i+1 < len(groups) && frames[0].Function == groups[i+1][0].Function {
				continue
			}
		}
		stack = append(stack, frames...)
	}
	return stack
}

// functionName cuts a demangled name's arguments, which the screen has no room for.
func functionName(name string) string {
	name = strings.ReplaceAll(name, "(anonymous namespace)::", "")
	name = strings.TrimSpace(strings.TrimSuffix(strings.TrimSpace(name), " const"))
	if !strings.HasSuffix(name, ")") {
		return name
	}
	depth := 0
	for i := len(name) - 1; i >= 0; i-- {
		switch name[i] {
		case ')':
			depth++
		case '(':
			depth--
			if depth == 0 {
				return name[:i]
			}
		}
	}
	return name
}

var (
	sanitizerFrame   = regexp.MustCompile(`^\s+#\d+ (?:0x[0-9a-f]+ in )?(.+?) (/\S+?|\S+\.\w+):(\d+)(?::\d+)?(?: \(.*\))?$`)
	sanitizerAny     = regexp.MustCompile(`^\s+#\d+ `)
	sanitizerSummary = regexp.MustCompile(`^SUMMARY: (\w+): (.+?)(?: \S+:\d+(?::\d+)?| \(\S+\))?(?: in .+)?$`)
)

// parseSanitizerStack takes the report's first stack, which is where the error happened. It's
// the fallback for an app that has been rebuilt, as a sanitizer named its frames at the time.
func parseSanitizerStack(text string) []Frame {
	var stack []Frame
	began := false
	for _, line := range strings.Split(text, "\n") {
		if !sanitizerAny.MatchString(line) {
			if began {
				break
			}
			continue
		}
		began = true
		if m := sanitizerFrame.FindStringSubmatch(line); m != nil {
			number, _ := strconv.Atoi(m[3])
			stack = append(stack, Frame{Function: functionName(m[1]), File: m[2], Line: number})
		}
	}
	return stack
}

// sanitizerWords gives the kind of error a sanitizer found and which sanitizer it was.
func sanitizerWords(text string) (kind, tool string) {
	for _, line := range strings.Split(text, "\n") {
		if m := sanitizerSummary.FindStringSubmatch(line); m != nil {
			return m[2], m[1]
		}
	}
	return "", ""
}

// crashFrame picks the first frame in the project's own code, past any of Preflash's stand-ins
// and downloaded components, or else the first frame with any source.
func (r *Report) crashFrame() int {
	first := -1
	for i, frame := range r.Stack {
		if frame.File == "" {
			continue
		}
		if first < 0 {
			first = i
		}
		if r.inProject(frame.File) && !strings.Contains(frame.File, "/"+managedFolder+"/") {
			return i
		}
	}
	return max(first, 0)
}

func (r *Report) inProject(file string) bool {
	return r.project != "" && strings.HasPrefix(file, r.project+string(filepath.Separator))
}

// describe finds the words for the crash, with its function if it has one.
func (r *Report) describe() {
	in := ""
	var at Frame
	if r.At < len(r.Stack) {
		at = r.Stack[r.At]
	}
	if at.File != "" || (at.Function != "" && !strings.HasPrefix(at.Function, "0x")) {
		in = " in " + at.Function
	}
	if at.File != "" {
		r.Location = fmt.Sprintf("%s:%d", strings.TrimPrefix(at.File, r.project+"/"), at.Line)
	}
	if kind, tool := sanitizerWords(r.Sanitizer); kind != "" {
		r.Title = kind + in
		r.summary = "memory error"
		if tool == "ThreadSanitizer" {
			r.summary = kind
		}
		return
	}
	switch r.Kind {
	case "unsupported":
		r.Title, r.summary = r.Message, "unsupported call"
		return
	case "error_check":
		r.Title, r.summary = "ESP_ERROR_CHECK failed: "+r.Message, "failed check"
		return
	case "system_abort":
		r.Title, r.summary = "esp_system_abort: "+r.Message, "system abort"
		return
	case "watchdog":
		r.Title, r.summary = "Task watchdog: "+r.Message, "task watchdog"
		return
	}
	switch r.Signal {
	case "SIGABRT":
		r.Title, r.summary = "abort() called"+in, "abort"
	case "SIGILL":
		r.Title, r.summary = "Illegal instruction"+in, "illegal instruction"
	case "SIGFPE":
		r.Title, r.summary = "Arithmetic fault"+in, "arithmetic fault"
	case "SIGTRAP":
		r.Title, r.summary = "Trap"+in, "trap"
	default:
		r.Title, r.summary = r.memoryWords()+in, "memory error"
	}
}

func (r *Report) memoryWords() string {
	address, ok := parseAddress(r.Address)
	null := ok && address < nullReach
	switch {
	case r.Access == "execute" && null:
		return "Jump to null"
	case r.Access == "execute":
		return "Jump to bad memory at " + r.Address
	case r.Access == "" && null:
		return "Null pointer access"
	case r.Access == "":
		return "Bad memory access at " + r.Address
	case null:
		return "Null pointer " + r.Access
	}
	return "Bad memory " + r.Access + " at " + r.Address
}

// Headline says what kind of crash this was, and where in the firmware.
func (r *Report) Headline() string {
	if r.Task == "" {
		return r.summary + " on a host thread"
	}
	return r.summary + " on the " + r.Task + " task"
}

// Where is a frame's place as the stack shows it: a file and line, or a downloaded
// component's name, where the line wouldn't be yours to change.
func (r *Report) Where(f Frame) string {
	if f.File == "" {
		return ""
	}
	if i := strings.Index(f.File, "/"+managedFolder+"/"); i >= 0 {
		folder, _, _ := strings.Cut(f.File[i+len(managedFolder)+2:], "/")
		if _, name, ok := strings.Cut(folder, "__"); ok {
			return name
		}
		return folder
	}
	return fmt.Sprintf("%s:%d", filepath.Base(f.File), f.Line)
}

// Rows gives the stack from the crash's frame out, one line each, with the places lined up.
func (r *Report) Rows() []Row {
	stack := r.Stack[min(r.At, len(r.Stack)):]
	width := 0
	for _, frame := range stack {
		width = max(width, len(frame.Function))
	}
	rows := make([]Row, len(stack))
	for i, frame := range stack {
		rows[i] = Row{Function: fmt.Sprintf("%-*s", width, frame.Function), Where: r.Where(frame)}
	}
	return rows
}

// Row is a line of the stack.
type Row struct{ Function, Where string }

// Text is the whole report, as preflash crash prints it and y copies it.
func (r *Report) Text() string {
	var out bytes.Buffer
	fmt.Fprintln(&out, r.Title)
	if r.Location != "" {
		fmt.Fprintln(&out, r.Location)
	}
	if r.Signal == "SIGSEGV" || r.Signal == "SIGBUS" {
		fmt.Fprintf(&out, "%s, %s at %s\n", r.Headline(), r.Signal, r.Address)
	} else {
		fmt.Fprintf(&out, "%s, %s\n", r.Headline(), r.Signal)
	}
	if r.Rebuilt {
		fmt.Fprintln(&out, "The app has been rebuilt since, so frames can't be named.")
	}
	fmt.Fprintln(&out)
	for i, row := range r.Rows() {
		mark := "  "
		if i == 0 {
			mark = "› "
		}
		fmt.Fprintln(&out, strings.TrimRight(mark+row.Function+"   "+row.Where, " "))
	}
	if r.Sanitizer != "" {
		fmt.Fprintf(&out, "\n%s", r.Sanitizer)
	}
	return out.String()
}

// Debugger is the command that attaches lldb to a stopped app.
func Debugger(pid int) *exec.Cmd {
	words := strings.Fields(os.Getenv("PREFLASH_LLDB"))
	if len(words) == 0 {
		words = []string{"lldb"}
	}
	return exec.Command(words[0], append(words[1:], "-p", strconv.Itoa(pid))...)
}

// Editor is the command that opens a file at a line. Most editors take +line, and the
// VS Code family wants -g file:line.
func Editor(file string, line int) *exec.Cmd {
	words := strings.Fields(os.Getenv("EDITOR"))
	if len(words) == 0 {
		words = strings.Fields(os.Getenv("VISUAL"))
	}
	if len(words) == 0 {
		words = []string{"vi"}
	}
	args := words[1:]
	switch filepath.Base(words[0]) {
	case "code", "cursor", "windsurf":
		args = append(args, "-g", fmt.Sprintf("%s:%d", file, line))
	default:
		args = append(args, "+"+strconv.Itoa(line), file)
	}
	return exec.Command(words[0], args...)
}

// Copy puts text on the Mac's clipboard.
func Copy(text string) error {
	name := os.Getenv("PREFLASH_PBCOPY")
	if name == "" {
		name = "pbcopy"
	}
	cmd := exec.Command(name)
	cmd.Stdin = strings.NewReader(text)
	return cmd.Run()
}
