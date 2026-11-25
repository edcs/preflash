package web

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"sync"
	"time"

	"github.com/coder/websocket"
	"github.com/edcs/preflash/internal/client"
)

// The terminal starts at this size. A tab that connects sends its own.
const (
	startCols = 120
	startRows = 40
)

// How long Serve gives the firmware app to answer before it is left alone.
const quitDialPatience = time.Second

// quitPatience is how long Serve waits in all for the firmware app to quit.
var quitPatience = 5 * time.Second

// stderr is where Serve says that the firmware app wouldn't stop.
var stderr io.Writer = os.Stderr

// runs tells every tab when the terminal app starts and stops, so each can say so to its page.
type runs struct {
	term *terminal

	mu      sync.Mutex
	gen     int  // counts starts
	running bool // whether run gen is still going
	changed chan struct{}
	wg      sync.WaitGroup
	stop    chan struct{} // closed by shutdown
}

func newRuns(term *terminal) *runs {
	return &runs{term: term, changed: make(chan struct{}), stop: make(chan struct{})}
}

// shutdown ends the watchers of each run, and waits for them.
func (r *runs) shutdown() {
	close(r.stop)
	r.wg.Wait()
}

// state is the number of the latest run and whether it is going, and a channel that is closed
// when either changes.
func (r *runs) state() (gen int, running bool, changed <-chan struct{}) {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.gen, r.running, r.changed
}

// notify wakes everyone waiting on state. The caller holds mu.
func (r *runs) notify() {
	close(r.changed)
	r.changed = make(chan struct{})
}

// start runs the app unless it already is.
func (r *runs) start() error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.running {
		return nil
	}
	if err := r.term.Start(); err != nil {
		return err
	}
	r.gen++
	r.running = true
	gen, exited := r.gen, r.term.Exited()
	r.notify()
	r.wg.Add(1)
	go func() {
		defer r.wg.Done()
		select {
		case <-exited:
		case <-r.stop:
			return
		}
		r.mu.Lock()
		defer r.mu.Unlock()
		if r.gen == gen {
			r.running = false
			r.notify()
		}
	}()
	return nil
}

// serveTerminal is /terminal: the terminal app's output goes to the page, and its keys come back.
func (s *Server) serveTerminal(w http.ResponseWriter, r *http.Request) {
	// The guard is the one place that checks the Origin, as for /device.
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{InsecureSkipVerify: true})
	if err != nil {
		return
	}
	defer conn.CloseNow()
	if s.term == nil {
		conn.Close(websocket.StatusNormalClosure, "no terminal app is running in this server")
		return
	}
	conn.SetReadLimit(1 << 20)
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()

	c := &terminalConn{runs: s.runs, conn: conn}
	go func() {
		defer cancel()
		c.read(ctx)
	}()
	// A tab that has gone without closing is detached here.
	go keepAlive(ctx, conn, s.pingEvery, s.pingWait, cancel)

	c.write(ctx)
	cancel()
	conn.CloseNow() // lets a blocked read go
	// The reader isn't waited for. It may be stuck typing into an app that isn't reading, and
	// that ends when the terminal closes, so only this tab is held up and not the handler.
}

// terminalConn is one page's connection to the terminal.
type terminalConn struct {
	runs *runs
	conn *websocket.Conn
}

