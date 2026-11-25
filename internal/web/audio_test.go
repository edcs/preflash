package web

import (
	"context"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// startAudio runs a relay on socket until the test ends.
func startAudio(t *testing.T, socket string) *audio {
	t.Helper()
	a := newAudio(socket)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		defer close(done)
		a.run(ctx)
	}()
	t.Cleanup(func() {
		cancel()
		<-done
	})
	return a
}

type heard struct {
	audioMsg
	at time.Time
}

// earTab is a listener that reads everything, noting when each message came.
type earTab struct {
	t    *testing.T
	got  chan heard
	stop func()
}

func listenTab(t *testing.T, a *audio) *earTab {
	t.Helper()
	msgs, stop := a.listen()
	tb := &earTab{t: t, got: make(chan heard, 4096), stop: stop}
	t.Cleanup(stop)
	go func() {
		for m := range msgs {
			tb.got <- heard{m, time.Now()}
		}
	}()
	return tb
}

func (tb *earTab) next() heard {
	tb.t.Helper()
	select {
	case h := <-tb.got:
		return h
	case <-time.After(3 * time.Second):
		require.FailNow(tb.t, "nothing arrived")
		return heard{}
	}
}

func (tb *earTab) format() *audioFormat {
	tb.t.Helper()
	h := tb.next()
	require.NotNil(tb.t, h.format, "wanted a format, got %+v", h.audioMsg)
	return h.format
}

func (tb *earTab) packets(n int) []heard {
	tb.t.Helper()
	var out []heard
	for len(out) < n {
		h := tb.next()
		require.NotNil(tb.t, h.packet, "wanted packet %d, got %+v", len(out), h.audioMsg)
		out = append(out, h)
	}
	return out
}

// quiet says that nothing arrives for d.
func (tb *earTab) quiet(d time.Duration) {
	tb.t.Helper()
	select {
	case h := <-tb.got:
		require.FailNow(tb.t, "something arrived", "%+v", h.audioMsg)
	case <-time.After(d):
	}
}

func TestNoListenerMeansNoSubscription(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 8, 6)
	a := startAudio(t, socket)

	time.Sleep(3 * redialEvery / 2)
	assert.Zero(t, app.audioSubscribes(), "subscribed with nobody listening")

	_, stop := a.listen()
	waitAudioSubscribed(t, app)
	stop()
	require.Eventually(t, func() bool { return app.audioListeners() == 0 }, 3*time.Second, 5*time.Millisecond,
		"the subscription outlived the last listener")

	_, stop = a.listen()
	defer stop()
	require.Eventually(t, func() bool { return app.audioSubscribes() == 2 && app.audioListeners() == 1 },
		3*time.Second, 5*time.Millisecond, "a new listener didn't subscribe again")
}

func TestEveryListenerGetsTheSamePackets(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 8, 6)
	a := startAudio(t, socket)
	one, two := listenTab(t, a), listenTab(t, a)
	waitAudioSubscribed(t, app)

	app.pushAudioFormat(48000, 1)
	app.pushAudio(beep(48000, 1, 200))

	for _, tb := range []*earTab{one, two} {
		assert.Equal(t, &audioFormat{Rate: 48000, Channels: 1}, tb.format())
	}
	first, second := one.packets(10), two.packets(10)
	for i := range first {
		assert.Equal(t, uint32(i), first[i].seq)
		assert.Equal(t, uint32(i), second[i].seq)
		assert.Equal(t, i == 0, first[i].burst)
		assert.Equal(t, first[i].packet, second[i].packet)
	}
}

