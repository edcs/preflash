package web

import (
	"errors"
	"io"
	"os"
	"os/exec"
	"sync"
	"syscall"
	"time"

	"github.com/charmbracelet/x/ansi"
	"github.com/charmbracelet/x/vt"
	"github.com/creack/pty"
)

// closePatience is how long Close waits for the terminal app to quit on ^C before it kills
// the app's process group.
var closePatience = 10 * time.Second

// liveBuffer is how many chunks of output an attached reader may fall behind by before it
// is cut off.
const liveBuffer = 256

var (
	errRunning    = errors.New("the terminal app is already running")
	errNotRunning = errors.New("the terminal app isn't running")
	errClosed     = errors.New("the terminal is closed")
)

// terminal runs the terminal app in a pseudo-terminal, keeps a virtual terminal in step with
// it so a tab that attaches late gets the screen, and passes the app's output to every
// attached tab.
type terminal struct {
	argv []string

	mu         sync.Mutex
	cols, rows int
	emu        *vt.SafeEmulator
	modes      map[ansi.Mode]bool // the modes the app set or reset on emu
	ptmx       *os.File
	cmd        *exec.Cmd
	running    bool
	exited     chan struct{}
	exitCode   int
	readers    map[*attachment]struct{}
	closed     bool
}

// attachment is one tab's feed of the app's output.
type attachment struct {
	live chan []byte
}

func newTerminal(argv []string, cols, rows int) *terminal {
	t := &terminal{
		argv:    argv,
		cols:    cols,
		rows:    rows,
		exited:  make(chan struct{}),
		readers: map[*attachment]struct{}{},
	}
	t.emu, t.modes = t.newScreen()
	return t
}

// newScreen is a blank virtual terminal whose mode changes are noted in the map it returns.
// Its callbacks run inside emu.Write, which only the output loop calls, under t.mu.
func (t *terminal) newScreen() (*vt.SafeEmulator, map[ansi.Mode]bool) {
	emu := vt.NewSafeEmulator(t.cols, t.rows)
	modes := map[ansi.Mode]bool{}
	emu.SetCallbacks(vt.Callbacks{
		EnableMode:  func(m ansi.Mode) { modes[m] = true },
		DisableMode: func(m ansi.Mode) { modes[m] = false },
	})
	return emu, modes
}

// Start runs the app on a fresh screen at the terminal's size.
func (t *terminal) Start() error {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.closed {
		return errClosed
	}
	if t.running {
		return errRunning
	}
	cmd := exec.Command(t.argv[0], t.argv[1:]...)
	cmd.Env = append(os.Environ(), "TERM=xterm-256color", "COLORTERM=truecolor")
	// StartWithSize makes the app a session and process group leader, so Close can kill its group.
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	ptmx, err := pty.StartWithSize(cmd, &pty.Winsize{Cols: uint16(t.cols), Rows: uint16(t.rows)})
	if err != nil {
		return err
	}
	emu, modes := t.newScreen()
	t.emu, t.modes = emu, modes
	t.ptmx, t.cmd, t.running = ptmx, cmd, true
	select {
	case <-t.exited:
		t.exited = make(chan struct{})
	default:
	}

	// What the screen answers, such as replies to the app's queries, goes back to the app.
	// Once the app has gone, the rest is thrown away, so the screen never blocks on a reply.
	replies := emu.InputPipe().(io.Closer)
	go func() {
		io.Copy(ptmx, emu)
		io.Copy(io.Discard, emu)
	}()
	go t.output(cmd, ptmx, emu, replies, t.exited)
	return nil
}

// output feeds what the app prints to the screen and to every attached reader, until the
// app has gone.
func (t *terminal) output(cmd *exec.Cmd, ptmx *os.File, emu *vt.SafeEmulator, replies io.Closer, exited chan struct{}) {
	buf := make([]byte, 32*1024)
	for {
		n, err := ptmx.Read(buf)
		if n > 0 {
			chunk := append([]byte(nil), buf[:n]...)
			t.mu.Lock()
			emu.Write(chunk)
			for r := range t.readers {
				select {
				case r.live <- chunk:
				default:
					// Too slow: cut it off rather than hold up the app. It redraws on reattach.
					delete(t.readers, r)
					close(r.live)
				}
			}
			t.mu.Unlock()
		}
		if err != nil {
			break
		}
	}
	replies.Close()
	cmd.Wait()
	ptmx.Close()

	t.mu.Lock()
	t.running = false
	t.ptmx = nil
	t.exitCode = cmd.ProcessState.ExitCode()
	close(exited)
	t.mu.Unlock()
}

// Attach gives the escape bytes that draw the current screen on a blank terminal, and the
// app's output from then on. The chunks on live are shared and must not be changed. live is
// closed by detach, by Close, or when the reader falls liveBuffer chunks behind.
func (t *terminal) Attach() (snapshot []byte, live <-chan []byte, detach func()) {
	t.mu.Lock()
	defer t.mu.Unlock()
	r := &attachment{live: make(chan []byte, liveBuffer)}
	snapshot = drawScreen(t.emu, t.modes)
	if t.closed {
		close(r.live)
	} else {
		t.readers[r] = struct{}{}
	}
	detach = func() {
		t.mu.Lock()
		defer t.mu.Unlock()
		if _, ok := t.readers[r]; ok {
			delete(t.readers, r)
			close(r.live)
		}
	}
	return snapshot, r.live, detach
}

// Input types into the app. It goes straight to the pty, not through the screen.
func (t *terminal) Input(b []byte) error {
	t.mu.Lock()
	ptmx := t.ptmx
	t.mu.Unlock()
	if ptmx == nil {
		return errNotRunning
	}
	_, err := ptmx.Write(b)
	return err
}

// Resize changes the size of the screen and of the app's terminal, now and for later starts.
func (t *terminal) Resize(cols, rows int) error {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.cols, t.rows = cols, rows
	t.emu.Resize(cols, rows)
	if t.ptmx == nil {
		return nil
	}
	return pty.Setsize(t.ptmx, &pty.Winsize{Cols: uint16(cols), Rows: uint16(rows)})
}

// Exited is closed when the app exits. Start replaces it for the next run.
func (t *terminal) Exited() <-chan struct{} {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.exited
}

// ExitCode is the last run's exit code, or -1 if a signal ended it.
func (t *terminal) ExitCode() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.exitCode
}

func (t *terminal) Running() bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.running
}

// Close quits the app with ^C, as a person would, so it stops its own firmware app. If it
// hasn't gone after closePatience, Close kills its process group. Every reader is detached,
// and the terminal can't start again.
func (t *terminal) Close() error {
	t.mu.Lock()
	t.closed = true
	for r := range t.readers {
		delete(t.readers, r)
		close(r.live)
	}
	running, ptmx, cmd, exited := t.running, t.ptmx, t.cmd, t.exited
	t.mu.Unlock()
	if !running {
		return nil
	}

	ptmx.Write([]byte("\x03"))
	select {
	case <-exited:
		return nil
	case <-time.After(closePatience):
	}
	if err := syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL); err != nil && !errors.Is(err, syscall.ESRCH) {
		return err
	}
	select {
	case <-exited:
		return nil
	case <-time.After(closePatience):
		// Something outside the group still holds the pty open. Closing ours ends the
		// output loop.
		ptmx.Close()
		return errors.New("the terminal app's pty stayed open after it was killed")
	}
}
