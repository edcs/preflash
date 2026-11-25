package web

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/edcs/preflash/internal/client"
	"github.com/stretchr/testify/require"
)

// A socket path short enough for macOS, which t.TempDir's isn't. TMPDIR decides where.
func shortPath(t *testing.T) string {
	t.Helper()
	dir, err := os.MkdirTemp("", "pf")
	require.NoError(t, err)
	t.Cleanup(func() { os.RemoveAll(dir) })
	return filepath.Join(dir, "s")
}

// fakeConn lets a push and a reply write to one connection without tearing each other's lines.
type fakeConn struct {
	net.Conn
	mu sync.Mutex
}

func (c *fakeConn) line(s string) time.Duration {
	c.mu.Lock()
	defer c.mu.Unlock()
	start := time.Now()
	fmt.Fprintln(c.Conn, s)
	return time.Since(start)
}

// fakeApp is an app's control socket with a panel of a set size. It answers hello, subscribe,
// frame and status, and can push frame events and hang up.
type fakeApp struct {
	listener net.Listener
	width    int
	height   int

	mu       sync.Mutex
	conns    []*fakeConn
	subs     []*fakeConn
	frame    uint64
	buttons  []client.ButtonState
	ops      []string
	maxWrite time.Duration
}

func startFakeApp(t *testing.T, path string, width, height int) *fakeApp {
	t.Helper()
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	app := &fakeApp{listener: listener, width: width, height: height,
		buttons: []client.ButtonState{{Name: "BOOT", Wiring: "GPIO0"}}}
	t.Cleanup(app.hangUp)
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			c := &fakeConn{Conn: conn}
			app.mu.Lock()
			app.conns = append(app.conns, c)
			app.mu.Unlock()
			go app.serve(c)
		}
	}()
	return app
}

func (a *fakeApp) serve(c *fakeConn) {
	defer c.Close()
	scanner := bufio.NewScanner(c)
	for scanner.Scan() {
		var req map[string]any
		if json.Unmarshal(scanner.Bytes(), &req) != nil {
			return
		}
		id := int(req["id"].(float64))
		op, _ := req["op"].(string)
		a.mu.Lock()
		a.ops = append(a.ops, op)
		a.mu.Unlock()
		switch op {
		case "hello":
			c.line(fmt.Sprintf(`{"id":%d,"ok":true,"protocol":1,"project":"demo","board":"B","pid":7}`, id))
		case "subscribe":
			a.mu.Lock()
			a.subs = append(a.subs, c)
			a.mu.Unlock()
			c.line(fmt.Sprintf(`{"id":%d,"ok":true}`, id))
		case "frame":
			a.mu.Lock()
			number := a.frame
			a.mu.Unlock()
			// A picture whose pixels are all one colour is enough to check the size.
			data, _ := json.Marshal(make([]byte, a.width*a.height*2))
			c.line(fmt.Sprintf(`{"id":%d,"ok":true,"width":%d,"height":%d,"format":"rgb565le","data":%s,"frame":%d}`,
				id, a.width, a.height, data, number))
		case "status":
			a.mu.Lock()
			status := client.Status{Project: "demo", Board: "B", Width: a.width, Height: a.height,
				Frames: a.frame, Power: "on", Buttons: append([]client.ButtonState(nil), a.buttons...)}
			a.mu.Unlock()
			body, _ := json.Marshal(status)
			c.line(fmt.Sprintf(`{"id":%d,"ok":true,%s`, id, body[1:]))
		default:
			c.line(fmt.Sprintf(`{"id":%d,"ok":true}`, id))
		}
	}
}

// push makes n the newest frame and tells every subscriber.
func (a *fakeApp) push(n uint64) {
	a.mu.Lock()
	a.frame = n
	subs := append([]*fakeConn(nil), a.subs...)
	a.mu.Unlock()
	for _, c := range subs {
		took := c.line(fmt.Sprintf(`{"event":"frame","frame":%d}`, n))
		a.mu.Lock()
		a.maxWrite = max(a.maxWrite, took)
		a.mu.Unlock()
	}
}

// setDown changes the first button without a new frame.
func (a *fakeApp) setDown(down bool) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.buttons[0].Down = down
}

// slowestWrite is the longest a push to a subscriber has taken.
func (a *fakeApp) slowestWrite() time.Duration {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.maxWrite
}

// hangUp stops listening and drops every connection, as an app that quit does.
func (a *fakeApp) hangUp() {
	a.listener.Close()
	a.mu.Lock()
	defer a.mu.Unlock()
	for _, c := range a.conns {
		c.Close()
	}
}