func TestSilenceSendsNothingAndAChimeStartsABurst(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 8, 6)
	a := startAudio(t, socket)
	tb := listenTab(t, a)
	waitAudioSubscribed(t, app)

	app.pushAudioFormat(48000, 1)
	tb.format()
	app.pushAudio(beep(48000, 1, 200))
	first := tb.packets(10)
	tb.quiet(1800 * time.Millisecond)
	// 110 ms is five whole frames and half of one, which is padded once the burst has ended.
	app.pushAudio(beep(48000, 1, 110))
	second := tb.packets(6)

	for i, h := range first {
		assert.Equal(t, uint32(i), h.seq)
		assert.Equal(t, i == 0, h.burst, "packet %d", i)
	}
	for i, h := range second {
		assert.Equal(t, uint32(10+i), h.seq, "numbers carry on across the gap")
		assert.Equal(t, i == 0, h.burst, "packet %d", 10+i)
	}
	tb.quiet(200 * time.Millisecond)
}

func TestAJoiningTabStartsWithAFormat(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 8, 6)
	a := startAudio(t, socket)
	early := listenTab(t, a)
	waitAudioSubscribed(t, app)

	app.pushAudioFormat(48000, 1)
	early.format()
	// Noise, so that no two packets are alike.
	app.pushAudio(noise(48000))
	// The first ten go at once; the rest are paced, so the stream is still going.
	before := early.packets(12)
	late := listenTab(t, a)

	assert.Equal(t, &audioFormat{Rate: 48000, Channels: 1}, late.format())
	joined := late.packets(5)
	after := early.packets(50 - len(before))
	all := append(before, after...)
	offset := -1
	for i, h := range all {
		if string(h.packet) == string(joined[0].packet) {
			offset = i
			break
		}
	}
	require.GreaterOrEqual(t, offset, len(before), "the late earTab got a packet sent before it joined")
	for i, h := range joined {
		assert.Equal(t, uint32(i), h.seq, "the late earTab counts from 0")
		assert.Equal(t, i == 0, h.burst, "the late earTab's first packet starts its burst")
		assert.Equal(t, all[offset+i].packet, h.packet)
		assert.Equal(t, uint32(offset+i), all[offset+i].seq, "the early earTab's numbers carry on")
	}
}

func TestARestartSendsANewFormat(t *testing.T) {
	socket := shortPath(t)
	old := startFakeApp(t, socket, 8, 6)
	a := startAudio(t, socket)
	tb := listenTab(t, a)
	waitAudioSubscribed(t, old)

	old.pushAudioFormat(48000, 1)
	tb.format()
	old.pushAudio(beep(48000, 1, 100))
	tb.packets(5)
	old.hangUp()
	assert.True(t, tb.next().stop, "the app going closes the stream")

	next := startFakeApp(t, socket, 8, 6)
	next.setCodec(16000, 2)
	waitAudioSubscribed(t, next)
	assert.Equal(t, &audioFormat{Rate: 48000, Channels: 2}, tb.format())
	next.pushAudio(beep(16000, 2, 200))
	got := tb.packets(3)
	for i, h := range got {
		assert.Equal(t, uint32(i), h.seq, "numbers start again")
		assert.Equal(t, i == 0, h.burst)
	}
}

func TestASlowTabDropsItsOldestPackets(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 8, 6)
	a := startAudio(t, socket)
	slow, stopSlow := a.listen()
	defer stopSlow()
	fast := listenTab(t, a)
	waitAudioSubscribed(t, app)

	app.pushAudioFormat(48000, 1)
	fast.format()
	app.pushAudio(beep(48000, 1, 2000))
	got := fast.packets(100)
	for i, h := range got {
		assert.Equal(t, uint32(i), h.seq, "the fast earTab missed nothing")
	}
	time.Sleep(50 * time.Millisecond)

	require.Len(t, slow, 50, "the slow earTab holds a second")
	first := <-slow
	require.NotNil(t, first.format, "the format is never dropped")
	for want := uint32(51); want < 100; want++ {
		m := <-slow
		require.NotNil(t, m.packet)
		assert.Equal(t, want, m.seq, "the slow earTab keeps the newest")
	}
}

func TestAudioPacesALongWrite(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 8, 6)
	a := startAudio(t, socket)
	tb := listenTab(t, a)
	waitAudioSubscribed(t, app)

	app.pushAudioFormat(48000, 1)
	tb.format()
	app.pushAudio(beep(48000, 1, 3000))
	got := tb.packets(150)

	// Ten packets are the lead, and go at once; the rest follow in real time.
	assert.Less(t, got[9].at.Sub(got[0].at), 300*time.Millisecond)
	span := got[149].at.Sub(got[0].at)
	assert.Greater(t, span, 2500*time.Millisecond, "the packets were sent faster than real time")
	assert.Less(t, span, 4*time.Second)
}

