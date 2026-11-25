package web

import (
	"fmt"
	"runtime"

	"github.com/kazzmir/opus-go/opus"
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
}

type kazzmirEncoder struct {
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
	enc, err := opus.NewEncoder(opusRate, channels, opus.ApplicationAudio)
	if err != nil {
		return nil, err
	}
	if err := enc.SetBitrate(bitrate); err != nil {
		enc.Close()
		return nil, err
	}
	e := &kazzmirEncoder{enc: enc, channels: channels, buf: make([]byte, opusMaxPacket)}
	// The encoder holds memory outside the Go heap; free it with the wrapper.
	runtime.AddCleanup(e, func(enc *opus.Encoder) { enc.Close() }, enc)
	return e, nil
}

func (e *kazzmirEncoder) Encode(pcm []int16) ([]byte, error) {
	if len(pcm) != opusFrame*e.channels {
		return nil, fmt.Errorf("opus: got %d samples, want %d (20 ms of %d channels)", len(pcm), opusFrame*e.channels, e.channels)
	}
	n, err := e.enc.Encode(pcm, opusFrame, e.buf)
	runtime.KeepAlive(e) // the cleanup must not close enc mid-call
	if err != nil {
		return nil, err
	}
	return append([]byte(nil), e.buf[:n]...), nil
}
