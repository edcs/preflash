package client

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"image/png"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"sync"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// A socket path short enough for macOS, which t.TempDir's isn't.
func shortPath(t *testing.T) string {
	t.Helper()
	dir, err := os.MkdirTemp("/tmp", "pf")
	require.NoError(t, err)
	t.Cleanup(func() { os.RemoveAll(dir) })
	return filepath.Join(dir, "s")
}

// fakeApp speaks the protocol with canned answers. Each connection gets hello for free;
// other ops go to the handler, which writes whatever lines it likes.
type fakeApp struct {
	t        *testing.T
	listener net.Listener
	mu       sync.Mutex
	requests []map[string]any
	handle   func(req map[string]any, reply func(string))
}

func startFake(t *testing.T, path string, handle func(map[string]any, func(string))) *fakeApp {
	t.Helper()
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	app := &fakeApp{t: t, listener: listener, handle: handle}
	t.Cleanup(func() { listener.Close() })
	go app.accept()
	return app
}

func (a *fakeApp) accept() {
	for {
		conn, err := a.listener.Accept()
		if err != nil {
			return
		}
		go a.serve(conn)
	}
}

func (a *fakeApp) serve(conn net.Conn) {
	defer conn.Close()
	scanner := bufio.NewScanner(conn)
	for scanner.Scan() {
		var req map[string]any
		if json.Unmarshal(scanner.Bytes(), &req) != nil {
			return
		}
		reply := func(line string) { fmt.Fprintln(conn, line) }
		id := int(req["id"].(float64))
		if req["op"] == "hello" {
			reply(fmt.Sprintf(`{"id":%d,"ok":true,"protocol":1,"project":"demo","board":"B","pid":7}`, id))
			continue
		}
		a.mu.Lock()
		a.requests = append(a.requests, req)
		a.mu.Unlock()
		a.handle(req, reply)
		if req["op"] == "hangup" {
			return
		}
	}
}

func (a *fakeApp) seen() []map[string]any {
	a.mu.Lock()
	defer a.mu.Unlock()
	return append([]map[string]any(nil), a.requests...)
}

func ok(req map[string]any) string {
	return fmt.Sprintf(`{"id":%d,"ok":true}`, int(req["id"].(float64)))
}

func TestDialSaysWhenNothingIsThere(t *testing.T) {
	_, err := Dial(shortPath(t))
	assert.ErrorIs(t, err, ErrNotRunning)
}

func TestDialTreatsAStaleSocketAsNotRunning(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	listener.(*net.UnixListener).SetUnlinkOnClose(false)
	listener.Close()
	require.FileExists(t, path)

	_, err = Dial(path)
	assert.ErrorIs(t, err, ErrNotRunning)
}

func TestDialRefusesAnotherProtocol(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	defer listener.Close()
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		bufio.NewReader(conn).ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":99}`)
	}()

	_, err = Dial(path)
	require.Error(t, err)
	assert.NotErrorIs(t, err, ErrNotRunning)
	assert.Contains(t, err.Error(), "99")
}

func TestDialGivesUpOnAListenerThatNeverAnswers(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	defer listener.Close()

	_, err = Dial(path)
	assert.ErrorIs(t, err, ErrNotRunning)
}

func TestStatusReadsEveryField(t *testing.T) {
	path := shortPath(t)
	startFake(t, path, func(req map[string]any, reply func(string)) {
		reply(fmt.Sprintf(`{"id":%d,"ok":true,"project":"demo","board":"B","width":480,"height":320,"pid":7,"uptime_ms":1500,"backlight":40,"frames":12,"fps":3,"wifi":"connected"}`, int(req["id"].(float64))))
	})
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()

	got, err := c.Status()
	require.NoError(t, err)
	assert.Equal(t, Status{Project: "demo", Board: "B", Width: 480, Height: 320, PID: 7, UptimeMs: 1500, Backlight: 40, Frames: 12, FPS: 3, WiFi: "connected"}, got)
}

