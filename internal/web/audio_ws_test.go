package web

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/coder/websocket"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// audioPage is a browser's side of /audio, reading everything in order.
type audioPage struct {
	t   *testing.T
	got chan wsMessage
}

type wsMessage struct {
	kind websocket.MessageType
	data []byte
}

func (s *testSite) openAudio(t *testing.T) *audioPage {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, "ws://"+s.addr+"/audio", &websocket.DialOptions{
		HTTPHeader: http.Header{"Origin": {"http://" + s.addr}},
	})
	require.NoError(t, err)
	t.Cleanup(func() { conn.CloseNow() })
	p := &audioPage{t: t, got: make(chan wsMessage, 1024)}
	go func() {
		for {
			kind, data, err := conn.Read(context.Background())
			if err != nil {
				return
			}
			p.got <- wsMessage{kind, data}
		}
	}()
	return p
}

func (p *audioPage) next() wsMessage {
	p.t.Helper()
	select {
	case m := <-p.got:
		return m
	case <-time.After(3 * time.Second):
		require.FailNow(p.t, "nothing arrived")
		return wsMessage{}
	}
}

// text waits for a text message that isn't a ping.
func (p *audioPage) text() map[string]any {
	p.t.Helper()
	for {
		m := p.next()
		require.Equal(p.t, websocket.MessageText, m.kind, "wanted text")
		var v map[string]any
		require.NoError(p.t, json.Unmarshal(m.data, &v))
		if v["type"] != "ping" {
			return v
		}
	}
}

func (p *audioPage) packet() (seq uint32, flags byte, opus []byte) {
	p.t.Helper()
	m := p.next()
	require.Equal(p.t, websocket.MessageBinary, m.kind, "wanted a packet, got %s", m.data)
	require.Greater(p.t, len(m.data), 5)
	return binary.BigEndian.Uint32(m.data), m.data[4], m.data[5:]
}

func TestAudioSocketSendsFormatThenHeaderedPackets(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 172, 640)
	p := startSite(t, socket).openAudio(t)
	waitAudioSubscribed(t, app)

	app.pushAudioFormat(48000, 2)
	app.pushAudio(beep(48000, 2, 60))
	app.pushAudioStop()

	assert.Equal(t, map[string]any{"type": "format", "rate": 48000.0, "channels": 2.0}, p.text())
	for i := range 3 {
		seq, flags, opus := p.packet()
		assert.Equal(t, uint32(i), seq)
		assert.Equal(t, i == 0, flags&1 == 1, "the burst bit")
		assert.Zero(t, flags&^1, "no other flags")
		assert.NotEmpty(t, opus)
	}
	assert.Equal(t, map[string]any{"type": "stop"}, p.text())
}

func TestAudioSocketIsGuarded(t *testing.T) {
	site := startSite(t, shortPath(t))
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_, resp, err := websocket.Dial(ctx, "ws://"+site.addr+"/audio", &websocket.DialOptions{
		HTTPHeader: http.Header{"Origin": {"https://evil.example"}},
	})
	require.Error(t, err)
	require.NotNil(t, resp)
	assert.Equal(t, http.StatusForbidden, resp.StatusCode)
}

func TestAudioSocketHasAHeartbeat(t *testing.T) {
	quickPings(t)
	p := startSite(t, shortPath(t)).openAudio(t)
	m := p.next()
	assert.Equal(t, websocket.MessageText, m.kind)
	assert.JSONEq(t, `{"type":"ping"}`, string(m.data))
}

func TestAudioSocketThatStopsAnsweringIsLetGo(t *testing.T) {
	quickPings(t)
	socket := shortPath(t)
	app := startFakeApp(t, socket, 172, 640)
	site := startSite(t, socket)
	dialSilent(t, site.addr, "/audio")
	waitAudioSubscribed(t, app)
	require.Eventually(t, func() bool { return app.audioListeners() == 0 }, 3*time.Second, 5*time.Millisecond,
		"a page that went still kept the subscription")
}
