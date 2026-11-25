// Package web serves Preflash's terminal app and the simulated device to a browser.
package web

import (
	"context"
	"embed"
	"errors"
	"io/fs"
	"net"
	"net/http"
	"time"
)

//go:embed static
var staticFiles embed.FS

// Options say what to serve.
type Options struct {
	Socket     string   // the running app's socket
	Terminal   []string // the terminal app's argv; empty starts none
	AllowHosts []string // names besides loopback that may reach the server
}

// Server is the HTTP side of preflash web.
type Server struct {
	opts Options
	mux  *http.ServeMux
	dev  *device // nil without a socket

	term *terminal // nil without a terminal app
	runs *runs

	// How often pages are checked and how long they have to answer, taken from pingInterval
	// and pingTimeout when the server is made.
	pingEvery, pingWait time.Duration
}

func New(o Options) *Server {
	s := &Server{opts: o, mux: http.NewServeMux(), pingEvery: pingInterval, pingWait: pingTimeout}
	static, err := fs.Sub(staticFiles, "static")
	if err != nil {
		panic(err) // the directory is embedded at build time
	}
	s.handle("/", http.FileServerFS(static))
	if o.Socket != "" {
		s.dev = newDevice(o.Socket)
		s.handle("/device", http.HandlerFunc(s.serveDevice))
	}
	if len(o.Terminal) > 0 {
		term, err := newTerminal(o.Terminal, startCols, startRows)
		if err != nil {
			panic(err) // the starting size is a constant
		}
		s.term, s.runs = term, newRuns(term)
	}
	// Without a terminal app the page still connects, and is told why it has nothing.
	s.handle("/terminal", http.HandlerFunc(s.serveTerminal))
	return s
}

// handle registers a route behind the guard.
func (s *Server) handle(pattern string, h http.Handler) {
	s.mux.Handle(pattern, guard(s.opts.AllowHosts, h))
}

// Serve answers on ln until ctx ends, and then returns nil.
func (s *Server) Serve(ctx context.Context, ln net.Listener) error {
	if s.term != nil {
		// Deferred first, so it runs after the device follower has stopped.
		defer s.stopTerminal()
		if err := s.runs.start(); err != nil {
			return err
		}
	}
	if s.dev != nil {
		devCtx, stopDevice := context.WithCancel(ctx)
		deviceDone := make(chan struct{})
		go func() {
			defer close(deviceDone)
			s.dev.run(devCtx)
		}()
		defer func() {
			stopDevice()
			<-deviceDone
		}()
	}
	srv := &http.Server{
		Handler:           s.mux,
		ReadHeaderTimeout: 10 * time.Second,
		// Shutdown leaves the hijacked sockets alone, so their handlers end with ctx instead.
		BaseContext: func(net.Listener) context.Context { return ctx },
	}
	done := make(chan struct{})
	stopped := make(chan struct{}) // closed when Serve fails, so the goroutine doesn't wait on ctx
	go func() {
		defer close(done)
		select {
		case <-ctx.Done():
		case <-stopped:
			return
		}
		// Sockets are hijacked and left alone by Shutdown, so don't wait on them.
		shutdown, cancel := context.WithTimeout(context.Background(), time.Second)
		defer cancel()
		if srv.Shutdown(shutdown) != nil {
			srv.Close()
		}
	}()
	err := srv.Serve(ln)
	if !errors.Is(err, http.ErrServerClosed) {
		close(stopped)
	}
	if errors.Is(err, http.ErrServerClosed) {
		<-done
		return nil
	}
	return err
}

// stopTerminal quits the terminal app, and the firmware app it may have left behind.
func (s *Server) stopTerminal() {
	s.term.Close()
	s.runs.shutdown()
	if s.opts.Socket != "" {
		quitFirmware(s.opts.Socket)
	}
}