func TestAnErrorReplyBecomesAnError(t *testing.T) {
	path := shortPath(t)
	startFake(t, path, func(req map[string]any, reply func(string)) {
		reply(fmt.Sprintf(`{"id":%d,"ok":false,"error":"nope, %s"}`, int(req["id"].(float64)), req["op"]))
	})
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()

	assert.EqualError(t, c.Send("x"), "nope, send")
}

func TestRequestsCarryTheirFields(t *testing.T) {
	path := shortPath(t)
	app := startFake(t, path, func(req map[string]any, reply func(string)) { reply(ok(req)) })
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()

	require.NoError(t, c.Send("ping"))
	require.NoError(t, c.Touch(1, 2, 30))
	require.NoError(t, c.TouchDefault(4, 5))
	seen := app.seen()
	require.Len(t, seen, 3)
	assert.Equal(t, "ping", seen[0]["line"])
	assert.EqualValues(t, 1, seen[1]["x"])
	assert.EqualValues(t, 2, seen[1]["y"])
	assert.EqualValues(t, 30, seen[1]["ms"])
	assert.NotContains(t, seen[2], "ms")
}

func TestEventsArrivingBeforeAReplyAreKept(t *testing.T) {
	path := shortPath(t)
	startFake(t, path, func(req map[string]any, reply func(string)) {
		reply(`{"event":"serial","data":"` + base64.StdEncoding.EncodeToString([]byte("early")) + `"}`)
		reply(`{"event":"dropped","bytes":12}`)
		reply(ok(req))
		reply(`{"event":"restarting"}`)
	})
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()

	require.NoError(t, c.Subscribe("serial"))
	first, err := c.Next()
	require.NoError(t, err)
	assert.Equal(t, Event{Kind: "serial", Data: []byte("early")}, first)
	second, err := c.Next()
	require.NoError(t, err)
	assert.Equal(t, Event{Kind: "dropped", Bytes: 12}, second)
	third, err := c.Next()
	require.NoError(t, err)
	assert.Equal(t, "restarting", third.Kind)
}

func TestALongLineComesThroughWhole(t *testing.T) {
	path := shortPath(t)
	big := bytes.Repeat([]byte{0xab}, 4<<20)
	startFake(t, path, func(req map[string]any, reply func(string)) {
		reply(fmt.Sprintf(`{"id":%d,"ok":true,"width":1,"height":1,"format":"rgb565le","data":"%s"}`,
			int(req["id"].(float64)), base64.StdEncoding.EncodeToString(big)))
	})
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()

	got, err := c.Frame()
	require.NoError(t, err)
	assert.Equal(t, big, got.Data)
}

func TestLosingTheAppMidCallSaysSo(t *testing.T) {
	path := shortPath(t)
	startFake(t, path, func(map[string]any, func(string)) {})
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()

	err = c.Call("hangup", nil, nil)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrStopped)
}

func TestWaitForExitReturnsAtHangUp(t *testing.T) {
	path := shortPath(t)
	startFake(t, path, func(req map[string]any, reply func(string)) { reply(ok(req)) })
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()

	require.NoError(t, c.Call("hangup", nil, nil))
	assert.NoError(t, c.WaitForExit(2*time.Second))
}

func TestFollowReconnectsAcrossARestart(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	go func() {
		conn, err := listener.Accept()
		require.NoError(t, err)
		reader := bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		fmt.Fprintln(conn, `{"event":"serial","data":"`+base64.StdEncoding.EncodeToString([]byte("first boot\n"))+`"}`)
		fmt.Fprintln(conn, `{"event":"restarting"}`)
		conn.Close()
		listener.Close()

		// The gap a restart leaves, with nothing listening.
		time.Sleep(100 * time.Millisecond)
		next, err := net.Listen("unix", path)
		require.NoError(t, err)
		conn, err = next.Accept()
		require.NoError(t, err)
		reader = bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		fmt.Fprintln(conn, `{"event":"serial","data":"`+base64.StdEncoding.EncodeToString([]byte("second boot\n"))+`"}`)
		conn.Close()
		next.Close()
	}()

	var out syncBuffer
	require.NoError(t, Follow(path, &out, func(string) {}))
	assert.Equal(t, "first boot\nsecond boot\n", out.String())
}

