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
}

func New(o Options) *Server {
	s := &Server{opts: o, mux: http.NewServeMux()}
	static, err := fs.Sub(staticFiles, "static")
	if err != nil {
		panic(err) // the directory is embedded at build time
	}
	s.handle("/", http.FileServerFS(static))
	if o.Socket != "" {
		s.dev = newDevice(o.Socket)
		s.handle("/device", http.HandlerFunc(s.serveDevice))
	}
	return s
}

// handle registers a route behind the guard.
func (s *Server) handle(pattern string, h http.Handler) {
	s.mux.Handle(pattern, guard(s.opts.AllowHosts, h))
}

// Serve answers on ln until ctx ends, and then returns nil.
func (s *Server) Serve(ctx context.Context, ln net.Listener) error {
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
	srv := &http.Server{Handler: s.mux, ReadHeaderTimeout: 10 * time.Second}
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
