package web

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"testing"
	"time"

	"github.com/coder/websocket"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// page is a browser's side of /device: it reads in the background so a test can say that
// nothing arrived.
type page struct {
	t     *testing.T
	conn  *websocket.Conn
	texts chan map[string]any
	bins  chan []byte
}

type testSite struct {
	srv  *Server
	addr string
}

func startSite(t *testing.T, socket string) *testSite {
	t.Helper()
	srv := New(Options{Socket: socket})
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		srv.Serve(ctx, ln)
		close(done)
	}()
	t.Cleanup(func() {
		cancel()
		<-done
	})
	return &testSite{srv: srv, addr: ln.Addr().String()}
}

func (s *testSite) open(t *testing.T) *page {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, "ws://"+s.addr+"/device", &websocket.DialOptions{
		HTTPHeader: http.Header{"Origin": {"http://" + s.addr}},
	})
	require.NoError(t, err)
	p := &page{t: t, conn: conn, texts: make(chan map[string]any, 256), bins: make(chan []byte, 256)}
	t.Cleanup(func() { conn.CloseNow() })
	go func() {
		for {
			kind, data, err := conn.Read(context.Background())
			if err != nil {
				return
			}
			if kind == websocket.MessageBinary {
				p.bins <- data
				continue
			}
			var m map[string]any
			if json.Unmarshal(data, &m) == nil {
				p.texts <- m
			}
		}
	}()
	return p
}

func (p *page) send(m map[string]any) {
	p.t.Helper()
	data, err := json.Marshal(m)
	require.NoError(p.t, err)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	require.NoError(p.t, p.conn.Write(ctx, websocket.MessageText, data))
}

// next waits for a text message of the given type.
func (p *page) next(typ string) map[string]any {
	p.t.Helper()
	timeout := time.After(3 * time.Second)
	for {
		select {
		case m := <-p.texts:
			if m["type"] == typ {
				return m
			}
		case <-timeout:
			require.FailNow(p.t, "no message of that type arrived", typ)
		}
	}
}

// frame waits for a frame header and the picture after it.
func (p *page) frame() map[string]any {
	p.t.Helper()
	header := p.next("frame")
	select {
	case png := <-p.bins:
		assert.Equal(p.t, "\x89PNG", string(png[:4]))
	case <-time.After(3 * time.Second):
		require.FailNow(p.t, "no picture followed the header")
	}
	return header
}

// ready waits until the page knows the panel, as a real page does before it sends input.
func (p *page) ready() {
	p.t.Helper()
	for {
		if m := p.next("state"); m["width"] == float64(172) {
			return
		}
	}
}

func TestPointerReachesTheApp(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 172, 640)
	app.setRotation(90)
	p := startSite(t, socket).open(t)
	for {
		if m := p.next("state"); m["board_rotation"] == float64(90) {
			break
		}
	}

	p.send(map[string]any{"type": "pointer", "u": 0, "v": 0, "down": true})
	p.send(map[string]any{"type": "pointer", "u": 0, "v": 0, "down": false})

	require.Eventually(t, func() bool { return len(app.requests("pointer")) == 2 }, 2*time.Second, 5*time.Millisecond)
	got := app.requests("pointer")
	assert.Equal(t, []any{float64(0), float64(639), true}, []any{got[0]["x"], got[0]["y"], got[0]["down"]})
	assert.Equal(t, []any{float64(0), float64(639), false}, []any{got[1]["x"], got[1]["y"], got[1]["down"]})
}

func TestPointerBeforeTheStateIsDropped(t *testing.T) {
	// No app is listening, so the page never learns a panel size.
	p := startSite(t, shortPath(t)).open(t)
	p.send(map[string]any{"type": "pointer", "u": 0.5, "v": 0.5, "down": true})
	assert.NotEmpty(t, p.next("error")["text"])
}

