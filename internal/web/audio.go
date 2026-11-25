package web

import (
	"context"
	"log"
	"slices"
	"sync"
	"time"

	"github.com/edcs/preflash/internal/client"
)

const (
	// listenerQueue is how many messages a tab may fall behind by, a second of packets,
	// before its oldest packets go.
	listenerQueue = 50
	// burstGap is how long the sound may stop for before what comes next is a new burst.
	burstGap = 60 * time.Millisecond
	// packetLead is how far ahead of real time a burst's packets may be sent. A long write
	// arrives all at once, and sending it all at once would flood the page's buffer.
	packetLead = 200 * time.Millisecond
	// packetTime is the sound in one packet.
	packetTime = 20 * time.Millisecond
	// sendQueue is how many queued messages stop the relay reading the next event, so the
	// socket drops the oldest sound for it instead. It gates reading only: one long write is
	// queued whole, however many packets it makes.
	sendQueue = 512
)

// audioFormat is what a page's decoder needs to know. Rate is always 48 kHz.
type audioFormat struct {
	Rate, Channels int
}

// audioMsg is one message for a tab: a format, a stop, or a packet with the tab's own sequence
// number and whether it starts a burst.
type audioMsg struct {
	format *audioFormat
	stop   bool
	seq    uint32
	burst  bool
	packet []byte
}

type listener struct {
	ch        chan audioMsg
	seq       uint32
	hasFormat bool // the tab has been sent a format since the last stop
	fresh     bool // the next packet is the first since a format, so starts a burst
}

// audio follows the running app's sound while any tab listens, encodes it once, and hands
// each packet to every listening tab. A tab too slow to read loses its oldest packets.
type audio struct {
	socket string
	wake   chan struct{} // the first listener has arrived

	mu         sync.Mutex
	listeners  map[*listener]struct{}
	format     *audioFormat       // the format the tabs were last sent, nil after a stop
	endSession context.CancelFunc // hangs up on the app; nil while not connected
}

func newAudio(socket string) *audio {
	return &audio{socket: socket, wake: make(chan struct{}, 1), listeners: map[*listener]struct{}{}}
}

// listen adds a tab. It gets the current format at once, if there is one, and packets after
// it numbered from 0. stop removes the tab and closes msgs.
func (a *audio) listen() (msgs <-chan audioMsg, stop func()) {
	l := &listener{ch: make(chan audioMsg, listenerQueue)}
	a.mu.Lock()
	if a.format != nil {
		a.sendFormat(l, a.format)
	}
	a.listeners[l] = struct{}{}
	if len(a.listeners) == 1 {
		select {
		case a.wake <- struct{}{}:
		default:
		}
	}
	a.mu.Unlock()
	var once sync.Once
	return l.ch, func() {
		once.Do(func() {
			a.mu.Lock()
			defer a.mu.Unlock()
			delete(a.listeners, l)
			close(l.ch)
			// Nobody is listening, so stop the app sending; the protocol has no unsubscribe.
			if len(a.listeners) == 0 && a.endSession != nil {
				a.endSession()
			}
		})
	}
}

// run follows the app while anyone listens, until ctx ends.
func (a *audio) run(ctx context.Context) {
	for ctx.Err() == nil {
		session, end := context.WithCancel(ctx)
		a.mu.Lock()
		listening := len(a.listeners) > 0
		if listening {
			a.endSession = end
		}
		a.mu.Unlock()
		if !listening {
			end()
			select {
			case <-ctx.Done():
			case <-a.wake:
			}
			continue
		}
		if c, err := client.Dial(a.socket); err == nil {
			a.follow(session, c)
		}
		ended := session.Err() != nil // by the last listener going, or by ctx
		end()
		a.mu.Lock()
		a.endSession = nil
		// The app has gone or been hung up on, so its codec is closed for whoever is left.
		if a.format != nil {
			a.publish(audioMsg{stop: true})
		}
		a.mu.Unlock()
		if !ended {
			select {
			case <-ctx.Done():
			case <-time.After(redialEvery):
			}
		}
	}
}

// arrival is an event and when it was read.
type arrival struct {
	event client.Event
	at    time.Time
}

// follow subscribes c to audio and relays what comes until the app goes or ctx ends.
func (a *audio) follow(ctx context.Context, c *client.Client) {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	// Closing the connection is what gets a blocked read to return.
	go func() {
		<-ctx.Done()
		c.Close()
	}()
	events := make(chan arrival, 16)
	readerDone := make(chan struct{})
	go func() {
		defer close(readerDone)
		defer close(events)
		if c.Subscribe("audio") != nil {
			return
		}
		for {
			e, err := c.Next()
			if err != nil {
				return
			}
			select {
			case events <- arrival{e, time.Now()}:
			case <-ctx.Done():
				return
			}
		}
	}()
	p := &pump{a: a}
	p.run(ctx, events)
	cancel()
	<-readerDone
}

