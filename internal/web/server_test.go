package web

import (
	"context"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestServeStopsWithTheContext(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- New(Options{}).Serve(ctx, ln) }()

	res, err := http.Get("http://" + ln.Addr().String() + "/")
	require.NoError(t, err)
	res.Body.Close()
	assert.Equal(t, 200, res.StatusCode)
	assert.True(t, strings.HasPrefix(res.Header.Get("Content-Type"), "text/html"))

	cancel()
	select {
	case err := <-done:
		assert.NoError(t, err)
	case <-time.After(time.Second):
		t.Fatal("Serve did not return after the context ended")
	}
}