func TestButtonPressAndRelease(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 172, 640)
	p := startSite(t, socket).open(t)
	p.ready()

	p.send(map[string]any{"type": "button", "name": "BOOT", "down": true})
	p.send(map[string]any{"type": "button", "name": "BOOT", "down": false})

	require.Eventually(t, func() bool { return len(app.requests("button")) == 2 }, 2*time.Second, 5*time.Millisecond)
	got := app.requests("button")
	assert.Equal(t, []any{"BOOT", "press"}, []any{got[0]["name"], got[0]["action"]})
	assert.Equal(t, []any{"BOOT", "release"}, []any{got[1]["name"], got[1]["action"]})
}

func TestOneFrameInFlight(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 172, 640)
	site := startSite(t, socket)
	p := site.open(t)
	p.frame() // the picture that is there on arrival
	waitSubscribed(t, app)

	_, pictures, stop := site.srv.dev.watch()
	defer stop()
	for n := uint64(1); n <= 20; n++ {
		app.push(n)
	}
	pictureWhere(t, pictures, 3*time.Second, func(pic Picture) bool { return pic.Number == 20 })

	select {
	case m := <-p.texts:
		assert.NotEqual(t, "frame", m["type"], "a frame arrived before the ack")
	case <-time.After(300 * time.Millisecond):
	}

	p.send(map[string]any{"type": "ack"})
	assert.Equal(t, float64(20), p.frame()["number"])
}

func TestClosingReleasesWhatThePageHeld(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 172, 640)
	p := startSite(t, socket).open(t)
	p.ready()

	p.send(map[string]any{"type": "pointer", "u": 0.5, "v": 0.5, "down": true})
	p.send(map[string]any{"type": "button", "name": "BOOT", "down": true})
	p.conn.Close(websocket.StatusNormalClosure, "")

	require.Eventually(t, func() bool {
		pointers, buttons := app.requests("pointer"), app.requests("button")
		return len(pointers) == 2 && pointers[1]["down"] == false &&
			len(buttons) == 2 && buttons[1]["action"] == "release" && buttons[1]["name"] == "BOOT"
	}, time.Second, 5*time.Millisecond)
}

// quickPings has the server check its pages every few milliseconds until the test ends. It
// goes before the site starts, which takes the timings when it is made.
func quickPings(t *testing.T) {
	t.Helper()
	every, wait := pingInterval, pingTimeout
	pingInterval, pingTimeout = 20*time.Millisecond, 100*time.Millisecond
	t.Cleanup(func() { pingInterval, pingTimeout = every, wait })
}

// dialSilent connects to path and never reads, so it answers no ping, as a page on an iPad
// that slept behind an SSH forward doesn't.
func dialSilent(t *testing.T, addr, path string) *websocket.Conn {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, "ws://"+addr+path, &websocket.DialOptions{
		HTTPHeader: http.Header{"Origin": {"http://" + addr}},
	})
	require.NoError(t, err)
	t.Cleanup(func() { conn.CloseNow() })
	return conn
}

func TestAPageThatStopsAnsweringIsReleased(t *testing.T) {
	quickPings(t)
	socket := shortPath(t)
	app := startFakeApp(t, socket, 172, 640)
	site := startSite(t, socket)
	conn := dialSilent(t, site.addr, "/device")

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	require.NoError(t, conn.Write(ctx, websocket.MessageText, []byte(`{"type":"button","name":"BOOT","down":true}`)))

	require.Eventually(t, func() bool {
		buttons := app.requests("button")
		return len(buttons) == 2 && buttons[1]["action"] == "release" && buttons[1]["name"] == "BOOT"
	}, 2*time.Second, 5*time.Millisecond, "BOOT was never let go")
}

func TestAPageHearsAPing(t *testing.T) {
	quickPings(t)
	p := startSite(t, shortPath(t)).open(t)
	p.next("ping")
}

func TestACrossSiteUpgradeIsRefused(t *testing.T) {
	site := startSite(t, shortPath(t))
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_, resp, err := websocket.Dial(ctx, "ws://"+site.addr+"/device", &websocket.DialOptions{
		HTTPHeader: http.Header{"Origin": {"https://evil.example"}},
	})
	require.Error(t, err)
	require.NotNil(t, resp)
	assert.Equal(t, http.StatusForbidden, resp.StatusCode)
}
