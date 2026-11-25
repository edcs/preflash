package web

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	uv "github.com/charmbracelet/ultraviolet"
	"github.com/charmbracelet/x/ansi"
	"github.com/charmbracelet/x/vt"
	"github.com/creack/pty"
	"github.com/stretchr/testify/require"
	"golang.org/x/term"
)

const termPatience = 30 * time.Second

// TestMain runs the test binary as a fake terminal app when PREFLASH_WEB_FAKE names how it
// should behave, so the terminal tests need no real app.
func TestMain(m *testing.M) {
	if behaviour := os.Getenv("PREFLASH_WEB_FAKE"); behaviour != "" {
		fakeTUI(behaviour)
		return
	}
	os.Exit(m.Run())
}

// fakeTUI takes its terminal raw, as the real app does, so ^C arrives as a byte, then says
// "ready" and its size and behaves as asked:
//
//	echo            prints each line typed as "echo:<line>", and its size on SIGWINCH
//	query           asks for the device attributes and exits 0 if they come within 2 s
//	print           prints PREFLASH_WEB_PRINT
//	flood           prints 3 MB and then an end marker
//	ignore-ctrl-c   starts a sleeping child in its group, prints its pid, ignores ^C
//	exit-on-ctrl-c  exits 0 on ^C
//	deaf            never reads what's typed, and asks for the device attributes every 10 ms
//
// Every behaviour but ignore-ctrl-c and deaf exits 0 on ^C.
func fakeTUI(behaviour string) {
	if _, err := term.MakeRaw(0); err != nil {
		fmt.Println("raw:", err)
		os.Exit(2)
	}
	out := os.Stdout
	cols, rows := fakeSize()
	fmt.Fprintf(out, "ready %dx%d\r\n", cols, rows)

	switch behaviour {
	case "query":
		out.WriteString("\x1b[c")
		reply := make(chan []byte)
		go func() {
			b := make([]byte, 64)
			n, _ := os.Stdin.Read(b)
			reply <- b[:n]
		}()
		select {
		case b := <-reply:
			if bytes.HasPrefix(b, []byte("\x1b[?")) {
				os.Exit(0)
			}
		case <-time.After(2 * time.Second):
		}
		os.Exit(1)
	case "print":
		out.WriteString(os.Getenv("PREFLASH_WEB_PRINT"))
	case "flood":
		line := strings.Repeat("x", 98) + "\r\n"
		block := strings.Repeat(line, 1000)
		for range 3 * 1024 * 1024 / len(block) {
			out.WriteString(block)
		}
		out.WriteString("END-OF-FLOOD\r\n")
	case "ignore-ctrl-c":
		signal.Ignore(os.Interrupt)
		child := exec.Command("sleep", "300")
		if err := child.Start(); err != nil {
			fmt.Fprintf(out, "sleep: %v\r\n", err)
			os.Exit(2)
		}
		fmt.Fprintf(out, "child %d ok\r\n", child.Process.Pid)
		for {
			b := make([]byte, 64)
			if _, err := os.Stdin.Read(b); err != nil {
				select {}
			}
		}
	case "deaf":
		for {
			out.WriteString("\x1b[c")
			time.Sleep(10 * time.Millisecond)
		}
	case "echo":
		winch := make(chan os.Signal, 1)
		signal.Notify(winch, syscall.SIGWINCH)
		go func() {
			for range winch {
				cols, rows := fakeSize()
				fmt.Fprintf(out, "size:%dx%d\r\n", cols, rows)
			}
		}()
	}

	var line []byte
	b := make([]byte, 1)
	for {
		if _, err := os.Stdin.Read(b); err != nil {
			os.Exit(3)
		}
		switch b[0] {
		case 0x03:
			os.Exit(0)
		case '\r':
			if behaviour == "echo" {
				fmt.Fprintf(out, "echo:%s\r\n", line)
			}
			line = line[:0]
		default:
			line = append(line, b[0])
		}
	}
}

func fakeSize() (cols, rows int) {
	rows, cols, err := pty.Getsize(os.Stdin)
	if err != nil {
		return 0, 0
	}
	return cols, rows
}

// startFake starts a terminal running the fake app with this behaviour, closed at the end
// of the test.
func startFake(t *testing.T, behaviour string) *terminal {
	t.Helper()
	term := newFake(t, behaviour)
	require.NoError(t, term.Start())
	return term
}

