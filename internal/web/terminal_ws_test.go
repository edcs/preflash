package web

import (
	"bytes"
	"context"
	"encoding/json"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/coder/websocket"
	"github.com/stretchr/testify/require"
)

// termSite is a server running the fake terminal app.
type termSite struct {
	srv    *Server
	addr   string
	cancel context.CancelFunc
	done   chan struct{}
	served error
}

// startTermSite serves the fake app with this behaviour, and a control socket if one is given.
func startTermSite(t *testing.T, behaviour, socket string) *termSite {
	t.Helper()
	t.Setenv("PREFLASH_WEB_FAKE", behaviour)
	t.Setenv("GORACE", "atexit_sleep_ms=0")
	was := closePatience
	closePatience = time.Second
	t.Cleanup(func() { closePatience = was })

	srv := New(Options{Socket: socket, Terminal: []string{os.Args[0]}})
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	ctx, cancel := context.WithCancel(context.Background())
	s := &termSite{srv: srv, addr: ln.Addr().String(), cancel: cancel, done: make(chan struct{})}
	go func() {
		s.served = srv.Serve(ctx, ln)
		close(s.done)
	}()
	t.Cleanup(s.stop)
	return s
}

// stop ends Serve and waits for it.
func (s *termSite) stop() {
	s.cancel()
	<-s.done
}

// tab is a browser's side of /terminal. It reads in the background and keeps the bytes.
type tab struct {
	t       *testing.T
	conn    *websocket.Conn
	texts   chan map[string]any
	first   chan []byte // the first binary message
	mu      sync.Mutex
	got     string // every binary message so far
	log     []string
	arrived chan struct{}
	closed  chan error
}

func (s *termSite) open(t *testing.T) *tab {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, "ws://"+s.addr+"/terminal", &websocket.DialOptions{
		HTTPHeader: http.Header{"Origin": {"http://" + s.addr}},
	})
	require.NoError(t, err)
	conn.SetReadLimit(1 << 24)
	b := &tab{t: t, conn: conn, texts: make(chan map[string]any, 256), first: make(chan []byte, 1),
		arrived: make(chan struct{}, 1), closed: make(chan error, 1)}
	t.Cleanup(func() { conn.CloseNow() })
	go func() {
		firstSeen := false
		for {
			kind, data, err := conn.Read(context.Background())
			if err != nil {
				b.closed <- err
				return
			}
			if kind == websocket.MessageBinary {
				if !firstSeen {
					firstSeen = true
					b.first <- data
				}
				b.mu.Lock()
				b.got += string(data)
				b.log = append(b.log, "bin:"+string(data))
				b.mu.Unlock()
				select {
				case b.arrived <- struct{}{}:
				default:
				}
				continue
			}
			var m map[string]any
			if json.Unmarshal(data, &m) == nil {
				b.mu.Lock()
				b.log = append(b.log, "text:"+m["type"].(string))
				b.mu.Unlock()
				b.texts <- m
			}
		}
	}()
	return b
}

func (b *tab) text(m map[string]any) {
	b.t.Helper()
	data, err := json.Marshal(m)
	require.NoError(b.t, err)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	require.NoError(b.t, b.conn.Write(ctx, websocket.MessageText, data))
}

func (b *tab) keys(s string) {
	b.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	require.NoError(b.t, b.conn.Write(ctx, websocket.MessageBinary, []byte(s)))
}

// until waits for the output so far to contain want.
func (b *tab) until(want string) string {
	b.t.Helper()
	deadline := time.After(termPatience)
	for {
		b.mu.Lock()
		got := b.got
		b.mu.Unlock()
		if strings.Contains(got, want) {
			return got
		}
		select {
		case <-b.arrived:
		case <-deadline:
			require.FailNow(b.t, "output never contained "+want, tail(got))
		}
	}
}

// next waits for a text message of the given type.
func (b *tab) next(typ string) {
	b.t.Helper()
	deadline := time.After(termPatience)
	for {
		select {
		case m := <-b.texts:
			if m["type"] == typ {
				return
			}
		case <-deadline:
			require.FailNow(b.t, "no message of that type arrived", typ)
		}
	}
}

func TestATabSeesTheScreenThenLiveOutput(t *testing.T) {
	site := startTermSite(t, "echo", "")
	first := site.open(t)
	first.until("ready 120x40")

	late := site.open(t)
	select {
	case screen := <-late.first:
		require.Contains(t, string(screen), "ready 120x40", "the first message redraws the screen")
	case <-time.After(termPatience):
		t.Fatal("no screen arrived")
	}

	late.keys("hi\r")
	late.until("echo:hi")
	first.until("echo:hi")
}

func TestATabSeesTheAppExitAndStartAgain(t *testing.T) {
	site := startTermSite(t, "exit-on-ctrl-c", "")
	one, two := site.open(t), site.open(t)
	one.until("ready")
	pid := site.srv.term.pid()

	one.keys("\x03")
	one.next("exited")
	two.next("exited")

	// A tab that arrives after the exit is told at once.
	late := site.open(t)
	late.next("exited")

	one.text(map[string]any{"type": "start"})
	one.next("started")
	two.next("started")
	late.next("started")
	require.NotEqual(t, pid, site.srv.term.pid(), "the app runs again")
	one.until("ready")
	two.until("ready")
}

