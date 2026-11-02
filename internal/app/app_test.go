package app

import (
	"bufio"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/edcs/preflash/internal/board"
)

const patience = 5 * time.Second

// fakeTarget builds by calling build, and runs a shell script as the app.
type fakeTarget struct {
	dir        string
	script     string
	build      func(out io.Writer) error
	noApp      error
	mu         sync.Mutex
	boards     []string
	builds     int
	headles    bool
	installErr error
}

func newTarget(t *testing.T, script string) *fakeTarget {
	t.Helper()
	dir, err := os.MkdirTemp("/tmp", "pf")
	require.NoError(t, err)
	t.Cleanup(func() { os.RemoveAll(dir) })
	return &fakeTarget{dir: dir, script: script}
}

func (f *fakeTarget) Environment(headless bool) ([]string, error) {
	f.headles = headless
	return []string{"PATH=" + os.Getenv("PATH"), "FROM_TARGET=1"}, nil
}
func (f *fakeTarget) CheckNoApp() error { return f.noApp }
func (f *fakeTarget) Build(ctx context.Context, env []string, out io.Writer) error {
	f.mu.Lock()
	f.builds++
	build := f.build
	f.mu.Unlock()
	if build != nil {
		return build(out)
	}
	return nil
}
func (f *fakeTarget) Install(b board.Board, env []string) (string, []string, error) {
	if f.installErr != nil {
		return "", nil, f.installErr
	}
	path := filepath.Join(f.dir, "app")
	if err := os.WriteFile(path, []byte("#!/bin/sh\n"+f.script+"\n"), 0o755); err != nil {
		return "", nil, err
	}
	f.mu.Lock()
	f.boards = append(f.boards, b.Name)
	f.mu.Unlock()
	return path, env, nil
}
func (f *fakeTarget) SocketPath() string { return filepath.Join(f.dir, "s") }
func (f *fakeTarget) LogPath() string    { return filepath.Join(f.dir, "app.log") }

func (f *fakeTarget) setBuild(build func(io.Writer) error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.build = build
}

func (f *fakeTarget) log(t *testing.T) string {
	t.Helper()
	data, err := os.ReadFile(f.LogPath())
	require.NoError(t, err)
	return string(data)
}

// fakeSocket answers as a running app does, for the process pid.
type fakeSocket struct {
	listener net.Listener
	pid      func() int
	quit     func()
	mu       sync.Mutex
	sent     []string
	subs     []net.Conn
}

func serveSocket(t *testing.T, path string, pid func() int, quit func()) *fakeSocket {
	t.Helper()
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	s := &fakeSocket{listener: listener, pid: pid, quit: quit}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go s.serve(conn)
		}
	}()
	return s
}

func (s *fakeSocket) serve(conn net.Conn) {
	lines := bufio.NewScanner(conn)
	for lines.Scan() {
		var req map[string]any
		if json.Unmarshal(lines.Bytes(), &req) != nil {
			return
		}
		id := int(req["id"].(float64))
		switch req["op"] {
		case "hello":
			fmt.Fprintf(conn, `{"id":%d,"ok":true,"protocol":1,"project":"demo","board":"B","pid":%d}`+"\n", id, s.pid())
		case "status":
			fmt.Fprintf(conn, `{"id":%d,"ok":true,"backlight":70,"fps":60}`+"\n", id)
		case "send":
			s.mu.Lock()
			s.sent = append(s.sent, req["line"].(string))
			s.mu.Unlock()
			fmt.Fprintf(conn, `{"id":%d,"ok":true}`+"\n", id)
		case "subscribe":
			fmt.Fprintf(conn, `{"id":%d,"ok":true}`+"\n", id)
			s.mu.Lock()
			s.subs = append(s.subs, conn)
			s.mu.Unlock()
		case "quit":
			fmt.Fprintf(conn, `{"id":%d,"ok":true}`+"\n", id)
			s.quit()
		}
	}
	conn.Close()
}

