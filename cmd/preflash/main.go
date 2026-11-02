// Command preflash runs ESP-IDF firmware on a Mac, against simulated boards.
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/edcs/preflash/internal/client"
	"github.com/edcs/preflash/internal/crash"
	"github.com/edcs/preflash/internal/project"
)

const usage = `usage: preflash [command] [--project dir]

  tui [--board file] [--headless] [--sanitize address|thread]
                                    open the terminal app, the default in a terminal
  run [--board file] [--headless] [--sanitize address|thread]
                                    build the project and run it
  send <line...>                    send a line to the firmware's console
  status [--json]                   show how the running app stands
  serial                            print the serial stream and follow it
  port                              print the virtual serial port's path
  shot <file.png|file.bmp>          save what the panel shows
  touch x y [ms]                    press the panel
  trace [--stream x] [--json]       print what firmware does to the hardware and follow it
  trace --save file                 save what the app has kept of that
  hardware [name value] [--json]    show or change the board's hardware and forced failures
  crash [--json]                    print the last crash report
  quit                              stop the running app
`

func main() {
	commands := map[string]func([]string) error{
		"tui":    terminalApp,
		"run":    run,
		"send":   send,
		"status": status,
		"serial": serial,
		"port":   port,
		"shot":   shot,
		"touch":  touch,
		"quit":   quit,

		"trace":    trace,
		"hardware": hardware,
		"crash":    crashReport,
	}
	command, args := chooseCommand(os.Args[1:], interactive())
	if commands[command] == nil {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	if err := commands[command](args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			fmt.Fprint(os.Stdout, usage)
			return
		}
		fmt.Fprintf(os.Stderr, "preflash: %v\n", err)
		os.Exit(1)
	}
}

// chooseCommand splits the arguments into a command and its own. No command means the
// terminal app, where there's a terminal for it, and that goes for a flag first too.
func chooseCommand(args []string, inTerminal bool) (string, []string) {
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		return args[0], args[1:]
	}
	if len(args) == 0 && !inTerminal {
		return "", nil
	}
	return "tui", args
}

func run(args []string) error {
	flags, dir := newFlags("run")
	boardName := flags.String("board", "", "board file, or a stock board's name")
	headless := flags.Bool("headless", false, "run with no window")
	sanitize := flags.String("sanitize", "", "build with a sanitizer: address or thread")
	if err := noArguments(flags, args); err != nil {
		return err
	}
	if err := project.CheckSanitizer(*sanitize); err != nil {
		return err
	}

	p, err := project.Find(*dir)
	if err != nil {
		return err
	}
	b, err := p.Board(*boardName)
	if err != nil {
		return err
	}
	return p.Sanitized(*sanitize).Run(b, *headless)
}

// crashReport needs no running app: the report outlives the process.
func crashReport(args []string) error {
	flags, dir := newFlags("crash")
	asJSON := flags.Bool("json", false, "print the report as the app wrote it")
	if err := noArguments(flags, args); err != nil {
		return err
	}
	p, err := project.Find(*dir)
	if err != nil {
		return err
	}
	report, err := p.LastCrash()
	if err != nil {
		return err
	}
	report.Resolve(p.Dir, crash.RunCommand)
	if *asJSON {
		return json.NewEncoder(os.Stdout).Encode(report)
	}
	fmt.Print(report.Text())
	return nil
}

// A line can hold words that look like flags, so flags come before it.
func send(args []string) error {
	flags, dir := newFlags("send")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() == 0 {
		return errors.New("send needs a line")
	}
	return withApp(*dir, func(c *client.Client) error {
		return c.Send(strings.Join(flags.Args(), " "))
	})
}