// A reader too far behind can lose the restarting event with the rest of its queue, so
// a hang-up on its own gets a short try at reconnecting.
func TestFollowComesBackFromAHangUpWithNoWarning(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	listener.(*net.UnixListener).SetUnlinkOnClose(false)
	go func() {
		conn, err := listener.Accept()
		require.NoError(t, err)
		reader := bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		fmt.Fprintln(conn, `{"event":"serial","data":"`+base64.StdEncoding.EncodeToString([]byte("first boot\n"))+`"}`)
		conn.Close()
		listener.Close()

		// The old socket file refuses until the new boot replaces it.
		time.Sleep(100 * time.Millisecond)
		os.Remove(path)
		next, err := net.Listen("unix", path)
		require.NoError(t, err)
		conn, err = next.Accept()
		require.NoError(t, err)
		reader = bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		fmt.Fprintln(conn, `{"event":"serial","data":"`+base64.StdEncoding.EncodeToString([]byte("second boot\n"))+`"}`)
		conn.Close()
		next.Close()
	}()

	var out syncBuffer
	require.NoError(t, Follow(path, &out, func(string) {}))
	assert.Equal(t, "first boot\nsecond boot\n", out.String())
}

// The event goes ahead of queued serial, so what's left of the old boot arrives after it.
func TestFollowKeepsReadingAfterTheRestartEvent(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	go func() {
		conn, err := listener.Accept()
		require.NoError(t, err)
		reader := bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		fmt.Fprintln(conn, `{"event":"restarting"}`)
		fmt.Fprintln(conn, `{"event":"serial","data":"`+base64.StdEncoding.EncodeToString([]byte("last words\n"))+`"}`)
		conn.Close()
		listener.Close()
		next, err := net.Listen("unix", path)
		require.NoError(t, err)
		conn, err = next.Accept()
		require.NoError(t, err)
		reader = bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		fmt.Fprintln(conn, `{"event":"serial","data":"`+base64.StdEncoding.EncodeToString([]byte("new boot\n"))+`"}`)
		conn.Close()
		next.Close()
	}()

	var out syncBuffer
	require.NoError(t, Follow(path, &out, func(string) {}))
	assert.Equal(t, "last words\nnew boot\n", out.String())
}

// With the process alive, a refused socket is a boot taking its time.
func TestFollowWaitsForASlowRebootAfterAHangUp(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	listener.(*net.UnixListener).SetUnlinkOnClose(false)
	go func() {
		conn, err := listener.Accept()
		require.NoError(t, err)
		reader := bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1,"pid":`+strconv.Itoa(os.Getpid())+`}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		conn.Close()
		listener.Close()

		time.Sleep(time.Second)
		os.Remove(path)
		next, err := net.Listen("unix", path)
		require.NoError(t, err)
		conn, err = next.Accept()
		require.NoError(t, err)
		reader = bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		fmt.Fprintln(conn, `{"event":"serial","data":"`+base64.StdEncoding.EncodeToString([]byte("slow boot\n"))+`"}`)
		conn.Close()
		next.Close()
	}()

	var out syncBuffer
	require.NoError(t, Follow(path, &out, func(string) {}))
	assert.Equal(t, "slow boot\n", out.String())
}

// A dead process is no reboot, however long its socket file stays.
func TestFollowStopsAtOnceWhenTheProcessHasGone(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	listener.(*net.UnixListener).SetUnlinkOnClose(false)
	gone := exec.Command("true")
	require.NoError(t, gone.Run())
	go func() {
		conn, err := listener.Accept()
		require.NoError(t, err)
		reader := bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1,"pid":`+strconv.Itoa(gone.Process.Pid)+`}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		conn.Close()
		listener.Close()
	}()

	start := time.Now()
	require.NoError(t, Follow(path, &syncBuffer{}, func(string) {}))
	assert.Less(t, time.Since(start), 2*time.Second)
}