func (s *fakeSocket) write(line string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, conn := range s.subs {
		fmt.Fprintln(conn, line)
	}
}

func (s *fakeSocket) lines() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string(nil), s.sent...)
}

// next waits for an event of type E, keeping the ones before it in seen.
func next[E any](t *testing.T, s *Supervisor, seen *[]Event) E {
	t.Helper()
	deadline := time.After(patience)
	for {
		select {
		case e := <-s.Events():
			if seen != nil {
				*seen = append(*seen, e)
			}
			if found, ok := e.(E); ok {
				return found
			}
		case <-deadline:
			var none E
			t.Fatalf("no %T arrived", none)
			return none
		}
	}
}

// quiet fails if anything but serial turns up for a moment.
func quiet(t *testing.T, s *Supervisor, wait time.Duration) {
	t.Helper()
	select {
	case e := <-s.Events():
		t.Fatalf("unexpected event %#v", e)
	case <-time.After(wait):
	}
}

func alive(pid int) bool { return syscall.Kill(pid, 0) == nil }

func eventually(t *testing.T, what string, check func() bool) {
	t.Helper()
	deadline := time.Now().Add(patience)
	for !check() {
		if time.Now().After(deadline) {
			t.Fatal(what)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

var testBoard = board.Board{Name: "the board"}

func started(t *testing.T, f *fakeTarget) (*Supervisor, *fakeSocket, Started) {
	t.Helper()
	s := New(f, true)
	t.Cleanup(s.Close)
	require.True(t, s.Run(testBoard))
	begun := next[Started](t, s, nil)
	socket := serveSocket(t, f.SocketPath(), func() int { return begun.PID }, func() {
		syscall.Kill(begun.PID, syscall.SIGKILL)
	})
	next[Connected](t, s, nil)
	return s, socket, begun
}

func TestRunBuildsThenStartsTheAppAndFollowsIt(t *testing.T) {
	f := newTarget(t, "read line; echo stdin-ended-$?; echo to-stderr >&2; echo from-$FROM_TARGET; exec sleep 30")
	f.build = func(out io.Writer) error {
		fmt.Fprint(out, "compiling\nlinking")
		return nil
	}
	s := New(f, true)
	t.Cleanup(s.Close)

	require.True(t, s.Run(testBoard))

	var seen []Event
	begun := next[Started](t, s, &seen)
	assert.Equal(t, "the board", begun.Board.Name)
	assert.True(t, f.headles)
	require.Len(t, seen, 4)
	assert.Equal(t, Building{Board: testBoard}, seen[0])
	assert.Equal(t, BuildOutput{Line: "compiling"}, seen[1])
	assert.Equal(t, BuildOutput{Line: "linking"}, seen[2])

	socket := serveSocket(t, f.SocketPath(), func() int { return begun.PID }, func() {
		syscall.Kill(begun.PID, syscall.SIGKILL)
	})
	connected := next[Connected](t, s, nil)
	assert.Equal(t, "demo", connected.Hello.Project)

	eventually(t, "the subscription never came", func() bool {
		socket.mu.Lock()
		defer socket.mu.Unlock()
		return len(socket.subs) == 1
	})
	socket.write(`{"event":"serial","data":"` + base64.StdEncoding.EncodeToString([]byte("boot\n")) + `"}`)
	assert.Equal(t, "boot\n", string(next[Serial](t, s, nil).Data))
	socket.write(`{"event":"dropped","bytes":12}`)
	assert.Equal(t, 12, next[Dropped](t, s, nil).Bytes)

	eventually(t, "the app's output never reached the log", func() bool {
		log := f.log(t)
		return strings.Contains(log, "stdin-ended-1") && strings.Contains(log, "to-stderr") && strings.Contains(log, "from-1")
	})
}

func TestSendGoesOutInOrder(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	s, socket, _ := started(t, f)

	s.Send("one", "two")
	s.Send("three")

	eventually(t, "the lines never arrived", func() bool { return len(socket.lines()) == 3 })
	assert.Equal(t, []string{"one", "two", "three"}, socket.lines())
}

func TestStatusReports(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	s, _, _ := started(t, f)

	s.Status()

	assert.Equal(t, 60, next[Status](t, s, nil).FPS)
}

func TestSendWithNothingRunningSaysSo(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	s := New(f, true)
	t.Cleanup(s.Close)

	s.Send("hello")

	failed := next[SendFailed](t, s, nil)
	assert.Equal(t, "hello", failed.Line)
	assert.ErrorContains(t, failed.Err, "not running")
}

func TestAFailedBuildWithNoAppSaysSo(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	f.build = func(io.Writer) error { return errors.New("nope") }
	s := New(f, true)
	t.Cleanup(s.Close)

	require.True(t, s.Run(testBoard))

	failed := next[BuildFailed](t, s, nil)
	assert.EqualError(t, failed.Err, "nope")
	assert.False(t, failed.AppRunning)
	assert.Empty(t, f.boards, "nothing should start")
}

func TestAFailedBuildKeepsTheOldAppRunning(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	s, _, begun := started(t, f)
	f.setBuild(func(out io.Writer) error {
		fmt.Fprintln(out, "main.cpp:1: error")
		return errors.New("nope")
	})

	require.True(t, s.Run(board.Board{Name: "another"}))

	var seen []Event
	failed := next[BuildFailed](t, s, &seen)
	assert.True(t, failed.AppRunning)
	assert.Contains(t, seen, Event(BuildOutput{Line: "main.cpp:1: error"}))
	assert.True(t, alive(begun.PID))
	assert.Equal(t, []string{"the board"}, f.boards)
}

func TestARebuildReplacesTheApp(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	s, _, first := started(t, f)

	require.True(t, s.Run(board.Board{Name: "another"}))

	var seen []Event
	second := next[Started](t, s, &seen)
	assert.Equal(t, "another", second.Board.Name)
	assert.NotEqual(t, first.PID, second.PID)
	assert.False(t, alive(first.PID), "the old app should have stopped first")
	for _, e := range seen {
		_, exited := e.(Exited)
		assert.False(t, exited, "a replaced app isn't a stopped one")
	}
}

func TestOnlyOneBuildRunsAtATime(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	release := make(chan struct{})
	f.build = func(io.Writer) error {
		<-release
		return nil
	}
	s := New(f, true)
	t.Cleanup(s.Close)
	require.True(t, s.Run(testBoard))
	next[Building](t, s, nil)

	assert.False(t, s.Run(testBoard))

	close(release)
	next[Started](t, s, nil)
	assert.Equal(t, 1, f.builds)
}

func TestAnAppAlreadyThereStopsARun(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	f.noApp = errors.New("demo (pid 1) already holds the socket")
	s := New(f, true)
	t.Cleanup(s.Close)

	require.True(t, s.Run(testBoard))

	assert.ErrorContains(t, next[BuildFailed](t, s, nil).Err, "already holds")
	assert.Zero(t, f.builds, "a build would change what's under that app")
}

func TestItsOwnAppDoesNotStopARebuild(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	s, _, _ := started(t, f)
	f.noApp = errors.New("demo (pid 1) already holds the socket")

	require.True(t, s.Run(testBoard))

	next[Started](t, s, nil)
}

func TestARestartIsPassedOn(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	s, socket, _ := started(t, f)
	eventually(t, "no subscription", func() bool {
		socket.mu.Lock()
		defer socket.mu.Unlock()
		return len(socket.subs) == 1
	})

	socket.write(`{"event":"restarting"}`)

	next[Restarting](t, s, nil)
}

func TestHowTheAppStoppedDecidesTheState(t *testing.T) {
	for name, test := range map[string]struct {
		script  string
		crashed bool
		reason  string
	}{
		"clean exit":  {"exit 0", false, "The app exited."},
		"exit code":   {"exit 3", true, "The app exited with code 3."},
		"segfault":    {"kill -SEGV $$; sleep 5", true, "The app died: segmentation fault."},
		"abort":       {"kill -ABRT $$; sleep 5", true, "The app died: abort trap."},
		"killed":      {"kill -KILL $$; sleep 5", false, "The app was killed."},
		"interrupted": {"kill -TERM $$; sleep 5", false, "The app was terminated."},
	} {
		t.Run(name, func(t *testing.T) {
			f := newTarget(t, test.script)
			s := New(f, true)
			t.Cleanup(s.Close)

			require.True(t, s.Run(testBoard))

			exited := next[Exited](t, s, nil)
			assert.Equal(t, test.crashed, exited.Crashed)
			assert.Equal(t, test.reason, exited.Reason)
		})
	}
}

func TestAnAppThatNeverConnectedLeavesItsLastOutput(t *testing.T) {
	f := newTarget(t, "echo preflash: no window here >&2; exit 1")
	s := New(f, true)
	t.Cleanup(s.Close)

	require.True(t, s.Run(testBoard))

	exited := next[Exited](t, s, nil)
	assert.True(t, exited.Crashed)
	assert.Equal(t, []string{"preflash: no window here"}, exited.Tail)
}

func TestAnAppThatConnectedLeavesNoTail(t *testing.T) {
	f := newTarget(t, "echo already seen on the socket; exec sleep 30")
	s, _, begun := started(t, f)

	syscall.Kill(begun.PID, syscall.SIGSEGV)

	assert.Empty(t, next[Exited](t, s, nil).Tail)
}

func TestARunAfterAnExitStartsAgain(t *testing.T) {
	f := newTarget(t, "exit 0")
	s := New(f, true)
	t.Cleanup(s.Close)
	require.True(t, s.Run(testBoard))
	next[Exited](t, s, nil)

	require.True(t, s.Run(testBoard))

	next[Exited](t, s, nil)
	assert.Equal(t, 2, f.builds)
}

func TestCloseAsksTheAppToQuit(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	var quits sync.WaitGroup
	quits.Add(1)
	s := New(f, true)
	require.True(t, s.Run(testBoard))
	begun := next[Started](t, s, nil)
	serveSocket(t, f.SocketPath(), func() int { return begun.PID }, func() {
		quits.Done()
		syscall.Kill(begun.PID, syscall.SIGKILL)
	})
	next[Connected](t, s, nil)

	s.Close()

	quits.Wait()
	assert.False(t, alive(begun.PID))
}

func TestCloseSignalsAnAppWithNoSocket(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	s := New(f, true)
	require.True(t, s.Run(testBoard))
	begun := next[Started](t, s, nil)

	s.Close()

	assert.False(t, alive(begun.PID))
}

func TestCloseKillsAnAppThatIgnoresEverything(t *testing.T) {
	f := newTarget(t, "trap '' TERM HUP INT; while :; do sleep 1; done")
	stopPatience, killPatience = 50*time.Millisecond, 100*time.Millisecond
	s := New(f, true)
	require.True(t, s.Run(testBoard))
	begun := next[Started](t, s, nil)
	time.Sleep(100 * time.Millisecond) // the trap must be in place

	s.Close()

	assert.False(t, alive(begun.PID))
}

func TestCloseStopsABuild(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	ended := make(chan struct{})
	f.build = func(io.Writer) error {
		time.Sleep(100 * time.Millisecond)
		close(ended)
		return context.Canceled
	}
	s := New(f, true)
	require.True(t, s.Run(testBoard))

	s.Close()

	<-ended
	assert.Empty(t, f.boards)
	assert.False(t, s.Run(testBoard), "nothing runs after Close")
}

func TestLineWriterJoinsPiecesAndKeepsTheLastPartialLine(t *testing.T) {
	var lines []string
	w := &lineWriter{emit: func(line string) { lines = append(lines, line) }}

	fmt.Fprint(w, "one\ntw")
	fmt.Fprint(w, "o\r\n\nthree")
	w.flush()
	w.flush()

	assert.Equal(t, []string{"one", "two", "", "three"}, lines)
}

func TestAFullQueueDoesNotBlockTheCaller(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	s := New(f, true)
	t.Cleanup(s.Close)
	returned := make(chan struct{})

	go func() {
		for range 3 * requestsQueued {
			s.Send("line")
		}
		close(returned)
	}()

	// Nobody reads Events here, as nobody does while Bubble Tea is in Update.
	select {
	case <-returned:
	case <-time.After(patience):
		t.Fatal("Send blocked on the event stream")
	}
}

func TestALineThatTimesOutIsNotSentAgain(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	callPatience = 100 * time.Millisecond
	t.Cleanup(func() { callPatience = 2 * time.Second })
	s := New(f, true)
	t.Cleanup(s.Close)
	require.True(t, s.Run(testBoard))
	begun := next[Started](t, s, nil)

	listener, err := net.Listen("unix", f.SocketPath())
	require.NoError(t, err)
	t.Cleanup(func() { listener.Close() })
	var mu sync.Mutex
	var sends int
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				lines := bufio.NewScanner(conn)
				for lines.Scan() {
					var req map[string]any
					if json.Unmarshal(lines.Bytes(), &req) != nil {
						return
					}
					switch req["op"] {
					case "hello":
						fmt.Fprintf(conn, `{"id":%d,"ok":true,"protocol":1,"pid":%d}`+"\n", int(req["id"].(float64)), begun.PID)
					case "send": // heard, and never answered, as an app stopped in a debugger does
						mu.Lock()
						sends++
						mu.Unlock()
					}
				}
			}()
		}
	}()

	s.Send("restart")

	failed := next[SendFailed](t, s, nil)
	assert.Equal(t, "restart", failed.Line)
	time.Sleep(300 * time.Millisecond)
	mu.Lock()
	defer mu.Unlock()
	assert.Equal(t, 1, sends)
}