// queued is a message waiting for its time to go.
type queued struct {
	due time.Time
	msg audioMsg
}

// pump turns one connection's events into paced messages. Only its goroutine touches it.
type pump struct {
	a *audio

	rs      *resampler  // nil while the codec is closed
	enc     opusEncoder // nil while the codec is closed
	rate    int         // the codec's rate, for timing what arrives
	inCh    int         // the codec's channels
	ch      int         // channels after resampling
	pending []int16     // resampled sound short of a whole packet

	inBurst    bool
	burstStart time.Time
	burstIn    int64     // frames received in this burst, at the codec's rate
	burstOut   int       // packets made in this burst
	lastAt     time.Time // when the latest samples arrived
	encodeErr  bool      // an encode error has been logged
	queue      []queued
}

// run handles events and sends queued messages on time, until events closes or ctx ends.
func (p *pump) run(ctx context.Context, events <-chan arrival) {
	defer p.closeCodec()
	timer := time.NewTimer(time.Hour)
	defer timer.Stop()
	for {
		p.sendDue(time.Now())
		var in <-chan arrival
		if len(p.queue) < sendQueue {
			in = events
		}
		var wake time.Time
		if len(p.queue) > 0 {
			wake = p.queue[0].due
		}
		if flush, ok := p.burstEnd(); ok && (wake.IsZero() || flush.Before(wake)) {
			wake = flush
		}
		var tick <-chan time.Time
		if !wake.IsZero() {
			timer.Reset(time.Until(wake))
			tick = timer.C
		}
		select {
		case <-ctx.Done():
			return
		case e, ok := <-in:
			if !ok {
				// The app has gone. Its unsent sound goes too, so a restarted app's sound
				// isn't held up behind it; run tells the tabs to stop.
				return
			}
			p.handle(e)
		case now := <-tick:
			if flush, ok := p.burstEnd(); ok && !now.Before(flush) {
				p.flush()
				p.inBurst = false
			}
		}
	}
}

// burstOver is when the current burst counts as over: a gap after both the latest samples
// arrived and the burst's sound would have finished playing. The second keeps one sound
// written in long writes, each arriving seconds after the last as the last runs out, in one
// burst. The first keeps an app that runs behind real time, as a headless one sleeping after
// each write does, from splitting its sound into bursts once it is a gap behind.
func (p *pump) burstOver() time.Time {
	played := p.burstStart.Add(time.Duration(p.burstIn) * time.Second / time.Duration(p.rate))
	if p.lastAt.After(played) {
		played = p.lastAt
	}
	return played.Add(burstGap)
}

// burstEnd is when the current burst counts as over, if a partial packet waits for it.
func (p *pump) burstEnd() (time.Time, bool) {
	if !p.inBurst || len(p.pending) == 0 {
		return time.Time{}, false
	}
	return p.burstOver(), true
}

func (p *pump) handle(e arrival) {
	switch e.event.Kind {
	case "audio_format":
		f := e.event.Audio
		p.flush()
		p.closeCodec()
		rs := newResampler(f.Rate, f.Channels)
		enc, err := newOpusEncoder(rs.Channels())
		if err != nil {
			// Tabs mustn't keep the old format for sound that can't come.
			log.Printf("preflash web: no sound: %v", err)
			p.enqueue(time.Time{}, audioMsg{stop: true})
			return
		}
		p.rs, p.enc, p.rate, p.inCh, p.ch = rs, enc, rs.from, rs.inCh, rs.Channels()
		p.inBurst = false
		p.enqueue(time.Time{}, audioMsg{format: &audioFormat{Rate: opusRate, Channels: p.ch}})
	case "audio":
		if p.enc == nil {
			return // sound with no format: nothing can make sense of it
		}
		samples := e.event.Audio.Samples
		if !p.inBurst || e.at.After(p.burstOver()) {
			// The sound stopped for longer than a gap, so the last burst's tail goes first.
			p.flush()
			p.inBurst, p.burstStart, p.burstIn, p.burstOut = true, e.at, 0, 0
		}
		p.lastAt = e.at
		p.burstIn += int64(len(samples) / p.inCh)
		p.pending = append(p.pending, p.rs.Push(samples)...)
		frame := opusFrame * p.ch
		for len(p.pending) >= frame {
			p.encode(p.pending[:frame])
			p.pending = p.pending[frame:]
		}
		// Keep the leftover in its own small slice rather than pinning the whole write.
		p.pending = append([]int16(nil), p.pending...)
	case "audio_stop":
		if p.enc == nil {
			return // a stop with no format before it
		}
		p.flush()
		p.closeCodec()
		p.enqueue(time.Time{}, audioMsg{stop: true})
	}
	// A dropped event for audio is a gap in the sound the socket already lost; the next
	// samples carry on, as a new burst if the gap was long enough.
}

