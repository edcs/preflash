// Package bench times a project through Preflash: how long it builds, how fast it boots and
// draws, and how quickly the socket and the serial stream answer. It works on a copy, so the
// project's own build and saved state stay as they are.
package bench

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"text/tabwriter"
	"time"

	"github.com/edcs/preflash/internal/board"
	"github.com/edcs/preflash/internal/client"
	"github.com/edcs/preflash/internal/project"
)

const (
	frameWindow     = 3 * time.Second
	roundTrips      = 2000
	serialBytes     = 8 << 20
	bootPatience    = 30 * time.Second
	serialPatience  = 60 * time.Second
	serialCommand   = "spew %d"
	serialDoneLine  = "spew done\n"
	firstFramePause = 5 * time.Millisecond
)

// Result is one run's measures.
type Result struct {
	CleanBuild       time.Duration
	Rebuild          time.Duration
	BootToFirstFrame time.Duration
	FramesPerSecond  float64
	SocketRoundTrip  time.Duration
	SerialMBPerSec   float64
}

// WriteJSON writes the measures with their units in their names.
func (r Result) WriteJSON(out io.Writer) error {
	return json.NewEncoder(out).Encode(map[string]float64{
		"clean_build_s":          r.CleanBuild.Seconds(),
		"rebuild_s":              r.Rebuild.Seconds(),
		"boot_to_first_frame_ms": float64(r.BootToFirstFrame) / float64(time.Millisecond),
		"frames_per_second":      r.FramesPerSecond,
		"socket_round_trip_us":   float64(r.SocketRoundTrip) / float64(time.Microsecond),
		"serial_mb_per_second":   r.SerialMBPerSec,
	})
}

// WriteTable writes the measures for a person to read.
func (r Result) WriteTable(out io.Writer) {
	table := tabwriter.NewWriter(out, 0, 0, 2, ' ', 0)
	fmt.Fprintf(table, "Clean build\t%.1f s\n", r.CleanBuild.Seconds())
	fmt.Fprintf(table, "Rebuild\t%.1f s\n", r.Rebuild.Seconds())
	fmt.Fprintf(table, "Boot to first frame\t%.0f ms\n", float64(r.BootToFirstFrame)/float64(time.Millisecond))
	fmt.Fprintf(table, "Frame rate\t%.1f fps\n", r.FramesPerSecond)
	fmt.Fprintf(table, "Socket round trip\t%.0f µs\n", float64(r.SocketRoundTrip)/float64(time.Microsecond))
	fmt.Fprintf(table, "Serial throughput\t%.1f MB/s\n", r.SerialMBPerSec)
	table.Flush()
}

// Run measures a copy of the project on the board. The project's firmware must answer a
// console line of "spew <bytes>" by printing that many bytes and then "spew done", as the
// demo does, and must keep something moving on the panel.
func Run(ctx context.Context, p project.Project, b board.Board) (Result, error) {
	var result Result
	parent, err := os.MkdirTemp("", "preflash-bench-")
	if err != nil {
		return result, err
	}
	defer os.RemoveAll(parent)
	dir := filepath.Join(parent, filepath.Base(p.Dir))
	if out, err := exec.CommandContext(ctx, "cp", "-R", p.Dir, dir).CombinedOutput(); err != nil {
		return result, fmt.Errorf("copying the project: %w: %s", err, out)
	}
	copied, err := project.Find(dir)
	if err != nil {
		return result, err
	}
	defer os.RemoveAll(copied.BuildDir())
	defer os.RemoveAll(copied.StateDir())

	env, err := copied.Environment(b, true)
	if err != nil {
		return result, err
	}
	var buildOutput bytes.Buffer
	build := func() (time.Duration, error) {
		buildOutput.Reset()
		started := time.Now()
		if err := copied.Build(ctx, env, &buildOutput); err != nil {
			return 0, fmt.Errorf("%w\n%s", err, buildOutput.String())
		}
		return time.Since(started), nil
	}
	if result.CleanBuild, err = build(); err != nil {
		return result, err
	}
	if err := touchFirstSource(copied.Dir); err != nil {
		return result, err
	}
	if result.Rebuild, err = build(); err != nil {
		return result, err
	}
	return result, measureApp(ctx, copied, b, env, &result)
}

