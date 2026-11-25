package web

import (
	"context"
	"encoding/json"
	"net/http"
	"sync"
	"time"

	"github.com/coder/websocket"
	"github.com/edcs/preflash/internal/client"
)

// How long a write to a page may take before the page counts as gone.
const writeTimeout = 10 * time.Second

// What a page may send. Type says which of the others count.
type pageMessage struct {
	Type string  `json:"type"`
	U    float64 `json:"u"`
	V    float64 `json:"v"`
	Name string  `json:"name"`
	Down bool    `json:"down"`
}

// serveDevice is /device: the device's state and pictures go to the page, and its touches and
// button presses come back.
func (s *Server) serveDevice(w http.ResponseWriter, r *http.Request) {
	// The guard has already checked the Origin against the Host and --allow-host names, and is
	// the one place that decides, so the library's own check, which knows only the Host, must
	// not refuse a host the guard let through.
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{InsecureSkipVerify: true})
	if err != nil {
		return
	}
	defer conn.CloseNow()
	conn.SetReadLimit(4096)
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()

	c := &deviceConn{dev: s.dev, conn: conn}
	acks := make(chan struct{}, 1)
	readerDone := make(chan struct{})
	go func() {
		defer close(readerDone)
		defer cancel()
		c.read(ctx, acks)
	}()
	// A page that has gone without closing, as one behind an SSH forward does, is let go
	// of here, and so is what it held.
	go keepAlive(ctx, conn, s.pingEvery, s.pingWait, cancel)

	c.write(ctx, acks)
	cancel()
	conn.CloseNow() // lets a blocked read go
	<-readerDone
	c.releaseAll()
}

// deviceConn is one page's connection and the input it holds down.
type deviceConn struct {
	dev  *device
	conn *websocket.Conn

	mu    sync.Mutex
	state DeviceState // the newest the page has been sent

	// Only the reading goroutine touches these.
	pointerDown bool
	pointerX    int
	pointerY    int
	held        map[string]bool
}

// write sends states and frames until the page goes or ctx ends.
func (c *deviceConn) write(ctx context.Context, acks <-chan struct{}) {
	states, pictures, stop := c.dev.watch()
	defer stop()
	inFlight := false
	for {
		// While a frame is unacknowledged, the newest waits in the channel.
		var next <-chan Picture
		if !inFlight {
			next = pictures
		}
		select {
		case <-ctx.Done():
			return
		case <-acks:
			inFlight = false
		case s := <-states:
			c.mu.Lock()
			c.state = s
			c.mu.Unlock()
			if c.send(ctx, struct {
				Type string `json:"type"`
				DeviceState
			}{"state", s}) != nil {
				return
			}
		case p := <-next:
			header := map[string]any{"type": "frame", "app": p.App, "number": p.Number}
			if c.send(ctx, header) != nil || c.sendPNG(ctx, p.PNG) != nil {
				return
			}
			inFlight = true
		}
	}
}

func (c *deviceConn) send(ctx context.Context, v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(ctx, writeTimeout)
	defer cancel()
	return c.conn.Write(ctx, websocket.MessageText, data)
}

func (c *deviceConn) sendPNG(ctx context.Context, png []byte) error {
	ctx, cancel := context.WithTimeout(ctx, writeTimeout)
	defer cancel()
	return c.conn.Write(ctx, websocket.MessageBinary, png)
}

// read takes the page's messages until it goes. Bad input is answered with an error and the
// connection stays open.
func (c *deviceConn) read(ctx context.Context, acks chan<- struct{}) {
	for {
		_, data, err := c.conn.Read(ctx)
		if err != nil {
			return
		}
		var m pageMessage
		if err := json.Unmarshal(data, &m); err != nil {
			c.fail(ctx, "not understood")
			continue
		}
		switch m.Type {
		case "ack":
			select {
			case acks <- struct{}{}:
			default:
			}
		case "pointer":
			c.pointer(ctx, m)
		case "button":
			c.button(ctx, m)
		default:
			c.fail(ctx, "unknown message "+m.Type)
		}
	}
}

func (c *deviceConn) fail(ctx context.Context, text string) {
	c.send(ctx, map[string]string{"type": "error", "text": text})
}

func (c *deviceConn) pointer(ctx context.Context, m pageMessage) {
	c.mu.Lock()
	s := c.state
	c.mu.Unlock()
	if s.Width == 0 || s.Height == 0 {
		c.fail(ctx, "the device has no panel yet")
		return
	}
	x, y := toPanel(m.U, m.V, s.BoardRotation, s.Width, s.Height)
	err := c.dev.call(func(cl *client.Client) error { return cl.Pointer(x, y, m.Down) })
	if err != nil {
		c.fail(ctx, err.Error())
		return
	}
	c.pointerDown, c.pointerX, c.pointerY = m.Down, x, y
}

func (c *deviceConn) button(ctx context.Context, m pageMessage) {
	action := "release"
	if m.Down {
		action = "press"
	}
	err := c.dev.call(func(cl *client.Client) error { return cl.Button(m.Name, action, 0) })
	if err != nil {
		c.fail(ctx, err.Error())
		return
	}
	if c.held == nil {
		c.held = map[string]bool{}
	}
	if m.Down {
		c.held[m.Name] = true
	} else {
		delete(c.held, m.Name)
	}
}

// releaseAll lets go of whatever this page left held, as a page that slept mid-touch would.
// The request that ended the connection is over, so call brings its own deadline.
func (c *deviceConn) releaseAll() {
	if c.pointerDown {
		c.dev.call(func(cl *client.Client) error { return cl.Pointer(c.pointerX, c.pointerY, false) })
	}
	for name := range c.held {
		c.dev.call(func(cl *client.Client) error { return cl.Button(name, "release", 0) })
	}
}
