// Package app supervises a project's firmware for the terminal app: it builds, starts the
// app as a child, follows it over its socket and reports all of that as events.
package app

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/edcs/preflash/internal/board"
	"github.com/edcs/preflash/internal/client"
)

const (
	// requestsQueued is how far console lines can run ahead of the socket.
	requestsQueued = 1024
	// tailLines and tailBytes bound what an app that never connected leaves in the console.
	tailLines = 20
	tailBytes = 64 * 1024
	// answerPatience is how long a request waits for the app to answer at all, which a
	// restart can take as long as the firmware's process does to start.
	answerPatience = 15 * time.Second
	// drainPatience is how long a stopped app's stream gets to finish.
	drainPatience = 2 * time.Second
	// watchPause spaces out looks for a socket that hasn't appeared yet.
	watchPause = 50 * time.Millisecond
	// traceEvery is how often trace rows reach the screen, and traceHeld how many can wait for
	// that: past it the oldest go, so that a screen behind never holds up the socket.
	traceEvery = 100 * time.Millisecond
	traceHeld  = 8192
)

// Tests shorten these.
var (
	// callPatience limits one request, so an app that has stopped answering can't hold up
	// the ones behind it.
	callPatience = 2 * time.Second
	// How long a stopping app gets, after a quit and then after a signal.
	stopPatience = 5 * time.Second
	killPatience = 2 * time.Second
)

// Event is something that happened to the build or the app.
type Event any

type (
	// Building says a build began.
	Building struct{ Board board.Board }
	// BuildOutput is one line the build printed.
	BuildOutput struct{ Line string }
	// BuildFailed says the build, or what comes before it, didn't work.
	BuildFailed struct {
		Err        error
		AppRunning bool // an earlier app is still up
	}
	// Started says an app process began. It isn't answering yet.
	Started struct {
		Board board.Board
		PID   int
	}
	// Connected says a boot answered on the socket.
	Connected struct{ Hello client.Hello }
	// Serial is the next bytes of the stream. Boundaries mean nothing.
	Serial struct{ Data []byte }
	// Dropped says output was lost because this reader fell behind.
	Dropped struct{ Bytes int }
	// Restarting says the app is about to restart itself.
	Restarting struct{}
	// Exited says the app stopped when nothing here asked it to.
	Exited struct {
		Crashed bool
		Reason  string   // a sentence saying how
		Tail    []string // its last output, if it never reached the socket
	}
	// Status answers a call to Supervisor.Status.
	Status struct{ client.Status }
	// TraceRows is the next trace records. Reset says a connection began, and with it the
	// ring's replay, so what came before these is out of date.
	TraceRows struct {
		Rows  []client.TraceRecord
		Reset bool
	}
	// Tasks answers a call to Supervisor.Tasks.
	Tasks struct{ Tasks []client.Task }
	// NVS answers a call to Supervisor.NVS.
	NVS struct{ Namespaces []client.Namespace }
	// Hardware answers a call to Supervisor.Hardware or SetHardware.
	Hardware struct{ Items []client.HardwareItem }
	// HardwareFailed says a change didn't happen, and why.
	HardwareFailed struct{ Err error }
	// TraceSaved says where a save put the trace.
	TraceSaved struct {
		Path    string
		Records int
	}
	// SendFailed says a line didn't reach the app.
	SendFailed struct {
		Line string
		Err  error
	}
)

// Target is the project as the supervisor needs it. project.Project is one.
type Target interface {
	Environment(headless bool) ([]string, error)
	CheckNoApp() error
	Build(ctx context.Context, env []string, output io.Writer) error
	Install(b board.Board, env []string) (string, []string, error)
	SocketPath() string
	LogPath() string
}

// errNotRunning is what a line sent to no app fails with.
var errNotRunning = errors.New("the app is not running")

// Supervisor runs one project's app. Its methods are safe to call from anywhere, and what
// comes of them arrives on Events, which someone must keep reading until Close.
type Supervisor struct {
	target   Target
	headless bool
	events   chan Event
	ctx      context.Context
	cancel   context.CancelFunc
	requests chan request
	asking   atomic.Bool // a status request is waiting
	builds   sync.WaitGroup
	workers  sync.WaitGroup

	mu        sync.Mutex
	current   *child
	building  bool
	closed    bool
	traceStop chan struct{} // closing it ends the trace's connection, nil if there's none
}