// touchFirstSource marks one file in main/ as changed, which is a typical edit's rebuild.
func touchFirstSource(dir string) error {
	var sources []string
	err := filepath.WalkDir(filepath.Join(dir, "main"), func(path string, entry fs.DirEntry, err error) error {
		if err == nil && !entry.IsDir() && slices.Contains([]string{".c", ".cpp"}, filepath.Ext(path)) {
			sources = append(sources, path)
		}
		return err
	})
	if err != nil {
		return err
	}
	if len(sources) == 0 {
		return errors.New("no source files in main/ to change")
	}
	now := time.Now()
	return os.Chtimes(sources[0], now, now)
}

func measureApp(ctx context.Context, p project.Project, b board.Board, env []string, result *Result) error {
	path, runEnv, err := p.Install(b, env)
	if err != nil {
		return err
	}
	app := exec.CommandContext(ctx, path)
	app.Env = runEnv
	started := time.Now()
	if err := app.Start(); err != nil {
		return err
	}
	defer func() {
		app.Process.Kill()
		app.Wait()
	}()

	c, err := client.Await(p.SocketPath(), bootPatience, app.Process.Pid)
	if err != nil {
		return err
	}
	defer c.Close()
	first, err := firstFrame(c)
	if err != nil {
		return err
	}
	result.BootToFirstFrame = first.Sub(started)
	if result.FramesPerSecond, err = frameRate(c); err != nil {
		return err
	}
	if result.SocketRoundTrip, err = roundTrip(c); err != nil {
		return err
	}
	if result.SerialMBPerSec, err = serialRate(p.SocketPath()); err != nil {
		return err
	}
	return c.Quit()
}

func firstFrame(c *client.Client) (time.Time, error) {
	deadline := time.Now().Add(bootPatience)
	for time.Now().Before(deadline) {
		status, err := c.Status()
		if err != nil {
			return time.Time{}, err
		}
		if status.Frames > 0 {
			return time.Now(), nil
		}
		time.Sleep(firstFramePause)
	}
	return time.Time{}, errors.New("the panel never drew a frame")
}

func frameRate(c *client.Client) (float64, error) {
	before, err := c.Status()
	if err != nil {
		return 0, err
	}
	started := time.Now()
	time.Sleep(frameWindow)
	after, err := c.Status()
	if err != nil {
		return 0, err
	}
	return float64(after.Frames-before.Frames) / time.Since(started).Seconds(), nil
}

func roundTrip(c *client.Client) (time.Duration, error) {
	started := time.Now()
	for range roundTrips {
		if _, err := c.Status(); err != nil {
			return 0, err
		}
	}
	return time.Since(started) / roundTrips, nil
}

// serialCounter counts what a stream delivers until the firmware's closing line.
type serialCounter struct {
	bytes int
	tail  []byte
	done  bool
}

func (s *serialCounter) add(data []byte) {
	s.bytes += len(data)
	s.tail = append(s.tail, data...)
	s.done = s.done || bytes.Contains(s.tail, []byte(serialDoneLine))
	if keep := len(serialDoneLine) - 1; len(s.tail) > keep {
		s.tail = slices.Clone(s.tail[len(s.tail)-keep:])
	}
}

// serialRate reads on a connection of its own, which sees only bytes after what it replays.
func serialRate(socket string) (float64, error) {
	c, err := client.Dial(socket)
	if err != nil {
		return 0, err
	}
	defer c.Close()
	if err := c.SetDeadline(time.Now().Add(serialPatience)); err != nil {
		return 0, err
	}
	if err := c.Subscribe("serial"); err != nil {
		return 0, err
	}
	if err := c.Send(fmt.Sprintf(serialCommand, serialBytes)); err != nil {
		return 0, err
	}
	var counter serialCounter
	started := time.Now()
	for !counter.done {
		event, err := c.Next()
		if err != nil {
			return 0, err
		}
		if event.Kind == "serial" {
			counter.add(event.Data)
		}
	}
	return float64(counter.bytes) / (1 << 20) / time.Since(started).Seconds(), nil
}
