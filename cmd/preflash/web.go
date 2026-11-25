package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"

	"github.com/edcs/preflash/internal/project"
	"github.com/edcs/preflash/internal/web"
)

// hosts collects a repeatable --allow-host.
type hosts []string

func (h *hosts) String() string     { return strings.Join(*h, ",") }
func (h *hosts) Set(v string) error { *h = append(*h, v); return nil }

// versionLine is what web prints first: a release is "v0.3.0", a dev build is just "dev".
func versionLine(v string) string {
	if v == "dev" {
		return "preflash dev"
	}
	return "preflash v" + v
}

func webCommand(args []string) error {
	ctx, stop := signal.NotifyContext(context.Background(), project.StopSignals()...)
	defer stop()
	go announceStop(ctx, stop, os.Stderr)
	return serveWeb(ctx, os.Stdout, args)
}

// announceStop says so when a signal starts the shutdown, and hands the signals back to their
// defaults, so a second ^C ends preflash web at once. That leaves nothing behind: the pty
// closes with the process, which hangs up the terminal app, and it stops its firmware.
func announceStop(ctx context.Context, stop context.CancelFunc, out io.Writer) {
	<-ctx.Done()
	// A signal leaves a cause of its own, which wraps the plain error; stop leaves none.
	if context.Cause(ctx) == ctx.Err() {
		return // web returned, and no signal came
	}
	stop()
	fmt.Fprintln(out, "preflash: stopping…")
}

func serveWeb(ctx context.Context, out io.Writer, args []string) error {
	flags, dir := newFlags("web")
	boardName := flags.String("board", "", "board file, or a stock board's name")
	sanitize := flags.String("sanitize", "", "build with a sanitizer: address or thread")
	port := flags.Int("port", 7480, "port to serve on")
	var allow hosts
	flags.Var(&allow, "allow-host", "a name besides localhost that may reach the page (repeatable)")
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

	// Listen before starting anything, so a taken port costs no build.
	ln, err := net.Listen("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(*port)))
	if errors.Is(err, syscall.EADDRINUSE) {
		return fmt.Errorf("port %d is in use: pick another with --port", *port)
	}
	if err != nil {
		return err
	}
	defer ln.Close()

	self, err := os.Executable()
	if err != nil {
		return err
	}
	terminal := []string{self, "tui", "--headless", "--project", p.Dir}
	if *boardName != "" {
		terminal = append(terminal, "--board", *boardName)
	}
	if *sanitize != "" {
		terminal = append(terminal, "--sanitize", *sanitize)
	}

	// The listener knows the port when --port was 0.
	got := ln.Addr().(*net.TCPAddr).Port
	fmt.Fprintf(out, "%s\n\nRunning on: http://localhost:%d\n", versionLine(version), got)
	return web.New(web.Options{
		Socket:     p.SocketPath(),
		Terminal:   terminal,
		AllowHosts: allow,
	}).Serve(ctx, ln)
}