func TestAudioStopPadsTheLastFrameAndTells(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 8, 6)
	a := startAudio(t, socket)
	tb := listenTab(t, a)
	waitAudioSubscribed(t, app)

	// A stop with nothing open is left out.
	app.pushAudioStop()
	tb.quiet(100 * time.Millisecond)

	app.pushAudioFormat(48000, 2)
	assert.Equal(t, &audioFormat{Rate: 48000, Channels: 2}, tb.format())
	app.pushAudio(beep(48000, 2, 30))
	app.pushAudioStop()
	got := tb.packets(2)
	assert.Equal(t, uint32(1), got[1].seq)
	assert.True(t, tb.next().stop)
	tb.quiet(100 * time.Millisecond)
}

// noise is n samples of a fixed pseudo-random signal.
func noise(n int) []int16 {
	out := make([]int16, n)
	x := uint32(1)
	for i := range out {
		x = x*1664525 + 1013904223
		out[i] = int16(x>>16) / 4
	}
	return out
}

func TestAudioLongWritesInARowAreOneBurst(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 8, 6)
	a := startAudio(t, socket)
	tb := listenTab(t, a)
	waitAudioSubscribed(t, app)

	app.pushAudioFormat(48000, 1)
	tb.format()
	// Each write comes as the last one runs out, as firmware playing a long sound in
	// half-second writes does.
	app.pushAudio(beep(48000, 1, 500))
	time.Sleep(480 * time.Millisecond)
	app.pushAudio(beep(48000, 1, 500))
	for i, h := range tb.packets(50) {
		assert.Equal(t, i == 0, h.burst, "packet %d", i)
	}
}

func TestAudioWritesRunningBehindStayOneBurst(t *testing.T) {
	socket := shortPath(t)
	app := startFakeApp(t, socket, 8, 6)
	a := startAudio(t, socket)
	tb := listenTab(t, a)
	waitAudioSubscribed(t, app)

	app.pushAudioFormat(48000, 1)
	tb.format()
	// A headless app sleeps after each write, so it falls a little further behind real time
	// with every one: here 10 ms of sound every 11 ms, 150 ms behind by the end.
	const writes = 150
	start := time.Now()
	for i := range writes {
		time.Sleep(time.Until(start.Add(time.Duration(i) * 11 * time.Millisecond)))
		app.pushAudio(beep(48000, 1, 10))
	}
	// 1.5 s is exactly 75 packets, so none is padded.
	for i, h := range tb.packets(writes * 10 / 20) {
		assert.Equal(t, i == 0, h.burst, "packet %d", i)
	}
	tb.quiet(200 * time.Millisecond)
}

func TestAudioAppGoingDropsItsUnsentSound(t *testing.T) {
	socket := shortPath(t)
	old := startFakeApp(t, socket, 8, 6)
	a := startAudio(t, socket)
	tb := listenTab(t, a)
	waitAudioSubscribed(t, old)

	old.pushAudioFormat(48000, 1)
	tb.format()
	// Three seconds arrive at once and are paced out; the app goes a moment in.
	old.pushAudio(beep(48000, 1, 3000))
	tb.packets(12)
	gone := time.Now()
	old.hangUp()
	for {
		h := tb.next()
		if h.stop {
			break
		}
		require.NotNil(t, h.packet)
	}
	assert.Less(t, time.Since(gone), 500*time.Millisecond, "the stop waited for the old sound")

	next := startFakeApp(t, socket, 8, 6)
	next.setCodec(48000, 1)
	waitAudioSubscribed(t, next)
	tb.format()
	next.pushAudio(beep(48000, 1, 100))
	got := tb.packets(5)
	assert.Equal(t, uint32(0), got[0].seq)
}