func newFake(t *testing.T, behaviour string) *terminal {
	t.Helper()
	t.Setenv("PREFLASH_WEB_FAKE", behaviour)
	// A race-enabled binary sleeps a second as it exits, unless told not to.
	t.Setenv("GORACE", "atexit_sleep_ms=0")
	term, err := newTerminal([]string{os.Args[0]}, 120, 40)
	require.NoError(t, err)
	t.Cleanup(func() { term.Close() })
	return term
}

// reader collects what an attachment receives, in the background.
type reader struct {
	got  chan string
	done chan struct{}
}

func read(live <-chan []byte) *reader {
	r := &reader{got: make(chan string, 1), done: make(chan struct{})}
	go func() {
		defer close(r.done)
		var all []byte
		for chunk := range live {
			all = append(all, chunk...)
			select {
			case <-r.got:
			default:
			}
			r.got <- string(all)
		}
	}()
	return r
}

// until waits for what the reader received to contain want.
func (r *reader) until(t *testing.T, want string) string {
	t.Helper()
	deadline := time.After(termPatience)
	last := ""
	for {
		select {
		case all := <-r.got:
			last = all
			if strings.Contains(all, want) {
				return all
			}
		case <-r.done:
			t.Fatalf("live closed before %q arrived; got %q", want, tail(last))
		case <-deadline:
			t.Fatalf("%q never arrived; got %q", want, tail(last))
		}
	}
}

func tail(s string) string {
	if len(s) > 200 {
		return "…" + s[len(s)-200:]
	}
	return s
}

func waitExited(t *testing.T, term *terminal) {
	t.Helper()
	select {
	case <-term.Exited():
	case <-time.After(termPatience):
		t.Fatal("the app never exited")
	}
}

func TestOutputReachesEveryAttachedReader(t *testing.T) {
	term := newFake(t, "echo")
	_, live1, detach1 := term.Attach()
	defer detach1()
	_, live2, detach2 := term.Attach()
	defer detach2()
	one, two := read(live1), read(live2)
	require.NoError(t, term.Start())
	one.until(t, "ready")

	require.NoError(t, term.Input([]byte("hi\r")))
	one.until(t, "echo:hi")
	two.until(t, "echo:hi")
}

// screenText is one row of an emulator's screen, as text.
func screenText(screen *vt.SafeEmulator, y int) string {
	var row strings.Builder
	for x := 0; x < screen.Width(); {
		cell := screen.CellAt(x, y)
		if cell == nil || cell.Content == "" {
			row.WriteByte(' ')
			x++
			continue
		}
		row.WriteString(cell.Content)
		x += max(cell.Width, 1)
	}
	return strings.TrimRight(row.String(), " ")
}

func TestALateReaderGetsTheScreen(t *testing.T) {
	const done = "\x1b[10;20H"
	t.Setenv("PREFLASH_WEB_PRINT", "\x1b[?1049h\x1b[2J\x1b[?25l\x1b[?1000h\x1b[?1006h\x1b[?2004h\x1b[?1h"+
		"\x1b[4;1H\x1b[1;31mhello\x1b[0m \x1b[48;2;10;20;30m wide 世界 \x1b[0m"+
		"\x1b[40;1H\x1b[4;38;5;200mbottom\x1b[0m\x1b[5 q"+done)
	term := newFake(t, "print")
	_, early, detach := term.Attach()
	defer detach()
	first := read(early)
	require.NoError(t, term.Start())
	first.until(t, done)

	snapshot, _, detachLate := term.Attach()
	defer detachLate()

	fresh := vt.NewSafeEmulator(120, 40)
	enabled := map[ansi.Mode]bool{}
	fresh.SetCallbacks(vt.Callbacks{
		EnableMode:  func(m ansi.Mode) { enabled[m] = true },
		DisableMode: func(m ansi.Mode) { enabled[m] = false },
	})
	go func() {
		b := make([]byte, 256)
		for {
			if _, err := fresh.Read(b); err != nil {
				return
			}
		}
	}()
	_, err := fresh.Write(snapshot)
	require.NoError(t, err)

	require.Equal(t, "hello  wide 世界", screenText(fresh, 3))
	require.Equal(t, "bottom", screenText(fresh, 39))
	hello := fresh.CellAt(0, 3)
	require.NotZero(t, hello.Style.Attrs&uv.AttrBold, "hello is bold")
	require.Equal(t, ansi.Red, hello.Style.Fg)

	source := term.screen()
	for y := range 40 {
		for x := range 120 {
			want, got := source.CellAt(x, y), fresh.CellAt(x, y)
			require.Equal(t, want.Content, got.Content, "cell %d,%d", x, y)
			require.True(t, want.Style.Equal(&got.Style), "style of cell %d,%d: want %+v, got %+v", x, y, want.Style, got.Style)
		}
	}
	require.Equal(t, uv.Pos(19, 9), fresh.CursorPosition())
	require.True(t, fresh.IsAltScreen(), "alternate screen")
	require.True(t, fresh.CursorHidden(), "cursor hidden")
	style, blink := fresh.CursorStyle()
	require.Equal(t, vt.CursorBar, style)
	require.True(t, blink)
	for _, mode := range []ansi.Mode{ansi.ModeMouseNormal, ansi.ModeMouseExtSgr, ansi.ModeBracketedPaste, ansi.ModeCursorKeys} {
		require.True(t, enabled[mode], "mode %v", mode)
	}
}

