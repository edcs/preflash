package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"sync"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/edcs/preflash/internal/client"
	"github.com/edcs/preflash/internal/project"
)

// fakeProject makes an ESP-IDF project with an app answering on its socket, which says yes to
// everything and keeps what it was asked.
func fakeProject(t *testing.T) (string, func() []map[string]any) {
	t.Helper()
	// Short, so that the socket's path fits.
	home, err := os.MkdirTemp("/tmp", "pf")
	require.NoError(t, err)
	t.Cleanup(func() { os.RemoveAll(home) })
	t.Setenv("HOME", home)
	dir := filepath.Join(home, "p")
	require.NoError(t, os.MkdirAll(dir, 0o755))
	require.NoError(t, os.WriteFile(filepath.Join(dir, "CMakeLists.txt"),
		[]byte("include($ENV{IDF_PATH}/tools/cmake/project.cmake)\n"), 0o644))
	p, err := project.Find(dir)
	require.NoError(t, err)
	socket := p.SocketPath()
	require.NoError(t, os.MkdirAll(filepath.Dir(socket), 0o700))
	listener, err := net.Listen("unix", socket)
	require.NoError(t, err)
	t.Cleanup(func() { listener.Close() })

	var mu sync.Mutex
	var requests []map[string]any
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				scanner := bufio.NewScanner(conn)
				for scanner.Scan() {
					var req map[string]any
					if json.Unmarshal(scanner.Bytes(), &req) != nil {
						return
					}
					id := int(req["id"].(float64))
					if req["op"] == "hello" {
						fmt.Fprintf(conn, `{"id":%d,"ok":true,"protocol":1,"project":"p","board":"B","pid":7}`+"\n", id)
						continue
					}
					mu.Lock()
					requests = append(requests, req)
					mu.Unlock()
					fmt.Fprintf(conn, `{"id":%d,"ok":true}`+"\n", id)
				}
			}()
		}
	}()
	return dir, func() []map[string]any {
		mu.Lock()
		defer mu.Unlock()
		return append([]map[string]any(nil), requests...)
	}
}

func TestHardwareButtonSendsTheOp(t *testing.T) {
	dir, seen := fakeProject(t)
	require.NoError(t, hardware([]string{"button", "BOOT", "tap", "50", "--project", dir}))
	require.NoError(t, hardware([]string{"button", "PWR", "press", "--project", dir}))
	require.NoError(t, hardware([]string{"button", "PWR", "tap", "--project", dir}))
	got := seen()
	require.Len(t, got, 3)
	assert.Equal(t, map[string]any{"id": float64(2), "op": "button", "name": "BOOT", "action": "tap", "ms": float64(50)}, got[0])
	assert.Equal(t, map[string]any{"id": float64(2), "op": "button", "name": "PWR", "action": "press"}, got[1])
	assert.Equal(t, float64(100), got[2]["ms"], "tap holds for 100 ms unless told")
}

func TestHardwareButtonChecksItsWords(t *testing.T) {
	dir, seen := fakeProject(t)
	for _, words := range [][]string{
		{"button"},
		{"button", "BOOT"},
		{"button", "BOOT", "wiggle"},
		{"button", "BOOT", "press", "50"},
		{"button", "BOOT", "tap", "-1"},
		{"button", "BOOT", "tap", "x"},
		{"button", "BOOT", "tap", "5", "6"},
	} {
		err := hardware(append(words, "--project", dir))
		assert.Error(t, err, "%v", words)
	}
	assert.Empty(t, seen())
}

func TestStatusNamesTheButtonsAndWhichAreDown(t *testing.T) {
	line := buttonsLine([]client.ButtonState{{Name: "PWR"}, {Name: "BOOT", Down: true}, {Name: "RST"}})
	assert.Equal(t, "PWR, BOOT (down), RST", line)
}
