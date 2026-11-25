package main

import (
	"bytes"
	"context"
	"fmt"
	"net"
	"os"
	"os/signal"
	"regexp"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/edcs/preflash/internal/project"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestWebRefusesATakenPort(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	defer ln.Close()
	port := ln.Addr().(*net.TCPAddr).Port

	err = webCommand([]string{"--project", "../../examples/demo", "--port", fmt.Sprint(port)})
	require.Error(t, err)
	assert.Contains(t, err.Error(), fmt.Sprintf("port %d is in use", port))
	assert.Contains(t, err.Error(), "--port")
}

type lockedBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (l *lockedBuffer) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.Write(p)
}

func (l *lockedBuffer) String() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.String()
}

func TestWebPrintsThePortItGot(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	out := &lockedBuffer{}
	done := make(chan error, 1)
	go func() { done <- serveWeb(ctx, out, []string{"--project", "../../examples/demo", "--port", "0"}) }()

	var line string
	require.Eventually(t, func() bool {
		line = out.String()
		return strings.Contains(line, "Running on:")
	}, 2*time.Second, 10*time.Millisecond)
	cancel()
	require.NoError(t, <-done)

	m := regexp.MustCompile(`Running on: http://localhost:(\d+)\n`).FindStringSubmatch(line)
	require.NotNil(t, m, line)
	assert.NotEqual(t, "0", m[1])
}

func TestAHangUpSaysWebIsStopping(t *testing.T) {
	if signal.Ignored(syscall.SIGHUP) {
		t.Skip("hang-ups are ignored here, as under nohup")
	}
	ctx, stop := signal.NotifyContext(context.Background(), project.StopSignals()...)
	defer stop()
	require.NoError(t, syscall.Kill(os.Getpid(), syscall.SIGHUP))
	select {
	case <-ctx.Done():
	case <-time.After(2 * time.Second):
		t.Fatal("SIGHUP didn't stop web")
	}

	stopped := false
	var out bytes.Buffer
	announceStop(ctx, func() { stopped = true; stop() }, &out)
	assert.True(t, stopped, "a second signal would still be caught")
	assert.Equal(t, "preflash: stopping…\n", out.String())
}

func TestReturningSaysNothing(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	var out bytes.Buffer
	cancel()
	announceStop(ctx, func() {}, &out)
	assert.Empty(t, out.String())
}