func TestQueriesAreAnsweredWithNoTabOpen(t *testing.T) {
	term := startFake(t, "query")
	waitExited(t, term)
	require.Equal(t, 0, term.ExitCode())
	require.False(t, term.Running())
}

func TestASlowReaderIsCutOffNotTheChild(t *testing.T) {
	term := newFake(t, "flood")
	_, stuck, detachStuck := term.Attach()
	defer detachStuck()
	_, live, detach := term.Attach()
	defer detach()
	fast := read(live)
	require.NoError(t, term.Start())
	fast.until(t, "END-OF-FLOOD")

	// The stuck reader keeps its full buffer and is then closed.
	for chunks := 0; ; chunks++ {
		select {
		case _, ok := <-stuck:
			if !ok {
				require.Equal(t, liveBuffer, chunks)
				return
			}
		default:
			t.Fatalf("the stuck reader wasn't cut off after %d chunks", chunks)
		}
	}
}

func TestSizesOutOfRangeAreRefused(t *testing.T) {
	for _, size := range [][2]int{{0, 40}, {120, 0}, {-1, 40}, {1001, 40}, {120, 1001}, {70000, 40}} {
		_, err := newTerminal([]string{"true"}, size[0], size[1])
		require.Error(t, err, "%v", size)
	}
	term, err := newTerminal([]string{"true"}, 1000, 1)
	require.NoError(t, err)
	require.Error(t, term.Resize(0, 24))
	require.Error(t, term.Resize(80, 65536))
	require.NoError(t, term.Resize(1, 1000))
}

func TestResizeReachesTheChild(t *testing.T) {
	term := newFake(t, "echo")
	_, live, detach := term.Attach()
	defer detach()
	r := read(live)
	require.NoError(t, term.Start())
	r.until(t, "ready 120x40")

	require.NoError(t, term.Resize(80, 24))
	r.until(t, "size:80x24")
	screen := term.screen()
	require.Equal(t, 80, screen.Width())
	require.Equal(t, 24, screen.Height())
}

func TestStartAgainAfterExit(t *testing.T) {
	term := newFake(t, "exit-on-ctrl-c")
	_, live, detach := term.Attach()
	defer detach()
	r := read(live)
	require.NoError(t, term.Start())
	r.until(t, "ready")
	require.True(t, term.Running())

	require.NoError(t, term.Input([]byte("\x03")))
	waitExited(t, term)
	require.False(t, term.Running())
	require.Error(t, term.Input([]byte("x")), "nothing to type into")

	require.NoError(t, term.Start())
	require.True(t, term.Running())
	select {
	case <-term.Exited():
		t.Fatal("Start gives a new Exited")
	default:
	}
	r.until(t, "ready 120x40\r\nready")
}

func TestCloseKillsAChildThatIgnoresCtrlC(t *testing.T) {
	was := closePatience
	closePatience = time.Second
	t.Cleanup(func() { closePatience = was })

	term := newFake(t, "ignore-ctrl-c")
	_, live, detach := term.Attach()
	defer detach()
	r := read(live)
	require.NoError(t, term.Start())
	got := r.until(t, " ok")
	pid := term.pid()
	_, rest, _ := strings.Cut(got, "child ")
	grandchild, err := strconv.Atoi(strings.Fields(rest)[0])
	require.NoError(t, err)

	start := time.Now()
	require.NoError(t, term.Close())
	require.Less(t, time.Since(start), 2*time.Second)
	require.True(t, gone(pid), "the app is gone")
	// The orphaned grandchild is reaped by whoever adopts it, which takes a moment.
	deadline := time.Now().Add(termPatience)
	for !gone(grandchild) {
		require.True(t, time.Now().Before(deadline), "the app's child is gone")
		time.Sleep(10 * time.Millisecond)
	}
	select {
	case <-r.done:
	case <-time.After(termPatience):
		t.Fatal("Close leaves live open")
	}
}

