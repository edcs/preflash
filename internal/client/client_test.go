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
		reply(fmt.Sprintf(`{"id":%d,"ok":true,"project":"demo","board":"B","width":480,"height":320,"pid":7,"uptime_ms":1500,"backlight":40,"rotation":90,"frames":12,"fps":3,"wifi":"connected","serial_port":"/tmp/x.tty"}`, int(req["id"].(float64))))
	})
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()

	got, err := c.Status()
	require.NoError(t, err)
	assert.Equal(t, Status{Project: "demo", Board: "B", Width: 480, Height: 320, PID: 7, UptimeMs: 1500, Backlight: 40, Rotation: 90, Frames: 12, FPS: 3, WiFi: "connected", SerialPort: "/tmp/x.tty"}, got)
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

// A restart can land on a rebuilt app, and that's no reason to report a stopped one.
func TestFollowSaysWhenTheNextBootSpeaksAnotherProtocol(t *testing.T) {
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
		conn.Close()
		listener.Close()
		next, err := net.Listen("unix", path)
		require.NoError(t, err)
		defer next.Close()
		conn, err = next.Accept()
		require.NoError(t, err)
		bufio.NewReader(conn).ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":99}`)
		conn.Close()
	}()

	err = Follow(path, &syncBuffer{}, func(string) {})
	require.Error(t, err)
	assert.Contains(t, err.Error(), "99")
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

// A quit that lands just as a restart execs goes unanswered by the new boot.
func TestQuitAsksAgainIfTheAppCameBack(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	listener.(*net.UnixListener).SetUnlinkOnClose(false)
	stopped := make(chan struct{})
	go func() {
		conn, err := listener.Accept()
		require.NoError(t, err)
		reader := bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		conn.Close()
		listener.Close()
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
		next.Close() // removes the socket, as a real quit does
		conn.Close()
		close(stopped)
	}()

	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()
	require.NoError(t, c.Quit())
	select {
	case <-stopped:
	case <-time.After(2 * time.Second):
		t.Fatal("the second boot never heard a quit")
	}
}

// The exec can close the connection before the reply, and the new boot can take a while.
func TestQuitOutlastsASlowRestartThatSwallowedIt(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	listener.(*net.UnixListener).SetUnlinkOnClose(false)
	stopped := make(chan struct{})
	go func() {
		conn, err := listener.Accept()
		require.NoError(t, err)
		reader := bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1,"pid":`+strconv.Itoa(os.Getpid())+`}`)
		reader.ReadBytes('\n')
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
		next.Close()
		conn.Close()
		close(stopped)
	}()

	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()
	require.NoError(t, c.Quit())
	select {
	case <-stopped:
	case <-time.After(2 * time.Second):
		t.Fatal("the second boot never heard a quit")
	}
}

// A quit can overtake a restart that was already announced, which is still a clean stop.
func TestFollowEndsCleanlyWhenARestartBecomesAQuit(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
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
		fmt.Fprintln(conn, `{"event":"restarting"}`)
		listener.Close() // removes the socket, as a real quit does
		conn.Close()
	}()

	assert.NoError(t, Follow(path, &syncBuffer{}, func(string) {}))
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

func TestWatchReportsEachBootAndWhatHappensInIt(t *testing.T) {
	path := shortPath(t)
	listener, err := net.Listen("unix", path)
	require.NoError(t, err)
	serve := func(l net.Listener, lines ...string) {
		conn, err := l.Accept()
		require.NoError(t, err)
		reader := bufio.NewReader(conn)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":1,"project":"demo"}`)
		reader.ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":2,"ok":true}`)
		for _, line := range lines {
			fmt.Fprintln(conn, line)
		}
		conn.Close()
		l.Close()
	}
	go func() {
		serve(listener, `{"event":"serial","data":"b25l"}`, `{"event":"dropped","bytes":9}`, `{"event":"restarting"}`)
		next, err := net.Listen("unix", path)
		require.NoError(t, err)
		serve(next, `{"event":"serial","data":"dHdv"}`)
	}()

	var log []string
	err = Watch(path, Watcher{
		Connected:  func(h Hello) { log = append(log, "connected "+h.Project) },
		Serial:     func(b []byte) error { log = append(log, "serial "+string(b)); return nil },
		Dropped:    func(n int) { log = append(log, fmt.Sprintf("dropped %d", n)) },
		Restarting: func() { log = append(log, "restarting") },
	})

	require.NoError(t, err)
	assert.Equal(t, []string{"connected demo", "serial one", "dropped 9", "restarting", "connected demo", "serial two"}, log)
}

func TestSetDeadlineStopsACallToAnAppThatHasGoneQuiet(t *testing.T) {
	path := shortPath(t)
	startFake(t, path, func(req map[string]any, reply func(string)) {})
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()

	require.NoError(t, c.SetDeadline(time.Now().Add(50*time.Millisecond)))
	_, err = c.Status()

	assert.ErrorIs(t, err, ErrStopped)
}

func TestAwaitWaitsForASocketToAppear(t *testing.T) {
	path := shortPath(t)
	go func() {
		time.Sleep(200 * time.Millisecond)
		startFake(t, path, func(req map[string]any, reply func(string)) { reply(ok(req)) })
	}()
	c, err := Await(path, 5*time.Second, os.Getpid())
	require.NoError(t, err)
	c.Close()
}