// request is one job for the control connection: lines to send in order, or work to run, or,
// with neither, a status to fetch.
type request struct {
	lines []string
	work  func(*client.Client) (Event, error)
	// failed turns a failure into an event, or leaves it unsaid if nil, as for a poll.
	failed func(error) Event
}

// child is one app process.
type child struct {
	cmd       *exec.Cmd
	exited    chan struct{} // closed once the process is reaped
	watched   chan struct{} // closed once its stream has ended
	finished  chan struct{} // closed once it's fully dealt with
	stopping  atomic.Bool   // stopped on purpose, so nobody needs to hear about it
	connected atomic.Bool
}

// New makes a supervisor. Nothing starts until Run.
func New(target Target, headless bool) *Supervisor {
	ctx, cancel := context.WithCancel(context.Background())
	s := &Supervisor{
		target:   target,
		headless: headless,
		events:   make(chan Event),
		ctx:      ctx,
		cancel:   cancel,
		requests: make(chan request, requestsQueued),
	}
	s.workers.Add(1)
	go s.control()
	return s
}

// Events is everything that happens, in order.
func (s *Supervisor) Events() <-chan Event { return s.events }

func (s *Supervisor) emit(e Event) {
	select {
	case s.events <- e:
	case <-s.ctx.Done():
	}
}

// Run builds the project and, if that works, replaces any running app with the result on
// board b. It returns false, doing nothing, if a build is already going.
func (s *Supervisor) Run(b board.Board) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.building || s.closed {
		return false
	}
	s.building = true
	s.builds.Add(1)
	go func() {
		defer s.builds.Done()
		defer func() {
			s.mu.Lock()
			s.building = false
			s.mu.Unlock()
		}()
		s.run(b)
	}()
	return true
}

func (s *Supervisor) run(b board.Board) {
	s.emit(Building{Board: b})
	env, err := s.target.Environment(s.headless)
	if err == nil && s.running() == nil {
		err = s.target.CheckNoApp()
	}
	if err == nil {
		output := &lineWriter{emit: func(line string) { s.emit(BuildOutput{Line: line}) }}
		err = s.target.Build(s.ctx, env, output)
		output.flush()
	}
	if err != nil {
		s.emit(BuildFailed{Err: err, AppRunning: s.running() != nil})
		return
	}
	if s.ctx.Err() != nil {
		return
	}
	// Before the old app stops, so a failure here leaves it running.
	path, runEnv, err := s.target.Install(b, env)
	if err != nil {
		s.emit(BuildFailed{Err: err, AppRunning: s.running() != nil})
		return
	}
	if old := s.running(); old != nil {
		s.stop(old)
	}
	if err := s.start(b, path, runEnv); err != nil {
		s.emit(BuildFailed{Err: err})
	}
}

// running is the app that's up, if there is one.
func (s *Supervisor) running() *child {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.current
}

func (s *Supervisor) start(b board.Board, path string, runEnv []string) error {
	log, err := os.Create(s.target.LogPath())
	if err != nil {
		return err
	}
	defer log.Close()

	cmd := exec.Command(path)
	cmd.Env = runEnv
	if s.tracing() {
		cmd.Env = append(cmd.Env[:len(cmd.Env):len(cmd.Env)], "PREFLASH_TRACE=1")
	}
	cmd.Stdout = log
	cmd.Stderr = log
	// A group of its own, so that nothing the terminal does reaches it behind this
	// process's back: it stops when the supervisor says.
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}

	c := &child{
		cmd:      cmd,
		exited:   make(chan struct{}),
		watched:  make(chan struct{}),
		finished: make(chan struct{}),
	}
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	if err := cmd.Start(); err != nil {
		s.mu.Unlock()
		return fmt.Errorf("starting %s: %w", path, err)
	}
	s.current = c
	s.mu.Unlock()

	go func() {
		cmd.Wait() // the state it leaves says how it went
		close(c.exited)
	}()
	s.emit(Started{Board: b, PID: cmd.Process.Pid})
	go s.watch(c)
	go s.finish(c)
	return nil
}