func TestStartWhileRunningIsIgnored(t *testing.T) {
	site := startTermSite(t, "echo", "")
	b := site.open(t)
	b.until("ready")
	pid := site.srv.term.pid()

	b.text(map[string]any{"type": "start"})
	b.keys("x\r")
	b.until("echo:x")
	require.Equal(t, pid, site.srv.term.pid())
	select {
	case m := <-b.texts:
		t.Fatalf("unexpected message %v", m)
	default:
	}
}

func TestResizeReachesTheApp(t *testing.T) {
	site := startTermSite(t, "echo", "")
	b := site.open(t)
	b.until("ready")
	b.text(map[string]any{"type": "resize", "cols": 100, "rows": 30})
	b.until("size:100x30")

	// A size the terminal refuses is answered with an error and the tab stays.
	b.text(map[string]any{"type": "resize", "cols": 0, "rows": 30})
	b.next("error")
	b.keys("ok\r")
	b.until("echo:ok")
}

func TestServeEndingStopsTheTerminal(t *testing.T) {
	site := startTermSite(t, "echo", "")
	require.Eventually(t, func() bool { return site.srv.term.Running() }, termPatience, 5*time.Millisecond)
	pid := site.srv.term.pid()

	site.stop()
	require.NoError(t, site.served)
	require.True(t, gone(pid), "the app is gone once Serve returns")
}

func TestServeEndingClosesTheTabs(t *testing.T) {
	site := startTermSite(t, "echo", "")
	b := site.open(t)
	b.until("ready")
	site.stop()
	select {
	case <-b.closed:
	case <-time.After(termPatience):
		t.Fatal("the tab stayed connected")
	}
}

func TestServeEndingAsksAnAppThatSurvivedToQuit(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 172, 640)
	site := startTermSite(t, "ignore-ctrl-c", socket)
	b := site.open(t)
	b.until(" ok")
	pid := site.srv.term.pid()

	site.stop()
	require.True(t, gone(pid))
	require.Equal(t, 1, app.count("quit"), "the firmware app was asked to quit")
}

func TestWithoutATerminalTheTabIsToldWhy(t *testing.T) {
	srv := New(Options{})
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		srv.Serve(ctx, ln)
		close(done)
	}()
	t.Cleanup(func() { cancel(); <-done })

	dial, stop := context.WithTimeout(context.Background(), 3*time.Second)
	defer stop()
	conn, _, err := websocket.Dial(dial, "ws://"+ln.Addr().String()+"/terminal", &websocket.DialOptions{
		HTTPHeader: http.Header{"Origin": {"http://" + ln.Addr().String()}},
	})
	require.NoError(t, err)
	defer conn.CloseNow()
	_, _, err = conn.Read(dial)
	require.Equal(t, websocket.StatusNormalClosure, websocket.CloseStatus(err))
	require.Contains(t, err.Error(), "no terminal app")
}

func TestServeEndingDoesNotWaitForeverForTheApp(t *testing.T) {
	was := quitPatience
	quitPatience = 1500 * time.Millisecond
	var said bytes.Buffer
	wasErr := stderr
	stderr = &said
	t.Cleanup(func() { quitPatience, stderr = was, wasErr })

	socket := shortPath(t)
	app := startFakeApp(t, socket, 172, 640)
	app.mu.Lock()
	app.stubborn = true
	app.mu.Unlock()
	site := startTermSite(t, "ignore-ctrl-c", socket)
	site.open(t).until(" ok")

	start := time.Now()
	site.stop()
	// closePatience (1 s) and the dial wait (1 s) come before the quit's own limit.
	require.Less(t, time.Since(start), 1*time.Second+quitPatience+2*time.Second)
	require.Equal(t, 1, app.count("quit"))
	require.Contains(t, said.String(), "the app didn't stop")
	require.Contains(t, said.String(), "pid 7")
}

// events is every message a tab has had, in the order it came, as "text:<type>" or "bin:<data>".
func (b *tab) events() []string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return append([]string(nil), b.log...)
}

func TestStartedComesBeforeTheNewRunsOutput(t *testing.T) {
	site := startTermSite(t, "exit-on-ctrl-c", "")
	b := site.open(t)
	b.until("ready")
	for range 5 {
		b.keys("\x03")
		b.next("exited")
		b.text(map[string]any{"type": "start"})
		b.next("started")
	}
	b.until("ready")
	// After each exited, no output may come until the started that follows.
	waiting := false
	for _, e := range b.events() {
		switch {
		case e == "text:exited":
			waiting = true
		case e == "text:started":
			waiting = false
		case waiting && strings.HasPrefix(e, "bin:"):
			t.Fatalf("output %q came between exited and started", e)
		}
	}
}

func TestATabThatStopsAnsweringIsDetached(t *testing.T) {
	quickPings(t)
	site := startTermSite(t, "echo", "")
	dialSilent(t, site.addr, "/terminal")
	require.Eventually(t, func() bool { return site.srv.term.attached() == 1 }, termPatience, 5*time.Millisecond)
	require.Eventually(t, func() bool { return site.srv.term.attached() == 0 }, 2*time.Second, 5*time.Millisecond,
		"the tab stayed attached")
}

func TestATabHearsAPing(t *testing.T) {
	quickPings(t)
	site := startTermSite(t, "echo", "")
	site.open(t).next("ping")
}
