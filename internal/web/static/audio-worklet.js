// The device's sound, played from a buffer that adapts to the link. audio.js decodes the Opus
// packets and posts each 20 ms of samples here; this plays them out.
//
// The buffer aims to hold `target` ms before it plays. Running dry in the middle of a burst is an
// underrun: the target goes up 50 ms, to 1 s at most, and the buffer fills to it again. After 5 s
// with no underrun the target comes down 10 ms, to 150 ms at least. Running dry before a burst
// start isn't an underrun, as the device simply went quiet. A buffer well past its target, as
// after the tab was in the background, drops its oldest sound to get back to the target, fading
// from the sound it was playing into the sound it skips to so the jump doesn't click.
//
// While it plays, playback runs up to 0.5% fast or slow, in proportion to how far a smoothed level
// is from the target, which holds the buffer steady against the device's clock drifting from the
// Mac's, and brings it back to the target without a skip. Skipping ahead is only the backstop.
//
// A new format keeps what is buffered, which plays out, as both channels are always stored; its
// first packet counts as a burst start. Only a clear, at hang-up and sound off, empties the buffer.
'use strict';

const rate = 48000; // what the server sends, after its resampling
const perMs = rate / 1000;
const startTarget = 150;
const maxTarget = 1000;
const raiseBy = 50;
const easeBy = 10;
const easeEvery = 5; // seconds
const skipPast = 300; // ms over the target before playback skips ahead
const statsEvery = 0.5; // seconds
// A buffer that hasn't reached its target plays anyway once nothing has come for this long: the
// burst was shorter than the target, or it ended.
const primeWait = 0.1; // seconds
const capacity = 2 * rate; // more than the most the buffer keeps, maxTarget + skipPast
const fadeMs = 8; // the crossfade when playback skips ahead
const maxTrim = 0.005; // the most playback runs fast or slow: 0.5%, under 9 cents
const trimSpan = 100; // ms from the target at which the trim is at its most
const trimSmooth = 1; // seconds: the level is smoothed over about this long, so the rate doesn't wobble

class PreflashAudio extends AudioWorkletProcessor {
  constructor() {
    super();
    this.bufs = [new Float32Array(capacity), new Float32Array(capacity)];
    this.channels = 1;
    this.start = 0; // index of the oldest sample
    this.level = 0; // samples held
    this.pos = 0; // fraction of a sample read, when the context's rate isn't 48 kHz
    this.baseStep = rate / sampleRate;
    this.step = this.baseStep;
    this.trim = 0; // how much faster than real time playback runs
    this.smooth = 0; // the level in ms, smoothed, while playing
    this.target = startTarget;
    this.underruns = 0;
    this.playing = false; // the buffer is primed and playing out
    this.dry = false; // it ran dry, and the next packet says whether that was an underrun
    this.ended = false; // a stop came: what is left plays out, and running dry is fine
    this.fresh = false; // a format came: the next packet starts a burst
    this.lastPush = 0;
    this.lastChange = 0; // when the target last moved
    this.lastStats = 0;
    this.sounding = false; // what the page was last told
    // A skip ahead fades out of a copy of the sound that was playing, the tail, while the sound
    // it skipped to fades in.
    this.fadeLen = Math.max(1, Math.round((fadeMs / 1000) * sampleRate)); // in frames played
    const tailLen = Math.ceil(this.fadeLen * this.step * 1.01) + 2;
    this.tail = [new Float32Array(tailLen), new Float32Array(tailLen)];
    this.tailHeld = 0;
    this.tailAt = 0;
    this.fading = 0; // frames of the fade left to play
    this.port.onmessage = (e) => this.receive(e.data);
  }

  receive(m) {
    switch (m.type) {
      case 'format':
        this.channels = m.channels === 2 ? 2 : 1;
        this.fresh = true;
        break;
      case 'clear':
        this.clear();
        break;
      case 'stop':
        this.ended = true;
        break;
      case 'audio':
        this.push(m);
        break;
    }
  }

  clear() {
    this.start = 0;
    this.level = 0;
    this.pos = 0;
    this.playing = false;
    this.dry = false;
    this.ended = false;
    this.fresh = false;
    this.fading = 0;
  }

