package web

import (
	"bufio"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"
	"net"
	"os"
	"path/filepath"
	"slices"
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
	reqs     []map[string]any
	rotation int
	gate     chan struct{}
	maxWrite time.Duration
	stubborn bool // quit is answered but the app never goes

	audioSubs  []*fakeConn // connections subscribed to audio and still open
	audioAsked int         // how many subscribes have named audio
	codec      *[2]int     // the open codec's rate and channels, nil while it's closed
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
	defer a.dropAudioSub(c)
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
		a.reqs = append(a.reqs, req)
		a.mu.Unlock()
		switch op {
		case "hello":
			c.line(fmt.Sprintf(`{"id":%d,"ok":true,"protocol":1,"project":"demo","board":"B","pid":7}`, id))
		case "subscribe":
			var streams []string
			if list, ok := req["streams"].([]any); ok {
				for _, s := range list {
					name, _ := s.(string)
					streams = append(streams, name)
				}
			}
			audio := slices.Contains(streams, "audio")
			a.mu.Lock()
			if !audio || len(streams) > 1 {
				a.subs = append(a.subs, c)
			}
			// Hold the lock over the reply, so no audio event can come before it.
			if audio {
				a.audioSubs = append(a.audioSubs, c)
				a.audioAsked++
			}
			codec := a.codec
			c.line(fmt.Sprintf(`{"id":%d,"ok":true}`, id))
			// As the app does, a subscriber that arrives while the codec is open is told its format.
			if audio && codec != nil {
				c.line(fmt.Sprintf(`{"event":"audio_format","rate":%d,"channels":%d}`, codec[0], codec[1]))
			}
			a.mu.Unlock()
		case "frame":
			a.mu.Lock()
			number, gate := a.frame, a.gate
			a.mu.Unlock()
			if gate != nil {
				<-gate
			}
			// A picture whose pixels are all one colour is enough to check the size.
			data, _ := json.Marshal(make([]byte, a.width*a.height*2))
			c.line(fmt.Sprintf(`{"id":%d,"ok":true,"width":%d,"height":%d,"format":"rgb565le","data":%s,"frame":%d}`,
				id, a.width, a.height, data, number))
		case "status":
			a.mu.Lock()
			status := client.Status{Project: "demo", Board: "B", Width: a.width, Height: a.height,
				BoardRotation: a.rotation, Frames: a.frame, Power: "on", Buttons: append([]client.ButtonState(nil), a.buttons...)}
			a.mu.Unlock()
			body, _ := json.Marshal(status)
			c.line(fmt.Sprintf(`{"id":%d,"ok":true,%s`, id, body[1:]))
		case "fail":
			c.line(fmt.Sprintf(`{"id":%d,"ok":false,"error":"nope"}`, id))
		case "quit":
			// As an app that quits does: answer, then leave and take the socket with it.
			c.line(fmt.Sprintf(`{"id":%d,"ok":true}`, id))
			a.mu.Lock()
			stubborn := a.stubborn
			a.mu.Unlock()
			if stubborn {
				continue
			}
			a.hangUp()
			return
		case "hang":
			// No reply, as a wedged app gives.
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

// dropConns hangs up on every client but keeps listening, as an app that restarts does.
func (a *fakeApp) dropConns() {
	a.mu.Lock()
	defer a.mu.Unlock()
	for _, c := range a.conns {
		c.Close()
	}
	a.conns = nil
}

// count says how many times op has been asked for.
func (a *fakeApp) count(op string) int {
	a.mu.Lock()
	defer a.mu.Unlock()
	n := 0
	for _, o := range a.ops {
		if o == op {
			n++
		}
	}
	return n
}

// waitSubscribed returns once a client has subscribed, because a frame pushed before that
// reaches nobody.
func waitSubscribed(t *testing.T, a *fakeApp) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		a.mu.Lock()
		subscribed := len(a.subs) > 0
		a.mu.Unlock()
		if subscribed {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	require.FailNow(t, "nobody subscribed")
}

// holdFrames makes frame requests wait, after noting which frame they were asked at, until
// the returned release is called. Events pushed meanwhile queue up behind them.
func (a *fakeApp) holdFrames() (release func()) {
	gate := make(chan struct{})
	a.mu.Lock()
	a.gate = gate
	a.mu.Unlock()
	var once sync.Once
	return func() {
		once.Do(func() {
			a.mu.Lock()
			a.gate = nil
			a.mu.Unlock()
			close(gate)
		})
	}
}

// setRotation changes the board_rotation the status reports.
func (a *fakeApp) setRotation(r int) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.rotation = r
}

// requests returns the fields of every request for op, in order.
func (a *fakeApp) requests(op string) []map[string]any {
	a.mu.Lock()
	defer a.mu.Unlock()
	var out []map[string]any
	for _, r := range a.reqs {
		if r["op"] == op {
			out = append(out, r)
		}
	}
	return out
}

func (a *fakeApp) dropAudioSub(c *fakeConn) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.audioSubs = slices.DeleteFunc(a.audioSubs, func(s *fakeConn) bool { return s == c })
}

// audioListeners says how many open connections are subscribed to audio, and audioSubscribes
// how many subscribes have ever named it.
func (a *fakeApp) audioListeners() int {
	a.mu.Lock()
	defer a.mu.Unlock()
	return len(a.audioSubs)
}

func (a *fakeApp) audioSubscribes() int {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.audioAsked
}

// waitAudioSubscribed returns once a connection is subscribed to audio.
func waitAudioSubscribed(t *testing.T, a *fakeApp) {
	t.Helper()
	require.Eventually(t, func() bool { return a.audioListeners() > 0 }, 3*time.Second, 5*time.Millisecond,
		"nobody subscribed to audio")
}

// pushAudioLine sends line to every audio subscriber. The lock keeps lines in order.
func (a *fakeApp) pushAudioLine(line string) {
	a.mu.Lock()
	defer a.mu.Unlock()
	for _, c := range a.audioSubs {
		c.line(line)
	}
}

// setCodec opens the codec without telling anyone, as an app that opened it before a
// subscriber arrived has.
func (a *fakeApp) setCodec(rate, channels int) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.codec = &[2]int{rate, channels}
}

// pushAudioFormat opens the codec and tells the audio subscribers.
func (a *fakeApp) pushAudioFormat(rate, channels int) {
	a.setCodec(rate, channels)
	a.pushAudioLine(fmt.Sprintf(`{"event":"audio_format","rate":%d,"channels":%d}`, rate, channels))
}

// pushAudio sends one write's samples, interleaved.
func (a *fakeApp) pushAudio(samples []int16) {
	data := make([]byte, 2*len(samples))
	for i, v := range samples {
		binary.LittleEndian.PutUint16(data[2*i:], uint16(v))
	}
	a.pushAudioLine(`{"event":"audio","data":"` + base64.StdEncoding.EncodeToString(data) + `"}`)
}

// pushAudioStop closes the codec and tells the audio subscribers.
func (a *fakeApp) pushAudioStop() {
	a.mu.Lock()
	a.codec = nil
	a.mu.Unlock()
	a.pushAudioLine(`{"event":"audio_stop"}`)
}

// beep is ms of a 440 Hz tone at rate, the same in every channel.
func beep(rate, channels, ms int) []int16 {
	frames := rate * ms / 1000
	out := make([]int16, frames*channels)
	for f := 0; f < frames; f++ {
		v := int16(8000 * math.Sin(2*math.Pi*440*float64(f)/float64(rate)))
		for c := 0; c < channels; c++ {
			out[f*channels+c] = v
		}
	}
	return out
}
