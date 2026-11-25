// The device's sound, played from a buffer that adapts to the link. audio.js decodes the Opus
// packets and posts each 20 ms of samples here; this plays them out.
//
// The buffer aims to hold `target` ms before it plays. Running dry in the middle of a burst is an
// underrun: the target goes up 50 ms, to 1 s at most, and the buffer fills to it again. After 5 s
// with no underrun the target comes down 10 ms, to 150 ms at least. Running dry before a burst
// start isn't an underrun, as the device simply went quiet. A buffer well past its target, as
// after the tab was in the background, drops its oldest sound to get back to the target.
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

class PreflashAudio extends AudioWorkletProcessor {
  constructor() {
    super();
    this.bufs = [new Float32Array(capacity), new Float32Array(capacity)];
    this.channels = 1;
    this.start = 0; // index of the oldest sample
    this.level = 0; // samples held
    this.pos = 0; // fraction of a sample read, when the context's rate isn't 48 kHz
    this.step = rate / sampleRate;
    this.target = startTarget;
    this.underruns = 0;
    this.playing = false; // the buffer is primed and playing out
    this.dry = false; // it ran dry, and the next packet says whether that was an underrun
    this.ended = false; // a stop came: what is left plays out, and running dry is fine
    this.lastPush = 0;
    this.lastChange = 0; // when the target last moved
    this.lastStats = 0;
    this.sounding = false; // what the page was last told
    this.port.onmessage = (e) => this.receive(e.data);
  }

  receive(m) {
    switch (m.type) {
      case 'format':
        this.channels = m.channels === 2 ? 2 : 1;
        this.clear();
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
  }

  push(m) {
    if (this.dry) {
      this.dry = false;
      if (!m.burst) {
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
    if (this.level > keep + skipPast * perMs) this.drop(this.level - keep);
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
      if (this.level >= this.target * perMs || this.ended || waited) this.playing = true;
    }

    if (this.playing) {
      const [l, r] = this.bufs;
      for (let i = 0; i < frames; i++) {
        if (this.level < 2) {
          // Dry. Whether that was an underrun waits on the next packet.
          for (let c = 0; c < out.length; c++) out[c].fill(0, i);
          this.drop(this.level);
          this.pos = 0;
          this.playing = false;
          if (!this.ended) this.dry = true;
          this.ended = false;
          break;
        }
        // Linear interpolation, which is a straight copy when the rates match.
        const a = this.start;
        const b = (a + 1) % capacity;
        const f = this.pos;
        out[0][i] = l[a] + (l[b] - l[a]) * f;
        if (out.length > 1) out[1][i] = r[a] + (r[b] - r[a]) * f;
        this.pos += this.step;
        const whole = Math.floor(this.pos);
        this.pos -= whole;
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
        playing: sounding,
      });
    }
    return true;
  }
}

registerProcessor('preflash-audio', PreflashAudio);