func TestAFailedInstallKeepsTheOldAppRunning(t *testing.T) {
	f := newTarget(t, "exec sleep 30")
	s, _, begun := started(t, f)
	f.installErr = errors.New("the build made no app")

	require.True(t, s.Run(testBoard))

	failed := next[BuildFailed](t, s, nil)
	assert.True(t, failed.AppRunning)
	assert.True(t, alive(begun.PID))
}

// fakeTraceApp answers a subscribe to the trace with one row, and the debug ops.
func fakeTraceApp(t *testing.T, path string) (subscribed chan struct{}) {
	t.Helper()
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	t.Cleanup(func() { listener.Close() })
	subscribed = make(chan struct{}, 8)
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				scanner := bufio.NewScanner(conn)
				for scanner.Scan() {
					var req map[string]any
					if json.Unmarshal(scanner.Bytes(), &req) != nil {
						return
					}
					id := int(req["id"].(float64))
					switch req["op"] {
					case "hello":
						fmt.Fprintf(conn, `{"id":%d,"ok":true,"protocol":1,"project":"demo","board":"B","pid":%d}`+"\n", id, os.Getpid())
					case "subscribe":
						fmt.Fprintf(conn, `{"id":%d,"ok":true}`+"\n", id)
						if streams, _ := req["streams"].([]any); len(streams) == 1 && streams[0] == "trace" {
							fmt.Fprintln(conn, `{"event":"trace","stream":"net","seq":0,"us":1,"subject":"wifi","op":"join","detail":"home","injected":false}`)
							subscribed <- struct{}{}
						}
					case "hardware_set":
						if req["name"] == "bad" {
							fmt.Fprintf(conn, `{"id":%d,"ok":false,"error":"no hardware called bad"}`+"\n", id)
							continue
						}
						fmt.Fprintf(conn, `{"id":%d,"ok":true,"items":[{"name":"rtc"}]}`+"\n", id)
					case "tasks":
						fmt.Fprintf(conn, `{"id":%d,"ok":true,"tasks":[{"name":"main"}]}`+"\n", id)
					case "nvs":
						fmt.Fprintf(conn, `{"id":%d,"ok":true,"namespaces":[{"name":"wifi"}]}`+"\n", id)
					case "trace_save":
						fmt.Fprintf(conn, `{"id":%d,"ok":true,"path":"/saved","records":3}`+"\n", id)
					default:
						fmt.Fprintf(conn, `{"id":%d,"ok":true}`+"\n", id)
					}
				}
			}()
		}
	}()
	return subscribed
}