// flush pads what's left of a packet with silence and sends it.
func (p *pump) flush() {
	if p.enc == nil || len(p.pending) == 0 {
		p.pending = nil
		return
	}
	frame := make([]int16, opusFrame*p.ch)
	copy(frame, p.pending)
	p.pending = nil
	p.encode(frame)
}

func (p *pump) encode(pcm []int16) {
	packet, err := p.enc.Encode(pcm)
	if err != nil {
		if !p.encodeErr {
			log.Printf("preflash web: sound lost: %v", err)
			p.encodeErr = true
		}
		return
	}
	// Each packet goes no sooner than its place in the burst less the lead; one that is
	// already late goes at once.
	due := p.burstStart.Add(time.Duration(p.burstOut)*packetTime - packetLead)
	p.enqueue(due, audioMsg{burst: p.burstOut == 0, packet: packet})
	p.burstOut++
}

func (p *pump) closeCodec() {
	if p.enc != nil {
		p.enc.Close()
	}
	p.rs, p.enc, p.pending, p.inBurst = nil, nil, nil, false
}

// enqueue adds a message to go at due, or at once once what's ahead of it has gone.
func (p *pump) enqueue(due time.Time, m audioMsg) {
	p.queue = append(p.queue, queued{due, m})
}

// sendDue sends, in order, every message whose time has come.
func (p *pump) sendDue(now time.Time) {
	n := 0
	for n < len(p.queue) && !p.queue[n].due.After(now) {
		n++
	}
	if n == 0 {
		return
	}
	p.a.mu.Lock()
	for _, q := range p.queue[:n] {
		p.a.publish(q.msg)
	}
	p.a.mu.Unlock()
	p.queue = append(p.queue[:0], p.queue[n:]...)
}

// publish hands m to every listener. a.mu must be held.
func (a *audio) publish(m audioMsg) {
	switch {
	case m.format != nil:
		a.format = m.format
		for l := range a.listeners {
			a.sendFormat(l, m.format)
		}
	case m.stop:
		a.format = nil
		for l := range a.listeners {
			l.hasFormat = false
			deliver(l.ch, audioMsg{stop: true})
		}
	default:
		for l := range a.listeners {
			if !l.hasFormat {
				continue
			}
			deliver(l.ch, audioMsg{seq: l.seq, burst: m.burst || l.fresh, packet: m.packet})
			l.seq++
			l.fresh = false
		}
	}
}

// sendFormat starts a new run of numbers for l. a.mu must be held.
func (a *audio) sendFormat(l *listener, f *audioFormat) {
	l.seq, l.hasFormat, l.fresh = 0, true, true
	deliver(l.ch, audioMsg{format: f})
}

// deliver puts m in ch. When ch is full its oldest packet goes to make room; a format or stop
// is never dropped. Only the publisher sends, under a.mu, so the room it makes stays free.
func deliver(ch chan audioMsg, m audioMsg) {
	select {
	case ch <- m:
		return
	default:
	}
	// Take everything out, leave out the oldest packet, and put the rest back in order. The
	// reader may take some meanwhile, which only leaves more room.
	held := make([]audioMsg, 0, cap(ch))
	for len(held) < cap(ch) {
		select {
		case old := <-ch:
			held = append(held, old)
			continue
		default:
		}
		break
	}
	oldest := slices.IndexFunc(held, func(o audioMsg) bool { return o.packet != nil })
	switch {
	case len(held) < cap(ch): // the reader made room meanwhile
	case oldest >= 0:
		held = slices.Delete(held, oldest, oldest+1)
	case m.packet != nil:
		// Nothing but formats and stops, which can't go, so the new packet does. Unreachable
		// in practice: a format or stop is at most one per codec open or close.
		m = audioMsg{}
	default:
		// Nothing but formats and stops, the oldest stale anyway. As unreachable in practice.
		held = held[1:]
	}
	for _, old := range held {
		ch <- old
	}
	if m.format != nil || m.stop || m.packet != nil {
		ch <- m
	}
}
