// Package client talks to a running app over its control socket.
package client

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net"
	"os"
	"syscall"
	"time"
)

// Protocol is the version of the socket's wire format this client speaks.
const Protocol = 1

// A stale socket and an app part way through a restart look the same from here, so
// a dial gives a refused socket this long to come back.
const refusedGrace = 500 * time.Millisecond

// A restart takes as long as the firmware's process does to start, with no build.
const restartGrace = 15 * time.Second

// A boot replaces a stale socket by removing it and binding anew, so the file can be
// missing for a moment on the way to coming back.
const replaceGrace = 100 * time.Millisecond

// Quit lets queued console lines finish, each with a second to show progress, so a long
// queue can take a while.
const quitGrace = 30 * time.Second

const helloGrace = 2 * time.Second

const dialPause = 20 * time.Millisecond

// How many times Quit asks again.
const quitTries = 2

var (
	// ErrNotRunning means no app answers on the socket.
	ErrNotRunning = errors.New("no app is running")
	// ErrStopped means the app went away while talking to it.
	ErrStopped = errors.New("the app stopped")
)

// Client is one connection to an app. It isn't safe for concurrent use.
type Client struct {
	conn    net.Conn
	path    string
	reader  *bufio.Reader
	lastID  int
	pending []Event
	Hello   Hello
}

// Hello is what an app says about itself when a client connects.
type Hello struct {
	Protocol int    `json:"protocol"`
	Project  string `json:"project"`
	Board    string `json:"board"`
	PID      int    `json:"pid"`
}

// Status is a running app's state.
type Status struct {
	Project   string `json:"project"`
	Board     string `json:"board"`
	Width     int    `json:"width"`
	Height    int    `json:"height"`
	PID       int    `json:"pid"`
	UptimeMs  int64  `json:"uptime_ms"`
	Backlight int    `json:"backlight"`
	Frames    uint64 `json:"frames"`
	FPS       int    `json:"fps"`
	WiFi      string `json:"wifi"`
}

// Event is a message from the app that answers no request.
type Event struct {
	Kind  string // "serial", "dropped" or "restarting"
	Data  []byte // what came out of the serial stream
	Bytes int    // how much a dropped event lost
}

// wire is every field a reply or an event can carry that this file reads itself.
type wire struct {
	ID    *int   `json:"id"`
	OK    bool   `json:"ok"`
	Error string `json:"error"`
	Event string `json:"event"`
	Data  []byte `json:"data"`
	Bytes int    `json:"bytes"`
}

// Dial connects to the app at path. The error is ErrNotRunning if none is there.
func Dial(path string) (*Client, error) {
	return dialWithin(path, refusedGrace, 0, 0)
}

// dialWithin keeps trying for as long as patience lasts, or missingPatience once the
// socket's file has gone, which usually means the app quit. It stops early if pid, the
// process expected to come back, dies meanwhile.
func dialWithin(path string, patience, missingPatience time.Duration, pid int) (*Client, error) {
	start := time.Now()
	for {
		c, err := dialOnce(path)
		if err == nil || !errors.Is(err, ErrNotRunning) {
			return c, err
		}
		waited := time.Since(start)
		if waited >= patience || !alive(pid) || (errors.Is(err, fs.ErrNotExist) && waited >= missingPatience) {
			return nil, ErrNotRunning
		}
		time.Sleep(dialPause)
	}
}

// alive says whether the process exists. A restart keeps its pid, so this tells one from a
// crash that left the socket's file behind. An app that gave no pid counts as alive.
func alive(pid int) bool {
	return pid <= 0 || !errors.Is(syscall.Kill(pid, 0), syscall.ESRCH)
}