func TestTheTraceFollowsOnlyWhileAsked(t *testing.T) {
	target := newTarget(t, "sleep 30")
	subscribed := fakeTraceApp(t, target.SocketPath())
	s := New(target, true)
	defer s.Close()
	require.True(t, s.Run(board.Board{Name: "b"}))
	next[Connected](t, s, nil)

	s.Trace(true)
	rows := next[TraceRows](t, s, nil)
	assert.True(t, rows.Reset)
	require.Len(t, rows.Rows, 1)
	assert.Equal(t, "join", rows.Rows[0].Op)
	<-subscribed

	s.Trace(false)
	s.Trace(true)
	rows = next[TraceRows](t, s, nil)
	assert.True(t, rows.Reset, "asking again replays the ring")
}

func TestAnAppStartedWhileTracingIsToldToTraceFromBoot(t *testing.T) {
	target := newTarget(t, `echo "trace=$PREFLASH_TRACE"; sleep 30`)
	fakeTraceApp(t, target.SocketPath())
	s := New(target, true)
	defer s.Close()
	s.Trace(true)
	require.True(t, s.Run(board.Board{Name: "b"}))
	next[TraceRows](t, s, nil)
	eventually(t, "the app's output", func() bool { return strings.Contains(target.log(t), "trace=1") })
}