// write sends the screen, the output after it, and a note whenever the app stops or starts,
// until the page goes or ctx ends.
func (c *terminalConn) write(ctx context.Context) {
	term := c.runs.term
	gen, running, changed := c.runs.state()
	snapshot, live, detach := term.Attach()
	defer func() { detach() }()
	if c.sendBinary(ctx, snapshot) != nil {
		return
	}
	if !running && c.sendText(ctx, "exited") != nil {
		return
	}
	for {
		select {
		case <-ctx.Done():
			return
		case chunk, ok := <-live:
			if !ok {
				// Too far behind, or the terminal is closing. The page redraws on reconnect.
				return
			}
			// A new run's output must not go before its started. Its start is counted before
			// any of its output can be seen here, so a chunk that finds a newer run waiting is
			// left out: the screen sent after started has it.
			if g, _, _ := c.runs.state(); g != gen {
				continue
			}
			if c.sendBinary(ctx, chunk) != nil {
				return
			}
		case <-changed:
			newGen, nowRunning, next := c.runs.state()
			changed = next
			switch {
			case newGen != gen:
				// A new run. Attach again so the screen it starts from comes first, ahead of
				// any of its output.
				detach()
				snapshot, live, detach = term.Attach()
				if c.sendText(ctx, "started") != nil || c.sendBinary(ctx, snapshot) != nil {
					return
				}
				if !nowRunning && c.sendText(ctx, "exited") != nil {
					return
				}
			case !nowRunning && running:
				// Let the page have what the app printed last before it hears it has gone.
				if c.drain(ctx, live) != nil || c.sendText(ctx, "exited") != nil {
					return
				}
			}
			gen, running = newGen, nowRunning
		}
	}
}

// drain sends the output that is already waiting.
func (c *terminalConn) drain(ctx context.Context, live <-chan []byte) error {
	for {
		select {
		case chunk, ok := <-live:
			if !ok {
				return context.Canceled
			}
			if err := c.sendBinary(ctx, chunk); err != nil {
				return err
			}
		default:
			return nil
		}
	}
}

func (c *terminalConn) sendText(ctx context.Context, typ string) error {
	return c.sendJSON(ctx, map[string]string{"type": typ})
}

func (c *terminalConn) sendJSON(ctx context.Context, v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(ctx, writeTimeout)
	defer cancel()
	return c.conn.Write(ctx, websocket.MessageText, data)
}

func (c *terminalConn) sendBinary(ctx context.Context, b []byte) error {
	ctx, cancel := context.WithTimeout(ctx, writeTimeout)
	defer cancel()
	return c.conn.Write(ctx, websocket.MessageBinary, b)
}

// read takes the page's keys and messages until it goes. Typing is only ever done from here,
// so an app that has stopped reading holds up this page and no other.
func (c *terminalConn) read(ctx context.Context) {
	for {
		kind, data, err := c.conn.Read(ctx)
		if err != nil {
			return
		}
		if kind == websocket.MessageBinary {
			// The app may have exited, and the page already knows.
			c.runs.term.Input(data)
			continue
		}
		var m struct {
			Type string `json:"type"`
			Cols int    `json:"cols"`
			Rows int    `json:"rows"`
		}
		if err := json.Unmarshal(data, &m); err != nil {
			c.fail(ctx, "not understood")
			continue
		}
		switch m.Type {
		case "resize":
			if err := c.runs.term.Resize(m.Cols, m.Rows); err != nil {
				c.fail(ctx, err.Error())
			}
		case "start":
			if err := c.runs.start(); err != nil {
				c.fail(ctx, err.Error())
			}
		default:
			c.fail(ctx, "unknown message "+m.Type)
		}
	}
}

func (c *terminalConn) fail(ctx context.Context, text string) {
	c.sendJSON(ctx, map[string]string{"type": "error", "text": text})
}

// quitFirmware asks the firmware app on socket to quit, if it still answers, and waits
// quitPatience at most. Closing the terminal app with ^C lets it stop its firmware, but a
// kill leaves the firmware, which is in a group of its own, running.
func quitFirmware(socket string) {
	deadline := time.After(quitPatience)
	dialed := make(chan *client.Client, 1)
	go func() {
		c, err := client.Dial(socket)
		if err != nil {
			c = nil
		}
		dialed <- c
	}()
	var c *client.Client
	select {
	case c = <-dialed:
		if c == nil {
			return
		}
	case <-time.After(quitDialPatience):
		go func() {
			if c := <-dialed; c != nil {
				c.Close()
			}
		}()
		return
	}
	pid := c.Hello.PID
	done := make(chan struct{})
	go func() {
		defer close(done)
		defer c.Close()
		c.Quit()
	}()
	select {
	case <-done:
	case <-deadline:
		// The quit goroutine is left to finish on its own, as the process is about to end.
		fmt.Fprintf(stderr, "preflash: the app didn't stop; it may still be running (pid %d)\n", pid)
	}
}
