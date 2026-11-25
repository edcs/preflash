package web

import (
	"context"
	"errors"
	"reflect"
	"sync"
	"time"

	"github.com/edcs/preflash/internal/client"
)

// How often the state is fetched when no frame has asked for it, which is how soon a button
// pressed on the board shows up.
const statusEvery = 200 * time.Millisecond

// How long to wait before looking for the app again.
const redialEvery = 500 * time.Millisecond

// DeviceState is what a page needs to know about the device apart from its pixels.
type DeviceState struct {
	Connected     bool                 `json:"connected"`
	Project       string               `json:"project"`
	Board         string               `json:"board"`
	Width         int                  `json:"width"`
	Height        int                  `json:"height"`
	Rotation      int                  `json:"rotation"`
	BoardRotation int                  `json:"board_rotation"`
	Backlight     int                  `json:"backlight"`
	FPS           int                  `json:"fps"`
	Power         string               `json:"power"`
	Crashed       bool                 `json:"crashed"`
	Buttons       []client.ButtonState `json:"buttons"`
}

// Picture is one frame as a PNG. App counts the apps the follower has connected to, because
// each one numbers its frames from the start.
type Picture struct {
	App    int
	Number uint64
	PNG    []byte
}

type watcher struct {
	states   chan DeviceState
	pictures chan Picture
}

// device follows the running app and hands its newest state and picture to any number of
// watchers, none of which can hold up the others or the app.
type device struct {
	socket string

	mu         sync.Mutex
	state      DeviceState
	picture    Picture
	hasPicture bool
	app        int
	watchers   map[*watcher]struct{}

	// fetchMu keeps two status fetches from publishing out of order.
	fetchMu sync.Mutex

	inputMu sync.Mutex
	input   *client.Client
}

func newDevice(socket string) *device {
	return &device{socket: socket, watchers: map[*watcher]struct{}{}}
}

// replace puts v in ch, dropping an unread value to make room. Only one goroutine sends at a
// time, so the retry always finds room.
func replace[T any](ch chan T, v T) {
	for {
		select {
		case ch <- v:
			return
		default:
		}
		select {
		case <-ch:
		default:
		}
	}
}

// watch starts a watcher, which gets the current state and newest picture at once.
func (d *device) watch() (states <-chan DeviceState, pictures <-chan Picture, stop func()) {
	w := &watcher{states: make(chan DeviceState, 1), pictures: make(chan Picture, 1)}
	d.mu.Lock()
	replace(w.states, d.state)
	if d.hasPicture {
		replace(w.pictures, d.picture)
	}
	d.watchers[w] = struct{}{}
	d.mu.Unlock()
	return w.states, w.pictures, func() {
		d.mu.Lock()
		delete(d.watchers, w)
		d.mu.Unlock()
	}
}

// publishState tells the watchers, if anything changed.
func (d *device) publishState(s DeviceState) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if reflect.DeepEqual(s, d.state) {
		return
	}
	d.state = s
	for w := range d.watchers {
		replace(w.states, s)
	}
}

func (d *device) publishPicture(p Picture) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.picture, d.hasPicture = p, true
	for w := range d.watchers {
		replace(w.pictures, p)
	}
}

// refreshState fetches the app's status and publishes it.
func (d *device) refreshState(c *client.Client) error {
	d.fetchMu.Lock()
	defer d.fetchMu.Unlock()
	status, err := c.Status()
	if err != nil {
		return err
	}
	d.publishState(DeviceState{
		Connected: true, Project: status.Project, Board: status.Board,
		Width: status.Width, Height: status.Height,
		Rotation: status.Rotation, BoardRotation: status.BoardRotation,
		Backlight: status.Backlight, FPS: status.FPS, Power: status.Power,
		Crashed: status.Crashed, Buttons: status.Buttons,
	})
	return nil
}

// refreshPicture fetches the panel, encodes it once, and returns its number.
func (d *device) refreshPicture(c *client.Client) (uint64, error) {
	frame, err := c.Frame()
	if err != nil {
		return 0, err
	}
	encoded, err := frame.PNG()
	if err != nil {
		return 0, err
	}
	d.mu.Lock()
	app := d.app
	d.mu.Unlock()
	d.publishPicture(Picture{App: app, Number: frame.Number, PNG: encoded})
	return frame.Number, nil
}

// run follows the app until ctx ends, looking for it again whenever it goes.
func (d *device) run(ctx context.Context) {
	defer d.closeInput()
	for ctx.Err() == nil {
		if c, err := client.Dial(d.socket); err == nil {
			d.mu.Lock()
			d.app++
			d.mu.Unlock()
			d.follow(ctx, c)
		}
		d.publishState(DeviceState{})
		select {
		case <-ctx.Done():
		case <-time.After(redialEvery):
		}
	}
}

// follow reads frames on c and keeps the state fresh on a second connection, because a
// connection that waits for events can't also ask for status. It returns on the first error.
func (d *device) follow(ctx context.Context, c *client.Client) {
	ticker, err := client.Dial(d.socket)
	if err != nil {
		c.Close()
		return
	}
	ctx, cancel := context.WithCancel(ctx)
	// Closing the connections is what gets a blocked read to return.
	go func() {
		<-ctx.Done()
		c.Close()
		ticker.Close()
	}()
	done := make(chan struct{}, 2)
	go func() {
		d.tick(ctx, ticker)
		done <- struct{}{}
	}()
	go func() {
		d.frames(c)
		done <- struct{}{}
	}()
	<-done
	cancel()
	<-done
}

func (d *device) tick(ctx context.Context, c *client.Client) {
	t := time.NewTicker(statusEvery)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			if d.refreshState(c) != nil {
				return
			}
		}
	}
}

func (d *device) frames(c *client.Client) {
	if c.Subscribe("frames") != nil {
		return
	}
	// The subscription may not replay, and a page that arrives mid-run wants a picture now.
	last, err := d.refreshPicture(c)
	if err != nil || d.refreshState(c) != nil {
		return
	}
	for {
		event, err := c.Next()
		if err != nil {
			return
		}
		// Events that queued while the last frame was fetched are for frames already shown.
		if event.Kind != "frame" || event.Frame <= last {
			continue
		}
		if last, err = d.refreshPicture(c); err != nil || d.refreshState(c) != nil {
			return
		}
	}
}

// call runs work on a connection kept for input, dialling again once if it went stale.
func (d *device) call(work func(*client.Client) error) error {
	d.inputMu.Lock()
	defer d.inputMu.Unlock()
	for attempt := 0; ; attempt++ {
		if d.input == nil {
			c, err := client.Dial(d.socket)
			if err != nil {
				return err
			}
			d.input = c
		}
		err := work(d.input)
		if err == nil {
			return nil
		}
		// An error reply means nothing ran, but a dead connection says nothing, so only
		// the latter is worth a second try.
		if !errors.Is(err, client.ErrStopped) || attempt > 0 {
			return err
		}
		d.input.Close()
		d.input = nil
	}
}

func (d *device) closeInput() {
	d.inputMu.Lock()
	defer d.inputMu.Unlock()
	if d.input != nil {
		d.input.Close()
		d.input = nil
	}
}
