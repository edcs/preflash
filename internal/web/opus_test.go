package web

import (
	"fmt"
	"math"
	"testing"

	"github.com/stretchr/testify/require"
	"github.com/tphakala/go-opus/opus"
)

// decodeForTest returns a decoder for packets from newOpusEncoder, using the
// encoder library's own decoder.
func decodeForTest(channels int) (func([]byte) ([]int16, error), error) {
	d, err := opus.NewDecoder(opusRate, channels)
	if err != nil {
		return nil, err
	}
	pcm := make([]int16, opusFrame*channels)
	return func(packet []byte) ([]int16, error) {
		n, err := d.Decode(packet, pcm)
		if err != nil {
			return nil, err
		}
		return append([]int16(nil), pcm[:n*channels]...), nil
	}, nil
}

// roundTrip encodes in, a whole number of 20 ms frames, and decodes it back.
func roundTrip(t testing.TB, channels int, in []int16) []int16 {
	t.Helper()
	enc, err := newOpusEncoder(channels)
	require.NoError(t, err)
	defer enc.Close()
	dec, err := decodeForTest(channels)
	require.NoError(t, err)
	step := opusFrame * channels
	var out []int16
	for i := 0; i+step <= len(in); i += step {
		packet, err := enc.Encode(in[i : i+step])
		require.NoError(t, err)
		pcm, err := dec(packet)
		require.NoError(t, err)
		out = append(out, pcm...)
	}
	return out
}

func testTone(channels, frames int, freq, amp float64) []int16 {
	n := frames * opusFrame
	out := make([]int16, n*channels)
	for i := range n {
		v := int16(math.Round(amp * math.MaxInt16 * math.Sin(2*math.Pi*freq*float64(i)/opusRate)))
		for c := range channels {
			out[i*channels+c] = v
		}
	}
	return out
}

// testMusic synthesises a deterministic clip: four chords, half a second
// each, every note with five harmonics and a plucked envelope. In stereo the
// notes are panned differently in each channel.
func testMusic(channels, frames int) []int16 {
	chords := [][]float64{
		{261.63, 329.63, 392.00}, // C
		{220.00, 261.63, 329.63}, // Am
		{174.61, 220.00, 261.63}, // F
		{196.00, 246.94, 293.66}, // G
	}
	chordLen := opusRate / 2
	n := frames * opusFrame
	out := make([]int16, n*channels)
	for i := range n {
		notes := chords[(i/chordLen)%len(chords)]
		at := float64(i%chordLen) / opusRate
		env := math.Min(1, at/0.01) * math.Exp(-3*at)
		for c := range channels {
			var s float64
			for k, f := range notes {
				pan := 1.0
				if channels == 2 {
					pan = 0.6 + 0.4*float64((k+c)%2)
				}
				for h := 1; h <= 5; h++ {
					s += pan * math.Sin(2*math.Pi*f*float64(h)*float64(i)/opusRate+float64(h)) / float64(h*h)
				}
			}
			out[i*channels+c] = int16(math.Round(0.2 * math.MaxInt16 * env * s))
		}
	}
	return out
}

func channelOf(pcm []int16, channels, c int) []float64 {
	out := make([]float64, len(pcm)/channels)
	for i := range out {
		out[i] = float64(pcm[i*channels+c])
	}
	return out
}

// frequencyOf counts rising zero crossings, placed by linear interpolation.
func frequencyOf(x []float64) float64 {
	var first, last float64
	count := 0
	for i := 1; i < len(x); i++ {
		if x[i-1] < 0 && x[i] >= 0 {
			at := float64(i-1) - x[i-1]/(x[i]-x[i-1])
			if count == 0 {
				first = at
			}
			last = at
			count++
		}
	}
	if count < 2 {
		return 0
	}
	return float64(count-1) * opusRate / (last - first)
}

func levelDB(x []float64) float64 {
	var sum float64
	for _, v := range x {
		sum += v * v
	}
	return 10 * math.Log10(sum/float64(len(x)))
}

// delayOf finds the codec's delay, in samples, by cross-correlation.
func delayOf(ref, got []float64) int {
	best, bestDelay := math.Inf(-1), 0
	for d := range 2000 {
		var sum float64
		for i := 0; i < len(ref) && i+d < len(got); i++ {
			sum += ref[i] * got[i+d]
		}
		if sum > best {
			best, bestDelay = sum, d
		}
	}
	return bestDelay
}