  push(m) {
    const burst = m.burst || this.fresh;
    this.fresh = false;
    if (this.dry) {
      this.dry = false;
      if (!burst) {
        // The sound was meant to carry on: the link was late.
        this.underruns++;
        this.target = Math.min(this.target + raiseBy, maxTarget);
        this.lastChange = currentTime;
      }
    }
    this.ended = false;
    this.lastPush = currentTime;
    const n = m.data[0].length;
    for (let c = 0; c < 2; c++) {
      const from = m.data[Math.min(c, m.data.length - 1)];
      const buf = this.bufs[c];
      let at = (this.start + this.level) % capacity;
      for (let i = 0; i < n; i++) {
        buf[at] = from[i];
        if (++at === capacity) at = 0;
      }
    }
    this.level += n;
    if (this.level > capacity) this.drop(this.level - capacity);
    const keep = this.target * perMs;
    if (this.level > keep + skipPast * perMs) this.skip(this.level - keep);
  }

  // skip drops the oldest n samples. While playing, what was about to play is kept as the tail
  // to fade out of. A skip during a fade, which takes another 300 ms arriving within it, only drops.
  skip(n) {
    if (this.playing && this.fading === 0) {
      const held = Math.min(this.tail[0].length, this.level);
      for (let c = 0; c < 2; c++) {
        const buf = this.bufs[c];
        const tail = this.tail[c];
        for (let i = 0, at = this.start; i < held; i++) {
          tail[i] = buf[at];
          if (++at === capacity) at = 0;
        }
      }
      this.tailHeld = held;
      this.tailAt = 0;
      this.fading = this.fadeLen;
    }
    this.drop(n);
  }

  drop(n) {
    n = Math.max(Math.min(n, this.level), 0);
    this.start = (this.start + n) % capacity;
    this.level -= n;
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const frames = out[0].length;
    const now = currentTime;

    if (!this.playing && this.level > 0) {
      const waited = now - this.lastPush > primeWait;
      if (this.level >= this.target * perMs || this.ended || waited) {
        this.playing = true;
        this.smooth = this.target; // primed to it, so the trim starts at nothing and eases in
      }
    }

    if (this.playing) {
      this.smooth += (this.level / perMs - this.smooth) * (frames / (sampleRate * trimSmooth));
      const off = (this.smooth - this.target) / trimSpan;
      this.trim = Math.max(-maxTrim, Math.min(maxTrim, off * maxTrim));
    } else {
      this.trim = 0;
    }
    this.step = this.baseStep * (1 + this.trim);

    if (this.playing) {
      const [l, r] = this.bufs;
      for (let i = 0; i < frames; i++) {
        if (this.level < 2) {
          // Dry. Whether that was an underrun waits on the next packet.
          for (let c = 0; c < out.length; c++) out[c].fill(0, i);
          this.drop(this.level);
          this.pos = 0;
          this.playing = false;
          this.fading = 0;
          if (!this.ended) this.dry = true;
          this.ended = false;
          break;
        }
        // Linear interpolation, which is a straight copy when the rates match.
        const a = this.start;
        const b = (a + 1) % capacity;
        const f = this.pos;
        let left = l[a] + (l[b] - l[a]) * f;
        let right = r[a] + (r[b] - r[a]) * f;
        if (this.fading > 0) {
          const g = 1 - this.fading / this.fadeLen; // how far into the new sound
          const t = this.tailAt;
          const u = Math.min(t + 1, this.tailHeld - 1);
          const [tl, tr] = this.tail;
          const oldL = t < this.tailHeld ? tl[t] + (tl[u] - tl[t]) * f : 0;
          const oldR = t < this.tailHeld ? tr[t] + (tr[u] - tr[t]) * f : 0;
          left = oldL + (left - oldL) * g;
          right = oldR + (right - oldR) * g;
          this.fading--;
        }
        out[0][i] = left;
        if (out.length > 1) out[1][i] = right;
        this.pos += this.step;
        const whole = Math.floor(this.pos);
        this.pos -= whole;
        this.tailAt += whole;
        // Below 24 kHz a step can pass the end of what is held.
        this.drop(Math.min(whole, this.level));
      }
    } else {
      for (let c = 0; c < out.length; c++) out[c].fill(0);
    }

    if (now - this.lastChange >= easeEvery) {
      this.target = Math.max(this.target - easeBy, startTarget);
      this.lastChange = now;
    }

    const sounding = this.playing;
    if (sounding !== this.sounding || now - this.lastStats >= statsEvery) {
      this.sounding = sounding;
      this.lastStats = now;
      this.port.postMessage({
        type: 'stats',
        level: Math.round(this.level / perMs),
        target: this.target,
        underruns: this.underruns,
        trim: Math.round(this.trim * 10000) / 100, // percent
        playing: sounding,
      });
    }
    return true;
  }
}

registerProcessor('preflash-audio', PreflashAudio);