func TestAwaitStopsWhenTheProcessDies(t *testing.T) {
	cmd := exec.Command("true")
	require.NoError(t, cmd.Run())
	start := time.Now()

	_, err := Await(shortPath(t), time.Minute, cmd.Process.Pid)

	assert.ErrorIs(t, err, ErrNotRunning)
	assert.Less(t, time.Since(start), time.Second)
}

func TestWatchHearsTheTraceWhenAskedAndStopsWhenTold(t *testing.T) {
	path := shortPath(t)
	app := startFake(t, path, func(req map[string]any, reply func(string)) {
		reply(ok(req))
		reply(`{"event":"trace","stream":"i2c","seq":3,"us":251204000,"bus":1,"address":81,"subject":"1:0x51","device":"PCF85063","op":"read","detail":"NACK (injected)","injected":true}`)
	})

	rows := make(chan TraceRecord, 1)
	stop := make(chan struct{})
	finished := make(chan error, 1)
	go func() {
		finished <- Watch(path, Watcher{
			Streams: []string{"trace"},
			Stop:    stop,
			Trace:   func(r TraceRecord) { rows <- r },
		})
	}()

	row := <-rows
	assert.Equal(t, "i2c", row.Stream)
	assert.Equal(t, uint64(3), row.Seq)
	assert.Equal(t, 1, row.Bus)
	assert.Equal(t, 81, row.Address)
	assert.Equal(t, "PCF85063", row.Device)
	assert.Equal(t, "NACK (injected)", row.Detail)
	assert.True(t, row.Injected)
	assert.Equal(t, "04:11.204", row.Time())
	assert.Equal(t, []any{"trace"}, app.seen()[0]["streams"])

	close(stop)
	select {
	case err := <-finished:
		assert.NoError(t, err)
	case <-time.After(time.Second):
		t.Fatal("Watch outlasted its stop")
	}
}

func TestEventsKeepTheirTraceRowsWhileACallWaits(t *testing.T) {
	path := shortPath(t)
	startFake(t, path, func(req map[string]any, reply func(string)) {
		reply(`{"event":"trace","stream":"net","seq":1,"us":1,"subject":"wifi","op":"join","detail":"x","injected":false}`)
		reply(ok(req))
	})
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()
	require.NoError(t, c.Send("x"))
	event, err := c.Next()
	require.NoError(t, err)
	assert.Equal(t, "trace", event.Kind)
	assert.Equal(t, "join", event.Trace.Op)
}

func TestTheTimeOfARowShowsHoursOnceThereAreAny(t *testing.T) {
	assert.Equal(t, "1:00:00.001", TraceRecord{Us: 3600_001_000}.Time())
}

func TestDebugCallsReadTheirReplies(t *testing.T) {
	path := shortPath(t)
	startFake(t, path, func(req map[string]any, reply func(string)) {
		id := int(req["id"].(float64))
		switch req["op"] {
		case "tasks":
			reply(fmt.Sprintf(`{"id":%d,"ok":true,"tasks":[{"name":"main","state":"blocked","stack":3584}]}`, id))
		case "nvs":
			reply(fmt.Sprintf(`{"id":%d,"ok":true,"namespaces":[{"name":"wifi","keys":[{"key":"ssid","size":6}]}]}`, id))
		case "hardware", "hardware_set":
			reply(fmt.Sprintf(`{"id":%d,"ok":true,"items":[{"name":"rtc","label":"RTC","group":"Break something","applies":"boot","value":"healthy","next":"flat","options":[{"value":"healthy","label":"healthy"},{"value":"flat","label":"flat backup cell"}]}]}`, id))
		case "trace_save":
			reply(fmt.Sprintf(`{"id":%d,"ok":true,"path":"/saved","records":12}`, id))
		}
	})
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()

	tasks, err := c.Tasks()
	require.NoError(t, err)
	assert.Equal(t, []Task{{Name: "main", State: "blocked", Stack: 3584}}, tasks)

	spaces, err := c.NVS()
	require.NoError(t, err)
	assert.Equal(t, []Namespace{{Name: "wifi", Keys: []NVSKey{{Key: "ssid", Size: 6}}}}, spaces)

	items, err := c.Hardware()
	require.NoError(t, err)
	require.Len(t, items, 1)
	assert.Equal(t, "flat backup cell", items[0].Label("flat"))
	assert.True(t, items[0].Waiting())

	items, err = c.SetHardware("rtc", "flat")
	require.NoError(t, err)
	assert.Len(t, items, 1)

	saved, records, err := c.SaveTrace("")
	require.NoError(t, err)
	assert.Equal(t, "/saved", saved)
	assert.Equal(t, 12, records)
}

func TestSavingWithNoPathLeavesItOutOfTheRequest(t *testing.T) {
	path := shortPath(t)
	app := startFake(t, path, func(req map[string]any, reply func(string)) {
		reply(fmt.Sprintf(`{"id":%d,"ok":true,"path":"/s","records":0}`, int(req["id"].(float64))))
	})
	c, err := Dial(path)
	require.NoError(t, err)
	defer c.Close()
	_, _, err = c.SaveTrace("")
	require.NoError(t, err)
	assert.NotContains(t, app.seen()[0], "path")
}