func dialOnce(path string) (*Client, error) {
	conn, err := net.Dial("unix", path)
	if err != nil {
		if errors.Is(err, fs.ErrNotExist) || errors.Is(err, syscall.ECONNREFUSED) {
			return nil, fmt.Errorf("%w: %w", ErrNotRunning, err)
		}
		return nil, err
	}
	c := &Client{conn: conn, reader: bufio.NewReader(conn), path: path}
	// A listener that's stopped still accepts, so hello can't wait for good.
	if err := conn.SetDeadline(time.Now().Add(helloGrace)); err != nil {
		conn.Close()
		return nil, err
	}
	if err := c.Call("hello", nil, &c.Hello); err != nil {
		conn.Close()
		if errors.Is(err, ErrStopped) {
			return nil, fmt.Errorf("%w: %w", ErrNotRunning, err)
		}
		return nil, err
	}
	if err := conn.SetDeadline(time.Time{}); err != nil {
		conn.Close()
		return nil, err
	}
	if c.Hello.Protocol != Protocol {
		conn.Close()
		return nil, fmt.Errorf("the app speaks protocol %d and this preflash speaks %d: rebuild one of them", c.Hello.Protocol, Protocol)
	}
	return c, nil
}

// Close ends the connection, leaving the app running.
func (c *Client) Close() error { return c.conn.Close() }

// Call sends one request and waits for its reply, which it decodes into reply if that
// isn't nil. Events that arrive first wait for Next.
func (c *Client) Call(op string, fields map[string]any, reply any) error {
	c.lastID++
	request := map[string]any{"id": c.lastID, "op": op}
	for name, value := range fields {
		request[name] = value
	}
	line, err := json.Marshal(request)
	if err != nil {
		return err
	}
	if _, err := c.conn.Write(append(line, '\n')); err != nil {
		return fmt.Errorf("%w: %w", ErrStopped, err)
	}
	for {
		raw, message, err := c.readLine()
		if err != nil {
			return err
		}
		if message.ID == nil {
			if message.Event != "" {
				c.pending = append(c.pending, message.event())
				continue
			}
			// The app couldn't tie its complaint to a request.
			return replyError(message)
		}
		if *message.ID != c.lastID {
			continue
		}
		if !message.OK {
			return replyError(message)
		}
		if reply == nil {
			return nil
		}
		return json.Unmarshal(raw, reply)
	}
}

func replyError(message wire) error {
	if message.Error == "" {
		return errors.New("the app sent a line this preflash doesn't understand")
	}
	return errors.New(message.Error)
}

func (w wire) event() Event { return Event{Kind: w.Event, Data: w.Data, Bytes: w.Bytes} }

// Next waits for the next event. The error wraps ErrStopped once the app has gone.
func (c *Client) Next() (Event, error) {
	if len(c.pending) > 0 {
		next := c.pending[0]
		c.pending = c.pending[1:]
		return next, nil
	}
	for {
		_, message, err := c.readLine()
		if err != nil {
			return Event{}, err
		}
		if message.Event != "" {
			return message.event(), nil
		}
	}
}

func (c *Client) readLine() ([]byte, wire, error) {
	raw, err := c.reader.ReadBytes('\n')
	if err != nil {
		return nil, wire{}, fmt.Errorf("%w: %w", ErrStopped, err)
	}
	var message wire
	if err := json.Unmarshal(raw, &message); err != nil {
		return nil, wire{}, fmt.Errorf("a bad line from the app: %w", err)
	}
	return raw, message, nil
}

// Status asks for the app's state.
func (c *Client) Status() (Status, error) {
	var status Status
	err := c.Call("status", nil, &status)
	return status, err
}

// Send adds one line to the app's console queue. It doesn't wait for the line to run.
func (c *Client) Send(line string) error {
	return c.Call("send", map[string]any{"line": line}, nil)
}

// Subscribe starts the named streams, each replaying what it has kept first.
func (c *Client) Subscribe(streams ...string) error {
	return c.Call("subscribe", map[string]any{"streams": streams}, nil)
}

// Frame fetches what the panel shows.
func (c *Client) Frame() (Frame, error) {
	var frame Frame
	err := c.Call("frame", nil, &frame)
	return frame, err
}