// snrDB compares ref with got shifted back by delay, leaving out the first
// and last 20 ms of the overlap.
func snrDB(ref, got []float64, delay int) float64 {
	var signal, noise float64
	for i := opusFrame; i < len(ref)-opusFrame && i+delay < len(got)-opusFrame; i++ {
		e := ref[i] - got[i+delay]
		signal += ref[i] * ref[i]
		noise += e * e
	}
	return 10 * math.Log10(signal/noise)
}

func TestOpusRoundTripsATone(t *testing.T) {
	for _, channels := range []int{1, 2} {
		t.Run(fmt.Sprintf("%d channels", channels), func(t *testing.T) {
			in := testTone(channels, 50, 1000, 0.5)
			out := roundTrip(t, channels, in)
			require.Len(t, out, len(in))
			skip := opusRate / 4 // past the codec's start-up
			for c := range channels {
				ref := channelOf(in, channels, c)[skip:]
				got := channelOf(out, channels, c)[skip:]
				require.InEpsilon(t, 1000, frequencyOf(got), 0.01, "channel %d frequency", c)
				require.InDelta(t, levelDB(ref), levelDB(got), 1, "channel %d level", c)
			}
		})
	}
}

func TestOpusRoundTripsMusic(t *testing.T) {
	for _, channels := range []int{1, 2} {
		t.Run(fmt.Sprintf("%d channels", channels), func(t *testing.T) {
			in := testMusic(channels, 200) // 4 s
			out := roundTrip(t, channels, in)
			require.Len(t, out, len(in))
			for c := range channels {
				ref, got := channelOf(in, channels, c), channelOf(out, channels, c)
				snr := snrDB(ref, got, delayOf(ref, got))
				require.GreaterOrEqual(t, snr, 20.0, "channel %d SNR", c)
			}
		})
	}
}

func TestOpusPacketsMatchTheBitrate(t *testing.T) {
	// At 20 ms a frame, bytes per packet = bitrate / 8 / 50.
	for channels, want := range map[int]float64{1: 64000 / 8 / 50, 2: 96000 / 8 / 50} {
		t.Run(fmt.Sprintf("%d channels", channels), func(t *testing.T) {
			enc, err := newOpusEncoder(channels)
			require.NoError(t, err)
			defer enc.Close()
			in := testMusic(channels, 200) // 4 s
			step := opusFrame * channels
			total := 0
			for i := 0; i < len(in); i += step {
				packet, err := enc.Encode(in[i : i+step])
				require.NoError(t, err)
				total += len(packet)
			}
			mean := float64(total) / float64(len(in)/step)
			// The clip is fixed, so the mean lands within about 1%. 10% still
			// catches libopus's automatic mono rate, about 20% lower.
			require.InEpsilon(t, want, mean, 0.10, "mean packet size in bytes")
		})
	}
}

func TestOpusEncoderCloses(t *testing.T) {
	enc, err := newOpusEncoder(2)
	require.NoError(t, err)
	require.NoError(t, enc.Close())
	require.NoError(t, enc.Close())
	_, err = enc.Encode(make([]int16, opusFrame*2))
	require.Error(t, err)
}

func TestOpusEncoderRejectsBadInput(t *testing.T) {
	_, err := newOpusEncoder(3)
	require.Error(t, err)

	enc, err := newOpusEncoder(2)
	require.NoError(t, err)
	defer enc.Close()
	_, err = enc.Encode(make([]int16, opusFrame)) // a mono frame on a stereo encoder
	require.Error(t, err)
}

func BenchmarkOpusStereo(b *testing.B) {
	music := testMusic(2, 50)
	enc, err := newOpusEncoder(2)
	require.NoError(b, err)
	defer enc.Close()
	step := opusFrame * 2
	frames := len(music) / step
	for i := 0; b.Loop(); i++ {
		j := i % frames
		if _, err := enc.Encode(music[j*step : (j+1)*step]); err != nil {
			b.Fatal(err)
		}
	}
	// Share of one core needed to encode in real time: time per frame / 20 ms.
	b.ReportMetric(float64(b.Elapsed().Nanoseconds())/float64(b.N)/20e6*100, "%core")
}