func TestDebugRequestsAnswerAsEvents(t *testing.T) {
	target := newTarget(t, "sleep 30")
	fakeTraceApp(t, target.SocketPath())
	s := New(target, true)
	defer s.Close()
	require.True(t, s.Run(board.Board{Name: "b"}))
	next[Connected](t, s, nil)

	s.Tasks()
	assert.Equal(t, "main", next[Tasks](t, s, nil).Tasks[0].Name)
	s.NVS()
	assert.Equal(t, "wifi", next[NVS](t, s, nil).Namespaces[0].Name)
	s.SetHardware("rtc", "flat")
	assert.Equal(t, "rtc", next[Hardware](t, s, nil).Items[0].Name)
	s.SetHardware("bad", "x")
	assert.ErrorContains(t, next[HardwareFailed](t, s, nil).Err, "no hardware called bad")
	s.SaveTrace()
	saved := next[TraceSaved](t, s, nil)
	assert.Equal(t, "/saved", saved.Path)
	assert.Equal(t, 3, saved.Records)
}

func TestAnAppTheTraceCantTalkToIsReportedOnce(t *testing.T) {
	target := newTarget(t, "sleep 30")
	listener, err := net.Listen("unix", target.SocketPath())
	require.NoError(t, err)
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				lines := bufio.NewScanner(conn)
				for lines.Scan() {
					fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":99,"project":"demo","board":"B","pid":1}`)
				}
			}()
		}
	}()
	s := New(target, true)
	defer s.Close()
	s.Trace(true)
	assert.ErrorContains(t, next[SendFailed](t, s, nil).Err, "protocol")
	quiet(t, s, 500*time.Millisecond)
}