// watch follows the app's stream from when its socket first answers until it has gone.
func (s *Supervisor) watch(c *child) {
	defer close(c.watched)
	detached := func() bool {
		select {
		case <-c.finished:
			return true
		default:
			return false
		}
	}
	watcher := client.Watcher{
		Connected: func(hello client.Hello) {
			c.connected.Store(true)
			s.emit(Connected{Hello: hello})
		},
		Serial: func(data []byte) error {
			if detached() {
				return errors.New("this app has been dealt with")
			}
			s.emit(Serial{Data: data})
			return nil
		},
		Dropped:    func(n int) { s.emit(Dropped{Bytes: n}) },
		Restarting: func() { s.emit(Restarting{}) },
	}
	for {
		err := client.Watch(s.target.SocketPath(), watcher)
		select {
		case <-c.exited:
			return
		default:
		}
		// Anything else, such as a protocol this build doesn't speak, won't get better.
		if err != nil && !errors.Is(err, client.ErrNotRunning) && !errors.Is(err, client.ErrStopped) {
			s.emit(SendFailed{Err: err})
			return
		}
		select {
		case <-c.exited:
			return
		case <-time.After(watchPause):
		}
	}
}

// finish reports the app's end, after the last of its stream.
func (s *Supervisor) finish(c *child) {
	<-c.exited
	select {
	case <-c.watched:
	case <-time.After(drainPatience):
	}
	s.mu.Lock()
	if s.current == c {
		s.current = nil
	}
	s.mu.Unlock()
	if !c.stopping.Load() {
		s.emit(s.ending(c))
	}
	close(c.finished)
}

func (s *Supervisor) ending(c *child) Exited {
	crashed, reason := describe(c.cmd.ProcessState)
	e := Exited{Crashed: crashed, Reason: reason}
	if !c.connected.Load() {
		e.Tail = lastLines(s.target.LogPath())
	}
	return e
}

// describe says how a process ended, and whether that counts as a crash.
func describe(state *os.ProcessState) (bool, string) {
	status, ok := state.Sys().(syscall.WaitStatus)
	if !ok || !status.Signaled() {
		if code := state.ExitCode(); code != 0 {
			return true, fmt.Sprintf("The app exited with code %d.", code)
		}
		return false, "The app exited."
	}
	switch signal := status.Signal(); signal {
	case syscall.SIGKILL:
		return false, "The app was killed."
	case syscall.SIGTERM, syscall.SIGINT, syscall.SIGHUP:
		return false, "The app was terminated."
	default:
		return true, "The app died: " + signal.String() + "."
	}
}

// lastLines gives the end of a log, for an app whose output never reached the socket.
func lastLines(path string) []string {
	file, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer file.Close()
	if info, err := file.Stat(); err == nil && info.Size() > tailBytes {
		if _, err := file.Seek(-tailBytes, io.SeekEnd); err != nil {
			return nil
		}
	}
	data, err := io.ReadAll(file)
	if err != nil {
		return nil
	}
	data = bytes.TrimRight(data, "\r\n")
	if len(data) == 0 {
		return nil
	}
	lines := strings.Split(string(data), "\n")
	return lines[max(len(lines)-tailLines, 0):]
}

// stop ends an app on purpose: a quit over the socket, then a signal, then a kill.
func (s *Supervisor) stop(c *child) {
	c.stopping.Store(true)
	go func() {
		conn, err := client.Dial(s.target.SocketPath())
		if err != nil {
			c.cmd.Process.Signal(syscall.SIGTERM)
			return
		}
		defer conn.Close()
		conn.Quit()
	}()
	if waitFor(c.finished, stopPatience) {
		return
	}
	c.cmd.Process.Signal(syscall.SIGTERM)
	if waitFor(c.finished, killPatience) {
		return
	}
	c.cmd.Process.Kill()
	<-c.finished
}

