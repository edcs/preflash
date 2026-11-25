package web

import (
	"context"
	"time"

	"github.com/coder/websocket"
)

// How often each page's sockets are checked, and how long a page has to answer. Behind an SSH
// forward, a page that has gone leaves its connection open, so only a check notices.
var (
	pingInterval = 15 * time.Second
	pingTimeout  = 10 * time.Second
)

// A ping the page's script can see, since a browser hides WebSocket pings from it. The page
// counts a socket that has said nothing for a while as dead, and opens it again.
var pagePing = []byte(`{"type":"ping"}`)

// keepAlive checks every interval that the page is still there, and calls gone when it isn't
// or when it can't be told so. It returns when ctx ends. The connection's reader must be running,
// as a pong is only seen by a read.
func keepAlive(ctx context.Context, conn *websocket.Conn, interval, timeout time.Duration, gone context.CancelFunc) {
	tick := time.NewTicker(interval)
	defer tick.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}
		if !answers(ctx, conn, timeout) {
			gone()
			return
		}
	}
}

// answers sends the page its ping and a WebSocket ping, and says whether it answered.
func answers(ctx context.Context, conn *websocket.Conn, timeout time.Duration) bool {
	writeCtx, cancel := context.WithTimeout(ctx, writeTimeout)
	err := conn.Write(writeCtx, websocket.MessageText, pagePing)
	cancel()
	if err != nil {
		return false
	}
	pingCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	return conn.Ping(pingCtx) == nil
}