func gone(pid int) bool {
	return errors.Is(syscall.Kill(pid, 0), syscall.ESRCH)
}

func TestDetachTwiceAndAfterClose(t *testing.T) {
	term := startFake(t, "echo")
	_, live, detach := term.Attach()
	detach()
	detach()
	_, ok := <-live
	require.False(t, ok)

	_, live2, detach2 := term.Attach()
	require.NoError(t, term.Close())
	_, ok = <-live2
	require.False(t, ok)
	detach2()
	require.NoError(t, term.Close(), "Close twice")
	require.Error(t, term.Start(), "no Start after Close")
}

// TestAWedgedAppCannotHangTheSession fills the input of an app that never reads it, so
// typing and the screen's replies to its queries both block, and checks that the output,
// Attach and Close carry on regardless.
func TestAWedgedAppCannotHangTheSession(t *testing.T) {
	was := closePatience
	closePatience = 500 * time.Millisecond
	t.Cleanup(func() { closePatience = was })

	before := runtime.NumGoroutine()
	term := newFake(t, "deaf")
	_, live, detach := term.Attach()
	defer detach()
	r := read(live)
	require.NoError(t, term.Start())
	r.until(t, "ready")
	pid := term.pid()

	typed := make(chan error, 1)
	go func() { typed <- term.Input(bytes.Repeat([]byte("x"), 1<<20)) }()
	// The app keeps asking long after its input is full.
	r.until(t, strings.Repeat("\x1b[c", 100))
	// macOS drops what doesn't fit in a raw terminal's input, so there it never fills.
	if runtime.GOOS == "linux" {
		select {
		case err := <-typed:
			t.Fatalf("the input never filled: %v", err)
		default:
		}
	}

	attached := make(chan struct{})
	go func() {
		_, _, detach := term.Attach()
		detach()
		close(attached)
	}()
	select {
	case <-attached:
	case <-time.After(5 * time.Second):
		t.Fatal("Attach hangs while the input is stuck")
	}

	closed := make(chan error, 1)
	start := time.Now()
	go func() { closed <- term.Close() }()
	select {
	case err := <-closed:
		require.NoError(t, err)
		require.Less(t, time.Since(start), 2*closePatience+500*time.Millisecond)
	case <-time.After(10 * time.Second):
		t.Fatal("Close hangs while the input is stuck")
	}
	require.True(t, gone(pid), "the app is gone")
	select {
	case <-typed:
	case <-time.After(termPatience):
		t.Fatal("Input stays blocked after Close")
	}
	<-r.done
	settles(t, before)
}

// TestNothingIsLeftRunning checks that an exit and a Close each leave no goroutine behind.
func TestNothingIsLeftRunning(t *testing.T) {
	before := runtime.NumGoroutine()
	term := newFake(t, "exit-on-ctrl-c")
	_, live, detach := term.Attach()
	r := read(live)
	require.NoError(t, term.Start())
	r.until(t, "ready")
	require.NoError(t, term.Input([]byte("\x03")))
	waitExited(t, term)
	detach()
	<-r.done
	settles(t, before)

	_, live, _ = term.Attach()
	r = read(live)
	require.NoError(t, term.Start())
	r.until(t, "ready")
	require.NoError(t, term.Close())
	<-r.done
	settles(t, before)
}

// settles waits for the goroutine count to fall back to what it was.
func settles(t *testing.T, before int) {
	t.Helper()
	deadline := time.Now().Add(termPatience)
	for runtime.NumGoroutine() > before {
		if time.Now().After(deadline) {
			buf := make([]byte, 1<<20)
			t.Fatalf("%d goroutines, not %d:\n%s", runtime.NumGoroutine(), before, buf[:runtime.Stack(buf, true)])
		}
		time.Sleep(10 * time.Millisecond)
	}
}

// screen is the virtual terminal, for tests.
func (t *terminal) screen() *vt.SafeEmulator {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.emu
}

// pid is the app's process id, for tests.
func (t *terminal) pid() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.cmd.Process.Pid
}
