package web

import (
	"math"
	"testing"
)

func tone(rate, ch, n int, hz float64) []int16 {
	out := make([]int16, 0, n*ch)
	for i := 0; i < n; i++ {
		v := int16(10000 * math.Sin(2*math.Pi*hz*float64(i)/float64(rate)))
		for c := 0; c < ch; c++ {
			out = append(out, v)
		}
	}
	return out
}

func TestResampleKeepsATonesPitch(t *testing.T) {
	for _, rate := range []int{8000, 16000, 22050, 44100, 48000} {
		r := newResampler(rate, 1)
		out := r.Push(tone(rate, 1, rate, 1000)) // one second
		if len(out) < 40000 {
			t.Fatalf("rate %d: only %d output samples", rate, len(out))
		}
		crossings := 0
		for i := 1; i < len(out); i++ {
			if out[i-1] < 0 && out[i] >= 0 {
				crossings++
			}
		}
		hz := float64(crossings) * 48000 / float64(len(out))
		if math.Abs(hz-1000) > 10 {
			t.Errorf("rate %d: got %.1f Hz, want 1000", rate, hz)
		}
	}
}

func TestResampleAcrossChunkBoundariesIsSeamless(t *testing.T) {
	for _, rate := range []int{8000, 22050, 44100} {
		in := tone(rate, 2, 4410, 440)
		whole := newResampler(rate, 2).Push(in)
		r := newResampler(rate, 2)
		var parts []int16
		for i := 0; i < 100; i++ {
			lo, hi := i*len(in)/100, (i+1)*len(in)/100
			lo, hi = lo&^1, hi&^1
			if i == 99 {
				hi = len(in)
			}
			parts = append(parts, r.Push(in[lo:hi])...)
		}
		if len(parts) != len(whole) {
			t.Fatalf("rate %d: %d vs %d samples", rate, len(parts), len(whole))
		}
		for i := range whole {
			if d := int(whole[i]) - int(parts[i]); d < -1 || d > 1 {
				t.Fatalf("rate %d: sample %d differs: %d vs %d", rate, i, whole[i], parts[i])
			}
		}
	}
}

func TestMoreThanTwoChannelsDownmixToStereo(t *testing.T) {
	r := newResampler(48000, 4)
	if r.Channels() != 2 {
		t.Fatalf("channels = %d", r.Channels())
	}
	out := r.Push([]int16{100, 10, 300, 30, -32768, 7, -32768, 9})
	want := []int16{200, 20, -32768, 8}
	if len(out) != len(want) {
		t.Fatalf("out = %v", out)
	}
	for i := range want {
		if out[i] != want[i] {
			t.Fatalf("out = %v, want %v", out, want)
		}
	}
	if newResampler(16000, 6).Channels() != 2 {
		t.Fatal("6 channels should be stereo")
	}
}

func TestMonoStaysMono(t *testing.T) {
	r := newResampler(48000, 1)
	if r.Channels() != 1 {
		t.Fatalf("channels = %d", r.Channels())
	}
	in := []int16{1, -2, 3, 32767, -32768}
	out := r.Push(in)
	if len(out) != len(in) {
		t.Fatalf("out = %v", out)
	}
	for i := range in {
		if out[i] != in[i] {
			t.Fatalf("not bit-exact: %v", out)
		}
	}
	if newResampler(8000, 1).Channels() != 1 {
		t.Fatal("8 kHz mono should stay mono")
	}
}

// streamHash folds output into a running FNV-1a hash and counts samples, so
// long streams compare without being held in memory.
type streamHash struct {
	h uint64
	n int
}

func (s *streamHash) add(out []int16) {
	if s.h == 0 {
		s.h = 14695981039346656037
	}
	for _, v := range out {
		s.h = (s.h ^ uint64(uint16(v))) * 1099511628211
	}
	s.n += len(out)
}

func TestResampleLongStreamsDoNotDrift(t *testing.T) {
	seconds := 600
	if testing.Short() {
		seconds = 60
	}
	for _, rate := range []int{44100, 22050, 8000} {
		for _, ch := range []int{1, 2} {
			whole, single, odd := newResampler(rate, ch), newResampler(rate, ch), newResampler(rate, ch)
			var hw, hs, ho streamHash
			oddSizes := []int{7, 13, 101}
			oi, singles := 0, 0
			for s := 0; s < seconds; s++ {
				block := make([]int16, rate*ch)
				for i := 0; i < rate; i++ {
					v := int16(10000 * math.Sin(2*math.Pi*1000*float64(i)/float64(rate)))
					for c := 0; c < ch; c++ {
						block[i*ch+c] = v
					}
				}
				hw.add(whole.Push(block))
				for i := 0; i < len(block); i += ch {
					hs.add(single.Push(block[i : i+ch]))
					if singles++; singles%5 == 0 {
						hs.add(single.Push(nil))
					}
				}
				for i := 0; i < len(block); {
					n := oddSizes[oi%len(oddSizes)]
					oi++
					if i+n > len(block) {
						n = len(block) - i
					}
					ho.add(odd.Push(block[i : i+n]))
					i += n
				}
			}
			if hs != hw || ho != hw {
				t.Errorf("rate %d ch %d: outputs differ: whole %+v single %+v odd %+v", rate, ch, hw, hs, ho)
			}
			want := float64(rate*seconds) * outRate / float64(rate) * float64(ch)
			slack := (float64(outRate)/float64(rate) + 1) * float64(ch)
			if d := math.Abs(float64(hw.n) - want); d > slack {
				t.Errorf("rate %d ch %d: %d samples, want %.0f within %.1f", rate, ch, hw.n, want, slack)
			}
		}
	}
}