// Touch presses the panel for ms and returns after the release.
func (c *Client) Touch(x, y, ms int) error {
	return c.Call("touch", map[string]any{"x": x, "y": y, "ms": ms}, nil)
}

// TouchDefault presses the panel for the app's default time.
func (c *Client) TouchDefault(x, y int) error {
	return c.Call("touch", map[string]any{"x": x, "y": y}, nil)
}

// Quit stops the app and returns once it has gone. A quit that lands just as a restart
// execs is lost, with or without its reply, so a hang-up isn't proof: the new boot gets as
// long as a restart can take to answer, and is asked again. Quitting removes the socket,
// and a dead process can't come back, so neither of those waits.
func (c *Client) Quit() error {
	for try := 0; ; try++ {
		err := c.Call("quit", nil, nil)
		if err != nil && !errors.Is(err, ErrStopped) {
			return err
		}
		if err == nil {
			if err := c.WaitForExit(quitGrace); err != nil {
				return err
			}
		}
		next, err := dialWithin(c.path, restartGrace, replaceGrace, c.Hello.PID)
		if errors.Is(err, ErrNotRunning) {
			return nil
		}
		if err != nil {
			return err
		}
		if try == quitTries {
			next.Close()
			return errors.New("the app is still running")
		}
		c.conn.Close()
		*c = *next
	}
}

// WaitForExit returns when the app hangs up, or with an error if it hasn't in time.
func (c *Client) WaitForExit(patience time.Duration) error {
	if err := c.conn.SetReadDeadline(time.Now().Add(patience)); err != nil {
		return err
	}
	_, err := io.Copy(io.Discard, c.reader)
	var timeout net.Error
	if errors.As(err, &timeout) && timeout.Timeout() {
		return errors.New("the app is still running")
	}
	return nil
}

// Follow writes the serial stream to out, starting with what the app has kept, and keeps
// on through restarts. It returns nil when the app stops, and warn hears about any output
// lost to a reader too slow to keep up.
func Follow(path string, out io.Writer, warn func(string)) error {
	c, err := Dial(path)
	if err != nil {
		return err
	}
	for {
		restarting, err := followOne(c, out, warn)
		pid := c.Hello.PID
		c.Close()
		switch {
		case err != nil:
			return err
		case restarting:
			if c, err = dialWithin(path, restartGrace, restartGrace, pid); errors.Is(err, ErrNotRunning) {
				// A quit can overtake the restart, and it removes the socket. A crash doesn't.
				if _, missing := os.Stat(path); errors.Is(missing, fs.ErrNotExist) {
					return nil
				}
				return ErrStopped
			}
			if err != nil {
				return err
			}
		default:
			// A writer stuck on this reader can lose the restarting event at the exec, so
			// a hang-up from a live process may be a restart. A quit removes the socket.
			if c, err = dialWithin(path, restartGrace, replaceGrace, pid); errors.Is(err, ErrNotRunning) {
				return nil
			}
			if err != nil {
				return err
			}
		}
	}
}

// followOne covers one boot, saying whether it ended in a restart. The event comes ahead
// of any serial still queued, so this reads on to the hang-up.
func followOne(c *Client, out io.Writer, warn func(string)) (bool, error) {
	// An app that stops here can still have said it's restarting, so the loop looks.
	if err := c.Subscribe("serial"); err != nil && !errors.Is(err, ErrStopped) {
		return false, err
	}
	restarting := false
	for {
		event, err := c.Next()
		if errors.Is(err, ErrStopped) {
			return restarting, nil
		}
		if err != nil {
			return false, err
		}
		switch event.Kind {
		case "serial":
			if _, err := out.Write(event.Data); err != nil {
				return false, err
			}
		case "dropped":
			warn(fmt.Sprintf("%d bytes of serial were dropped because this reader fell behind", event.Bytes))
		case "restarting":
			restarting = true
		}
	}
}
