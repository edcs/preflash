// Command webprobe is the browser's stand-in for tests/integration/web.sh. It opens the
// sockets that preflash web serves, as a page would, and checks what comes back.
//
//	webprobe start ADDR
//	webprobe device ADDR PREFLASH PROJECT
//	webprobe audio ADDR PREFLASH PROJECT
//
// start opens /terminal as a page does on load, and checks that the app's output arrives.
// device opens /device, waits for a connected state and a PNG frame, taps the middle of the
// panel, holds BOOT, checks that preflash status sees it down, and lets go.
// audio opens /audio, sends the demo's beep to the console, and checks that a format message
// and Opus packets arrive, the first of them marking the start of a burst.
package main

import (
	"bytes"
	"context"
	"encoding/binary"
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
	case len(args) == 4 && args[0] == "audio":
		return audio(ctx, args[1], args[2], args[3])
	}
	return errors.New("usage: webprobe start ADDR | webprobe device ADDR PREFLASH PROJECT | webprobe audio ADDR PREFLASH PROJECT")
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

// start sends the page's start message, a no-op while the app runs, and waits for output.
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

// audio listens on /audio before the beep goes out, so that the relay is subscribed, then
// expects a format message and at least five packets with valid headers within 5 s.
func audio(ctx context.Context, addr, preflash, project string) error {
	conn, err := dial(ctx, addr, "/audio")
	if err != nil {
		return err
	}
	defer conn.CloseNow()

	// Give the server a moment to subscribe to the app's sound before the beep.
	time.Sleep(500 * time.Millisecond)
	if out, err := exec.CommandContext(ctx, preflash, "send", "--project", project, "beep").CombinedOutput(); err != nil {
		return fmt.Errorf("sending the beep: %w\n%s", err, out)
	}

	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	gotFormat, packets := false, 0
	var last uint32
	for packets < 5 {
		kind, data, err := conn.Read(ctx)
		if err != nil {
			return fmt.Errorf("waiting for sound (format %v, %d packets): %w", gotFormat, packets, err)
		}
		if kind == websocket.MessageText {
			var m struct {
				Type     string `json:"type"`
				Rate     int    `json:"rate"`
				Channels int    `json:"channels"`
			}
			if err := json.Unmarshal(data, &m); err != nil {
				return err
			}
			if m.Type == "format" {
				if m.Rate != 48000 || (m.Channels != 1 && m.Channels != 2) {
					return fmt.Errorf("an odd format: %s", data)
				}
				gotFormat = true
			}
			continue
		}
		if !gotFormat {
			return errors.New("a packet came before the format message")
		}
		if len(data) <= 5 || data[4]&^1 != 0 {
			return fmt.Errorf("a packet has a bad header: %d bytes, flags %#x", len(data), data[min(4, len(data)-1)])
		}
		seq := binary.BigEndian.Uint32(data)
		burst := data[4]&1 != 0
		if packets == 0 && !burst {
			return errors.New("the first packet doesn't mark the start of a burst")
		}
		if packets > 0 && !burst && seq != last+1 {
			return fmt.Errorf("sequence jumped from %d to %d inside a burst", last, seq)
		}
		last = seq
		packets++
	}
	return nil
}
