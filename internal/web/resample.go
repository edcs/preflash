package web

const outRate = 48000

// resampler converts interleaved int16 PCM at any rate and channel count to
// 48 kHz mono or stereo. More than two channels become stereo: left is the
// mean of the even-indexed channels, right the mean of the odd-indexed ones.
// Resampling is linear interpolation; position state carries across Push
// calls, so chunk boundaries leave no seam.
type resampler struct {
	from  int
	inCh  int
	outCh int
	rem   []int16 // incomplete trailing input frame
	buf   []int16 // downmixed frames kept for interpolation
	pos   int64   // read position in buf, in 1/outRate input frames
}

func newResampler(fromRate, fromChannels int) *resampler {
	// Rates above 48 kHz would alias (no low-pass filter) and are not
	// expected: firmware opens the codec at 8-48 kHz.
	if fromRate <= 0 {
		fromRate = outRate
	}
	if fromChannels < 1 {
		fromChannels = 1
	}
	outCh := 2
	if fromChannels == 1 {
		outCh = 1
	}
	return &resampler{from: fromRate, inCh: fromChannels, outCh: outCh}
}

// Channels reports the output channel count, 1 or 2.
func (r *resampler) Channels() int { return r.outCh }

func sat16(v int) int16 {
	if v > 32767 {
		return 32767
	}
	if v < -32768 {
		return -32768
	}
	return int16(v)
}

func (r *resampler) downmix(in []int16) []int16 {
	if r.inCh == r.outCh {
		return in
	}
	n := len(in) / r.inCh
	out := make([]int16, 0, n*2)
	for f := 0; f < n; f++ {
		fr := in[f*r.inCh : (f+1)*r.inCh]
		var ls, rs, lc, rc int
		for c, v := range fr {
			if c%2 == 0 {
				ls += int(v)
				lc++
			} else {
				rs += int(v)
				rc++
			}
		}
		out = append(out, sat16(ls/lc), sat16(rs/rc))
	}
	return out
}

// Push consumes interleaved input and returns the 48 kHz output it can
// produce so far.
func (r *resampler) Push(in []int16) []int16 {
	if len(r.rem) > 0 {
		in = append(append([]int16(nil), r.rem...), in...)
		r.rem = nil
	}
	if whole := len(in) / r.inCh * r.inCh; whole != len(in) {
		r.rem = append([]int16(nil), in[whole:]...)
		in = in[:whole]
	}
	mixed := r.downmix(in)
	if r.from == outRate {
		return append([]int16(nil), mixed...)
	}
	ch := r.outCh
	r.buf = append(r.buf, mixed...)
	frames := int64(len(r.buf) / ch)
	var out []int16
	for {
		idx := r.pos / outRate
		if idx+1 >= frames {
			break
		}
		frac := r.pos % outRate
		a := r.buf[int(idx)*ch : int(idx)*ch+ch]
		b := r.buf[int(idx+1)*ch : int(idx+1)*ch+ch]
		for c := 0; c < ch; c++ {
			d := (int64(b[c]) - int64(a[c])) * frac
			// round half away from zero
			if d >= 0 {
				d += outRate / 2
			} else {
				d -= outRate / 2
			}
			out = append(out, int16(int64(a[c])+d/outRate))
		}
		r.pos += int64(r.from)
	}
	drop := r.pos / outRate
	if drop > frames {
		drop = frames
	}
	r.buf = append(r.buf[:0], r.buf[int(drop)*ch:]...)
	r.pos -= drop * outRate
	return out
}
