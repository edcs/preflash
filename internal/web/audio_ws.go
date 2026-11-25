package web

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"net/http"

	"github.com/coder/websocket"
)

// serveAudio is /audio: the device's sound, as a format message and then 20 ms Opus packets,
// each after a 5-byte header of a big-endian sequence number and a flags byte whose bit 0
// marks the first packet of a burst. The page sends nothing.
func (s *Server) serveAudio(w http.ResponseWriter, r *http.Request) {
	// As for /device, the guard has already decided about the Origin.
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{InsecureSkipVerify: true})
	if err != nil {
		return
	}
	defer conn.CloseNow()
	conn.SetReadLimit(4096)
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()

	readerDone := make(chan struct{})
	go func() {
		defer close(readerDone)
		defer cancel()
		// Reading is what sees pongs and a close; what the page says is ignored.
		for {
			if _, _, err := conn.Read(ctx); err != nil {
				return
			}
		}
	}()
	go keepAlive(ctx, conn, s.pingEvery, s.pingWait, cancel)

	msgs, stop := s.audio.listen()
	writeAudio(ctx, conn, msgs)
	stop()
	cancel()
	conn.CloseNow()
	<-readerDone
}

// writeAudio sends msgs to the page until it goes or ctx ends.
func writeAudio(ctx context.Context, conn *websocket.Conn, msgs <-chan audioMsg) {
	for {
		var m audioMsg
		select {
		case <-ctx.Done():
			return
		case next, ok := <-msgs:
			if !ok {
				return
			}
			m = next
		}
		kind, data := websocket.MessageBinary, []byte(nil)
		switch {
		case m.format != nil:
			kind = websocket.MessageText
			data, _ = json.Marshal(map[string]any{"type": "format", "rate": m.format.Rate, "channels": m.format.Channels})
		case m.stop:
			kind, data = websocket.MessageText, []byte(`{"type":"stop"}`)
		default:
			data = make([]byte, 5, 5+len(m.packet))
			binary.BigEndian.PutUint32(data, m.seq)
			if m.burst {
				data[4] = 1
			}
			data = append(data, m.packet...)
		}
		writeCtx, cancelWrite := context.WithTimeout(ctx, writeTimeout)
		err := conn.Write(writeCtx, kind, data)
		cancelWrite()
		if err != nil {
			return
		}
	}
}
