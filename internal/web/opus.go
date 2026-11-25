package web

import (
	"errors"
	"fmt"
	"sync"

	"github.com/tphakala/go-opus/opus"
)

const (
	opusRate  = 48000
	opusFrame = opusRate / 50 // samples per channel in 20 ms
	// opusMaxPacket is the most one 20 ms frame can take (RFC 6716, 3.2.1).
	opusMaxPacket = 1275
)

// opusEncoder turns 20 ms of 48 kHz sound into one Opus packet.
type opusEncoder interface {
	// Encode takes exactly one 20 ms frame, interleaved, and returns a packet
	// the caller may keep.
	Encode(pcm []int16) ([]byte, error)
	// Close frees the encoder. It is safe to call more than once, and Encode
	// fails after it.
	Close() error
}

var errOpusClosed = errors.New("opus: encoder closed")

// goOpusEncoder wraps tphakala/go-opus, a pure-Go port of libopus whose
// encoder codes CELT only, which suits music.
type goOpusEncoder struct {
	mu       sync.Mutex // go-opus encoders aren't safe for concurrent use
	enc      *opus.Encoder
	channels int
	buf      []byte
}

// newOpusEncoder returns an encoder for mono (64 kbps) or stereo (96 kbps)
// sound at 48 kHz.
func newOpusEncoder(channels int) (opusEncoder, error) {
	var bitrate int
	switch channels {
	case 1:
		bitrate = 64000
	case 2:
		bitrate = 96000
	default:
		return nil, fmt.Errorf("opus: %d channels, want 1 or 2", channels)
	}
	enc, err := opus.NewEncoder(opus.EncoderConfig{
		SampleRate: opusRate,
		Channels:   channels,
		Bitrate:    bitrate,
		// libopus's default, which go-opus leaves off unless asked.
		ConstrainedVBR: true,
	})
	if err != nil {
		return nil, err
	}
	return &goOpusEncoder{enc: enc, channels: channels, buf: make([]byte, opusMaxPacket)}, nil
}

func (e *goOpusEncoder) Encode(pcm []int16) ([]byte, error) {
	if len(pcm) != opusFrame*e.channels {
		return nil, fmt.Errorf("opus: got %d samples, want %d (20 ms of %d channels)", len(pcm), opusFrame*e.channels, e.channels)
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.enc == nil {
		return nil, errOpusClosed
	}
	n, err := e.enc.Encode(pcm, e.buf)
	if err != nil {
		return nil, err
	}
	return append([]byte(nil), e.buf[:n]...), nil
}

// Close drops the encoder's state. go-opus holds only Go memory, so there is
// nothing else to free.
func (e *goOpusEncoder) Close() error {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.enc = nil
	return nil
}
