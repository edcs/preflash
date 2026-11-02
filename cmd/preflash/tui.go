package main

import (
	"errors"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"

	tea "charm.land/bubbletea/v2"
	"golang.org/x/term"

	"github.com/edcs/preflash/internal/app"
	"github.com/edcs/preflash/internal/crash"
	"github.com/edcs/preflash/internal/project"
	"github.com/edcs/preflash/internal/rc"
	"github.com/edcs/preflash/internal/tui"
)

func interactive() bool {
	return term.IsTerminal(int(os.Stdin.Fd())) && term.IsTerminal(int(os.Stdout.Fd()))
}

func terminalApp(args []string) error {
	flags, dir := newFlags("tui")
	boardName := flags.String("board", "", "board file, or a stock board's name")
	headless := flags.Bool("headless", false, "run with no window")
	sanitize := flags.String("sanitize", "", "build with a sanitizer: address or thread")
	if err := noArguments(flags, args); err != nil {
		return err
	}
	if err := project.CheckSanitizer(*sanitize); err != nil {
		return err
	}
	if !interactive() {
		return errors.New("the terminal app needs a terminal: use preflash run")
	}

	p, err := project.Find(*dir)
	if err != nil {
		return err
	}
	options, err := appOptions(p, *boardName)
	if err != nil {
		return err
	}

	options.Headless = *headless
	options.Sanitize = *sanitize
	supervisor := app.New(target{p}, *headless)
	defer supervisor.Close()
	supervisor.SetSanitizer(*sanitize)
	program := tea.NewProgram(tui.New(supervisor, options))

	// Bubble Tea handles an interrupt and a terminate. A closing terminal needs the same,
	// because the app has a group of its own and won't hear the hang-up.
	hangUp := make(chan os.Signal, 1)
	signal.Notify(hangUp, syscall.SIGHUP)
	defer signal.Stop(hangUp)
	go func() {
		<-hangUp
		program.Quit()
	}()

	_, err = program.Run()
	// Stopping the app can take seconds, and with Bubble Tea's handler gone a signal now
	// would end this process and leave the app running, with nothing to stop it.
	signal.Ignore(os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	if errors.Is(err, tea.ErrInterrupted) || errors.Is(err, tea.ErrProgramKilled) {
		return nil
	}
	return err
}

// target is a project as the supervisor sees it.
type target struct{ project.Project }

func (t target) Sanitized(kind string) app.Target { return target{t.Project.Sanitized(kind)} }

func (t target) ProjectDir() string { return t.Dir }

func appOptions(p project.Project, boardName string) (tui.Options, error) {
	config, hasConfig, err := rc.Load(p.Dir)
	if err != nil {
		return tui.Options{}, err
	}
	options := tui.Options{
		Name:      filepath.Base(p.Dir),
		Path:      shortPath(p.Dir),
		Config:    config,
		HasConfig: hasConfig,
		LVGL:      p.LVGLVersion(),
		Choices:   p.Boards(),
		Boards:    p.Boards,
		Tools:     tui.Tools{Debugger: crash.Debugger, Editor: crash.Editor, Copy: crash.Copy},
	}
	// A named board goes straight to running, so a bad one stops here, as it would for run.
	if boardName != "" || config.Board != "" {
		b, err := p.Board(boardName)
		if err != nil {
			return tui.Options{}, err
		}
		options.Board = &b
	}
	return options, nil
}

// shortPath writes a folder under the home folder with a tilde.
func shortPath(dir string) string {
	home, err := os.UserHomeDir()
	if err != nil {
		return dir
	}
	if rest, ok := strings.CutPrefix(dir, home+string(filepath.Separator)); ok {
		return filepath.Join("~", rest)
	}
	return dir
}