func waitFor(done <-chan struct{}, patience time.Duration) bool {
	select {
	case <-done:
		return true
	case <-time.After(patience):
		return false
	}
}

// Send queues console lines for the app, in order. What goes wrong arrives as SendFailed.
func (s *Supervisor) Send(lines ...string) {
	select {
	case s.requests <- request{lines: lines}:
	default:
		// Not from here: the caller can be Bubble Tea's loop, which is what reads events.
		go s.emit(SendFailed{Line: strings.Join(lines, "; "), Err: errors.New("too many lines are waiting")})
	}
}

// Status asks how the app stands. The answer arrives as a Status event, unless the app
// can't give one. A request already waiting covers this one.
func (s *Supervisor) Status() {
	if !s.asking.CompareAndSwap(false, true) {
		return
	}
	select {
	case s.requests <- request{}:
	default:
		s.asking.Store(false)
	}
}

// ask queues work for the control connection. A full queue costs a poll nothing, and anything
// else hears about it.
func (s *Supervisor) ask(r request) {
	select {
	case s.requests <- r:
	default:
		if r.failed != nil {
			go s.emit(r.failed(errors.New("too many requests are waiting")))
		}
	}
}

// Tasks asks what's running. The answer arrives as a Tasks event, unless the app can't give one.
func (s *Supervisor) Tasks() {
	s.ask(request{work: func(c *client.Client) (Event, error) {
		tasks, err := c.Tasks()
		return Tasks{tasks}, err
	}})
}

// NVS asks what's saved. The answer arrives as an NVS event, unless the app can't give one.
func (s *Supervisor) NVS() {
	s.ask(request{work: func(c *client.Client) (Event, error) {
		spaces, err := c.NVS()
		return NVS{spaces}, err
	}})
}

// Hardware asks how the board stands. The answer arrives as a Hardware event, unless the app
// can't give one.
func (s *Supervisor) Hardware() {
	s.ask(request{work: func(c *client.Client) (Event, error) {
		items, err := c.Hardware()
		return Hardware{items}, err
	}})
}

// SetHardware changes one thing on the board. A Hardware or HardwareFailed event follows.
func (s *Supervisor) SetHardware(name, value string) {
	s.ask(request{
		work: func(c *client.Client) (Event, error) {
			items, err := c.SetHardware(name, value)
			return Hardware{items}, err
		},
		failed: func(err error) Event { return HardwareFailed{err} },
	})
}

// SaveTrace has the app save what its ring holds. A TraceSaved or SendFailed event follows.
func (s *Supervisor) SaveTrace() {
	s.ask(request{
		work: func(c *client.Client) (Event, error) {
			path, records, err := c.SaveTrace("")
			return TraceSaved{path, records}, err
		},
		failed: func(err error) Event { return SendFailed{Err: err} },
	})
}

// Trace starts following the app's trace, which also turns it on in the app, or stops. Rows
// arrive as TraceRows events, across restarts and rebuilds, until it's told to stop.
func (s *Supervisor) Trace(on bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed || on == (s.traceStop != nil) {
		return
	}
	if !on {
		close(s.traceStop)
		s.traceStop = nil
		return
	}
	s.traceStop = make(chan struct{})
	s.workers.Add(1)
	go s.followTrace(s.traceStop)
}

func (s *Supervisor) tracing() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.traceStop != nil
}

// traceBuffer is what has come off the socket and not yet reached the screen.
type traceBuffer struct {
	mu    sync.Mutex
	rows  []client.TraceRecord
	reset bool
}

func (b *traceBuffer) begin() {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.rows = nil
	b.reset = true
}

func (b *traceBuffer) add(r client.TraceRecord) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if len(b.rows) >= traceHeld {
		b.rows = b.rows[1:]
	}
	b.rows = append(b.rows, r)
}

func (b *traceBuffer) take() (TraceRows, bool) {
	b.mu.Lock()
	defer b.mu.Unlock()
	rows := TraceRows{Rows: b.rows, Reset: b.reset}
	b.rows, b.reset = nil, false
	return rows, len(rows.Rows) > 0 || rows.Reset
}

