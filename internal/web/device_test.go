package web

import (
	"bytes"
	"context"
	"image/png"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func startDevice(t *testing.T, socket string) *device {
	t.Helper()
	d := newDevice(socket)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		d.run(ctx)
		close(done)
	}()
	t.Cleanup(func() {
		cancel()
		<-done
	})
	return d
}

// stateWhere waits for a state that suits.
func stateWhere(t *testing.T, states <-chan DeviceState, within time.Duration, ok func(DeviceState) bool) DeviceState {
	t.Helper()
	timeout := time.After(within)
	for {
		select {
		case s := <-states:
			if ok(s) {
				return s
			}
		case <-timeout:
			require.FailNow(t, "no such state arrived")
		}
	}
}

func pictureWhere(t *testing.T, pictures <-chan Picture, within time.Duration, ok func(Picture) bool) Picture {
	t.Helper()
	timeout := time.After(within)
	for {
		select {
		case p := <-pictures:
			if ok(p) {
				return p
			}
		case <-timeout:
			require.FailNow(t, "no such picture arrived")
		}
	}
}

func TestFramesArriveNewestFirst(t *testing.T) {
	path := shortPath(t)
	app := startFakeApp(t, path, 8, 6)
	d := startDevice(t, path)
	_, pictures, stop := d.watch()
	defer stop()

	go func() {
		for n := uint64(1); n <= 50; n++ {
			app.push(n)
		}
	}()

	var last uint64
	seen := false
	for last < 50 {
		p := pictureWhere(t, pictures, 2*time.Second, func(Picture) bool { return true })
		if seen {
			assert.Greater(t, p.Number, last, "numbers only go up")
		}
		seen, last = true, p.Number
		img, err := png.Decode(bytes.NewReader(p.PNG))
		require.NoError(t, err)
		assert.Equal(t, 8, img.Bounds().Dx())
		assert.Equal(t, 6, img.Bounds().Dy())
		time.Sleep(5 * time.Millisecond) // a slow reader
	}
}

func TestAStalledWatcherHoldsUpNoOne(t *testing.T) {
	path := shortPath(t)
	app := startFakeApp(t, path, 8, 6)
	d := startDevice(t, path)
	_, _, stopStalled := d.watch()
	defer stopStalled()
	_, pictures, stop := d.watch()
	defer stop()

	for n := uint64(1); n <= 50; n++ {
		app.push(n)
	}
	pictureWhere(t, pictures, time.Second, func(p Picture) bool { return p.Number == 50 })
	assert.Less(t, app.slowestWrite(), 100*time.Millisecond)
}

func TestNoAppMeansNotConnected(t *testing.T) {
	path := shortPath(t)
	d := startDevice(t, path)
	states, _, stop := d.watch()
	defer stop()

	first := <-states
	assert.False(t, first.Connected)

	startFakeApp(t, path, 8, 6)
	s := stateWhere(t, states, 2*time.Second, func(s DeviceState) bool { return s.Connected })
	assert.Equal(t, "demo", s.Project)
	assert.Equal(t, 8, s.Width)
}

func TestANewAppStartsTheFramesAgain(t *testing.T) {
	path := shortPath(t)
	old := startFakeApp(t, path, 8, 6)
	d := startDevice(t, path)
	states, pictures, stop := d.watch()
	defer stop()

	old.push(40)
	first := pictureWhere(t, pictures, 2*time.Second, func(p Picture) bool { return p.Number == 40 })
	old.hangUp()
	stateWhere(t, states, 2*time.Second, func(s DeviceState) bool { return !s.Connected })

	next := startFakeApp(t, path, 172, 640)
	go func() {
		// Frame 1 only reaches a subscriber, so wait for the follower to be back.
		for i := 0; i < 100; i++ {
			next.mu.Lock()
			subscribed := len(next.subs) > 0
			next.mu.Unlock()
			if subscribed {
				next.push(1)
				return
			}
			time.Sleep(20 * time.Millisecond)
		}
	}()
	p := pictureWhere(t, pictures, 3*time.Second, func(p Picture) bool { return p.App > first.App && p.Number == 1 })
	assert.Equal(t, first.App+1, p.App)
	stateWhere(t, states, 2*time.Second, func(s DeviceState) bool {
		return s.Connected && s.Width == 172 && s.Height == 640
	})
}

func TestStateFollowsButtonsWithoutFrames(t *testing.T) {
	path := shortPath(t)
	app := startFakeApp(t, path, 8, 6)
	d := startDevice(t, path)
	states, _, stop := d.watch()
	defer stop()
	stateWhere(t, states, 2*time.Second, func(s DeviceState) bool { return s.Connected })

	app.setDown(true)
	stateWhere(t, states, 500*time.Millisecond, func(s DeviceState) bool {
		return len(s.Buttons) > 0 && s.Buttons[0].Down
	})
}