func TestFollowPassesOnDrops(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	go func() {
		conn, err := listener.Accept()
		require.NoError(t, err)
		reader := bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		fmt.Fprintln(conn, `{"event":"dropped","bytes":5}`)
		conn.Close()
		listener.Close()
	}()

	var warnings []string
	require.NoError(t, Follow(path, &syncBuffer{}, func(m string) { warnings = append(warnings, m) }))
	require.Len(t, warnings, 1)
	assert.Contains(t, warnings[0], "5 bytes")
}

func TestFollowSaysWhenNoAppIsRunning(t *testing.T) {
	assert.ErrorIs(t, Follow(shortPath(t), &syncBuffer{}, func(string) {}), ErrNotRunning)
}

// A pixel of each primary, then white, as little-endian RGB565.
func testFrame() Frame {
	return Frame{Width: 2, Height: 2, Format: "rgb565le", Data: []byte{0x00, 0xf8, 0xe0, 0x07, 0x1f, 0x00, 0xff, 0xff}}
}

func TestSavePNG(t *testing.T) {
	path := filepath.Join(t.TempDir(), "shot.png")
	require.NoError(t, testFrame().Save(path))

	file, err := os.Open(path)
	require.NoError(t, err)
	defer file.Close()
	img, err := png.Decode(file)
	require.NoError(t, err)
	assert.Equal(t, 2, img.Bounds().Dx())
	check := func(x, y int, r, g, b uint32) {
		t.Helper()
		gotR, gotG, gotB, gotA := img.At(x, y).RGBA()
		assert.Equal(t, [4]uint32{r, g, b, 255}, [4]uint32{gotR >> 8, gotG >> 8, gotB >> 8, gotA >> 8})
	}
	check(0, 0, 255, 0, 0)
	check(1, 0, 0, 255, 0)
	check(0, 1, 0, 0, 255)
	check(1, 1, 255, 255, 255)
}

func TestSaveBMPMatchesTheRuntimesLayout(t *testing.T) {
	path := filepath.Join(t.TempDir(), "shot.bmp")
	require.NoError(t, testFrame().Save(path))

	data, err := os.ReadFile(path)
	require.NoError(t, err)
	assert.Equal(t, "BM", string(data[:2]))
	assert.Len(t, data, 54+2*8)
	// Rows run bottom up, each padded to four bytes, with pixels as blue, green, red.
	assert.Equal(t, []byte{255, 0, 0, 255, 255, 255, 0, 0}, data[54:62])
	assert.Equal(t, []byte{0, 0, 255, 0, 255, 0, 0, 0}, data[62:70])
}

func TestSaveRefusesImpossibleDimensions(t *testing.T) {
	path := filepath.Join(t.TempDir(), "shot.bmp")
	for _, frame := range []Frame{
		{Width: 0, Height: 0, Format: "rgb565le"},
		{Width: -1, Height: -2, Format: "rgb565le", Data: make([]byte, 4)},
		{Width: 1 << 62, Height: 8, Format: "rgb565le"},
		{Width: 1 << 32, Height: 1 << 31, Format: "rgb565le"},
	} {
		assert.Error(t, frame.Save(path), "%+v", frame)
		assert.NoFileExists(t, path)
	}
}

func TestSaveRefusesOtherFormatsAndShortFrames(t *testing.T) {
	dir := t.TempDir()
	err := testFrame().Save(filepath.Join(dir, "shot.gif"))
	require.Error(t, err)
	assert.Contains(t, err.Error(), ".png")
	assert.NoFileExists(t, filepath.Join(dir, "shot.gif"))

	short := testFrame()
	short.Data = short.Data[:4]
	assert.Error(t, short.Save(filepath.Join(dir, "shot.png")))

	other := testFrame()
	other.Format = "rgb888"
	assert.Error(t, other.Save(filepath.Join(dir, "shot.png")))
}

type syncBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *syncBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *syncBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}