func status(args []string) error {
	flags, dir := newFlags("status")
	asJSON := flags.Bool("json", false, "print JSON")
	if err := noArguments(flags, args); err != nil {
		return err
	}
	return withApp(*dir, func(c *client.Client) error {
		s, err := c.Status()
		if err != nil {
			return err
		}
		if *asJSON {
			return json.NewEncoder(os.Stdout).Encode(s)
		}
		uptime := time.Duration(s.UptimeMs) * time.Millisecond
		fmt.Printf("project    %s\n", s.Project)
		fmt.Printf("board      %s (%dx%d)\n", s.Board, s.Width, s.Height)
		fmt.Printf("pid        %d\n", s.PID)
		fmt.Printf("uptime     %s\n", uptime.Round(time.Second/10))
		fmt.Printf("backlight  %d%%\n", s.Backlight)
		fmt.Printf("rotation   %d°\n", s.Rotation)
		fmt.Printf("frames     %d (%d fps)\n", s.Frames, s.FPS)
		fmt.Printf("wifi       %s\n", s.WiFi)
		if s.SerialPort != "" {
			fmt.Printf("port       %s\n", s.SerialPort)
		}
		if s.Crashed {
			fmt.Println("crashed    yes: preflash crash shows the report")
		}
		return nil
	})
}

func serial(args []string) error {
	flags, dir := newFlags("serial")
	if err := noArguments(flags, args); err != nil {
		return err
	}
	socket, err := socketPath(*dir)
	if err != nil {
		return err
	}
	err = client.Follow(socket, os.Stdout, func(warning string) {
		fmt.Fprintf(os.Stderr, "preflash: %s\n", warning)
	})
	return describe(err, *dir)
}

func port(args []string) error {
	flags, dir := newFlags("port")
	if err := noArguments(flags, args); err != nil {
		return err
	}
	return withApp(*dir, func(c *client.Client) error {
		s, err := c.Status()
		if err != nil {
			return err
		}
		if s.SerialPort == "" {
			return errors.New("this app has no serial port")
		}
		fmt.Println(s.SerialPort)
		return nil
	})
}

func shot(args []string) error {
	flags, dir := newFlags("shot")
	words, err := parse(flags, args)
	if err != nil {
		return err
	}
	if len(words) != 1 {
		return errors.New("shot needs one file, ending in .png or .bmp")
	}
	return withApp(*dir, func(c *client.Client) error {
		frame, err := c.Frame()
		if err != nil {
			return err
		}
		return frame.Save(words[0])
	})
}

func touch(args []string) error {
	flags, dir := newFlags("touch")
	words, err := parse(flags, args)
	if err != nil {
		return err
	}
	if len(words) != 2 && len(words) != 3 {
		return errors.New("touch needs x y [ms]")
	}
	numbers := make([]int, len(words))
	for i, word := range words {
		if numbers[i], err = strconv.Atoi(word); err != nil {
			return fmt.Errorf("touch needs numbers, not %q", word)
		}
	}
	return withApp(*dir, func(c *client.Client) error {
		if len(numbers) == 3 {
			return c.Touch(numbers[0], numbers[1], numbers[2])
		}
		return c.TouchDefault(numbers[0], numbers[1])
	})
}

func quit(args []string) error {
	flags, dir := newFlags("quit")
	if err := noArguments(flags, args); err != nil {
		return err
	}
	return withApp(*dir, (*client.Client).Quit)
}

func newFlags(name string) (*flag.FlagSet, *string) {
	flags := flag.NewFlagSet(name, flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	return flags, flags.String("project", ".", "project folder")
}

// parse takes flags from anywhere among the words and gives back the words.
func parse(flags *flag.FlagSet, args []string) ([]string, error) {
	var words []string
	for {
		if err := flags.Parse(args); err != nil {
			return nil, err
		}
		if flags.NArg() == 0 {
			return words, nil
		}
		words = append(words, flags.Arg(0))
		args = flags.Args()[1:]
	}
}

func noArguments(flags *flag.FlagSet, args []string) error {
	words, err := parse(flags, args)
	if err != nil {
		return err
	}
	if len(words) > 0 {
		return fmt.Errorf("unexpected argument %q", words[0])
	}
	return nil
}

func socketPath(dir string) (string, error) {
	p, err := project.Find(dir)
	if err != nil {
		return "", err
	}
	return p.SocketPath(), nil
}

// withApp connects to the app running from the project in dir.
func withApp(dir string, work func(*client.Client) error) error {
	socket, err := socketPath(dir)
	if err != nil {
		return err
	}
	c, err := client.Dial(socket)
	if err != nil {
		return describe(err, dir)
	}
	defer c.Close()
	return describe(work(c), dir)
}

func describe(err error, dir string) error {
	if errors.Is(err, client.ErrNotRunning) {
		return fmt.Errorf("no app is running for %s: start it with preflash run", dir)
	}
	return err
}