func (s *Supervisor) followTrace(stop <-chan struct{}) {
	defer s.workers.Done()
	buffer := &traceBuffer{}
	var delivering sync.WaitGroup
	delivering.Add(1)
	go func() {
		defer delivering.Done()
		tick := time.NewTicker(traceEvery)
		defer tick.Stop()
		for {
			select {
			case <-stop:
				return
			case <-s.ctx.Done():
				return
			case <-tick.C:
				if rows, any := buffer.take(); any {
					s.emit(rows)
				}
			}
		}
	}()
	defer delivering.Wait()
	watcher := client.Watcher{
		Streams:   []string{"trace"},
		Stop:      stop,
		Connected: func(client.Hello) { buffer.begin() },
		Trace:     buffer.add,
	}
	for {
		err := client.Watch(s.target.SocketPath(), watcher)
		if err != nil && !errors.Is(err, client.ErrNotRunning) && !errors.Is(err, client.ErrStopped) {
			s.emit(SendFailed{Err: err})
		}
		select {
		case <-stop:
			return
		case <-s.ctx.Done():
			return
		case <-time.After(watchPause):
		}
	}
}

// control owns the second connection, which carries requests while the first carries the
// stream. Requests run one at a time, so lines keep their order.
func (s *Supervisor) control() {
	defer s.workers.Done()
	var conn *client.Client
	defer func() {
		if conn != nil {
			conn.Close()
		}
	}()
	// call runs work on a live connection, trying once more if the app went from under it,
	// as a restart does. An app that's merely slow may still have the request and act on it
	// later, so running out of time isn't worth a second try.
	call := func(work func(*client.Client) error) error {
		var err error
		for range 2 {
			if conn == nil {
				c := s.running()
				if c == nil {
					return errNotRunning
				}
				if conn, err = client.Await(s.target.SocketPath(), answerPatience, c.cmd.Process.Pid); err != nil {
					conn = nil
					return err
				}
			}
			if err = conn.SetDeadline(time.Now().Add(callPatience)); err == nil {
				err = work(conn)
			}
			if err == nil || !errors.Is(err, client.ErrStopped) {
				return err
			}
			conn.Close()
			conn = nil
			if errors.Is(err, os.ErrDeadlineExceeded) {
				return err
			}
		}
		return err
	}
	for {
		select {
		case <-s.ctx.Done():
			return
		case r := <-s.requests:
			if r.work != nil {
				var answer Event
				err := call(func(c *client.Client) (err error) {
					answer, err = r.work(c)
					return err
				})
				switch {
				case err == nil:
					s.emit(answer)
				case r.failed != nil:
					s.emit(r.failed(err))
				}
				continue
			}
			if r.lines == nil {
				s.asking.Store(false)
				var status client.Status
				err := call(func(c *client.Client) (err error) {
					status, err = c.Status()
					return err
				})
				if err == nil {
					s.emit(Status{status})
				}
				continue
			}
			for _, line := range r.lines {
				if err := call(func(c *client.Client) error { return c.Send(line) }); err != nil {
					s.emit(SendFailed{Line: line, Err: err})
				}
			}
		}
	}
}

// Close stops any build and any app, and returns once they've gone. Events stop arriving.
func (s *Supervisor) Close() {
	s.Trace(false)
	s.mu.Lock()
	s.closed = true
	s.mu.Unlock()
	s.cancel()
	s.builds.Wait()
	if c := s.running(); c != nil {
		s.stop(c)
	}
	s.workers.Wait()
}

// lineWriter turns a stream of bytes into lines.
type lineWriter struct {
	emit    func(string)
	partial []byte
}

func (w *lineWriter) Write(p []byte) (int, error) {
	w.partial = append(w.partial, p...)
	for {
		end := bytes.IndexByte(w.partial, '\n')
		if end < 0 {
			return len(p), nil
		}
		w.emit(string(bytes.TrimRight(w.partial[:end], "\r")))
		w.partial = w.partial[end+1:]
	}
}

// flush hands over a last line that never got its newline.
func (w *lineWriter) flush() {
	if len(w.partial) > 0 {
		w.emit(string(w.partial))
		w.partial = nil
	}
}
