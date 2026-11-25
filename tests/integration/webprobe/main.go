// Command webprobe is the browser's stand-in for tests/integration/web.sh. It opens the
// sockets that preflash web serves, as a page would, and checks what comes back.
//
//	webprobe start ADDR
//	webprobe device ADDR PREFLASH PROJECT
//
// start asks the terminal app to start, as a page does on load. device opens /device, waits for
// a connected state and a PNG frame, taps the middle of the panel, holds BOOT, checks that
// preflash status sees it down, and lets go.
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"time"

	"github.com/coder/websocket"
)

var pngMagic = []byte("\x89PNG\r\n\x1a\n")

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "webprobe:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	switch {
	case len(args) == 2 && args[0] == "start":
		return start(ctx, args[1])
	case len(args) == 4 && args[0] == "device":
		return device(ctx, args[1], args[2], args[3])
	}
	return errors.New("usage: webprobe start ADDR | webprobe device ADDR PREFLASH PROJECT")
}

// dial connects with an Origin that matches the Host, which the server insists on.
func dial(ctx context.Context, addr, path string) (*websocket.Conn, error) {
	conn, _, err := websocket.Dial(ctx, "ws://"+addr+path, &websocket.DialOptions{
		HTTPHeader: http.Header{"Origin": {"http://" + addr}},
	})
	if err != nil {
		return nil, fmt.Errorf("dial %s: %w", path, err)
	}
	conn.SetReadLimit(16 << 20)
	return conn, nil
}

func send(ctx context.Context, conn *websocket.Conn, v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return conn.Write(ctx, websocket.MessageText, data)
}

// start sends the page's start message, and waits for the terminal app's first output.
func start(ctx context.Context, addr string) error {
	conn, err := dial(ctx, addr, "/terminal")
	if err != nil {
		return err
	}
	defer conn.CloseNow()
	if err := send(ctx, conn, map[string]any{"type": "resize", "cols": 120, "rows": 40}); err != nil {
		return err
	}
	if err := send(ctx, conn, map[string]any{"type": "start"}); err != nil {
		return err
	}
	for {
		kind, data, err := conn.Read(ctx)
		if err != nil {
			return fmt.Errorf("waiting for the terminal app's output: %w", err)
		}
		if kind == websocket.MessageText {
			var m struct{ Type, Text string }
			json.Unmarshal(data, &m)
			if m.Type == "error" {
				return errors.New("the server refused to start the app: " + m.Text)
			}
			continue
		}
		// Any printed output means the terminal app is running in the terminal.
		if bytes.IndexFunc(data, func(r rune) bool { return r > ' ' && r < 0x7f }) >= 0 {
			return nil
		}
	}
}

func device(ctx context.Context, addr, preflash, project string) error {
	conn, err := dial(ctx, addr, "/device")
	if err != nil {
		return err
	}
	defer conn.CloseNow()

	// A state with the app connected, and a frame after it.
	connected, framed := false, false
	wantPNG := false
	for !connected || !framed {
		kind, data, err := conn.Read(ctx)
		if err != nil {
			return fmt.Errorf("waiting for a connected state and a frame (connected %v, frame %v): %w", connected, framed, err)
		}
		if kind == websocket.MessageBinary {
			if !wantPNG {
				return errors.New("a binary message came with no frame header")
			}
			if !bytes.HasPrefix(data, pngMagic) {
				return errors.New("a frame is not a PNG")
			}
			wantPNG, framed = false, true
			if err := send(ctx, conn, map[string]string{"type": "ack"}); err != nil {
				return err
			}
			continue
		}
		var m struct {
			Type      string `json:"type"`
			Connected bool   `json:"connected"`
		}
		if err := json.Unmarshal(data, &m); err != nil {
			return err
		}
		switch m.Type {
		case "state":
			connected = connected || m.Connected
		case "frame":
			wantPNG = true
		case "error":
			return fmt.Errorf("the server sent an error: %s", data)
		}
	}

	// The input requests are answered only with an error, so an error from here on is a failure.
	// A reader drains the page's messages, acking frames, and reports any error.
	problems := make(chan error, 1)
	go func() {
		wantPNG := false
		for {
			kind, data, err := conn.Read(ctx)
			if err != nil {
				return
			}
			if kind == websocket.MessageBinary {
				if wantPNG {
					send(ctx, conn, map[string]string{"type": "ack"})
				}
				wantPNG = false
				continue
			}
			var m struct{ Type string }
			json.Unmarshal(data, &m)
			switch m.Type {
			case "frame":
				wantPNG = true
			case "error":
				select {
				case problems <- fmt.Errorf("the server sent an error: %s", data):
				default:
				}
			}
		}
	}()

	for _, down := range []bool{true, false} {
		if err := send(ctx, conn, map[string]any{"type": "pointer", "u": 0.5, "v": 0.5, "down": down}); err != nil {
			return err
		}
	}
	if err := send(ctx, conn, map[string]any{"type": "button", "name": "BOOT", "down": true}); err != nil {
		return err
	}
	if err := waitBoot(ctx, preflash, project, true, problems); err != nil {
		return err
	}
	if err := send(ctx, conn, map[string]any{"type": "button", "name": "BOOT", "down": false}); err != nil {
		return err
	}
	return waitBoot(ctx, preflash, project, false, problems)
}

// waitBoot polls preflash status until BOOT is down or up, as asked.
func waitBoot(ctx context.Context, preflash, project string, down bool, problems <-chan error) error {
	var last string
	for range 100 {
		select {
		case err := <-problems:
			return err
		default:
		}
		out, err := exec.CommandContext(ctx, preflash, "status", "--project", project, "--json").CombinedOutput()
		last = string(out)
		if err == nil {
			var s struct {
				Buttons []struct {
					Name string `json:"name"`
					Down bool   `json:"down"`
				} `json:"buttons"`
			}
			if json.Unmarshal(out, &s) == nil {
				for _, b := range s.Buttons {
					if b.Name == "BOOT" && b.Down == down {
						return nil
					}
				}
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
	return fmt.Errorf("BOOT never went down=%v in status:\n%s", down, last)
}
