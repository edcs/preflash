// The sound buffer's rules, run against internal/web/static/audio-worklet.js in Node with the
// AudioWorklet globals stubbed. Run with `node --test tests/web/audio-worklet.test.mjs`.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(new URL('../../internal/web/static/audio-worklet.js', import.meta.url), 'utf8');
const quantum = 128; // frames per process() call, as in a browser

// load runs the worklet's script in a fresh global scope and gives back a player and a clock.
function load(sampleRate = 48000) {
  const g = {
    sampleRate,
    currentTime: 0,
    AudioWorkletProcessor: class {
      constructor() {
        this.port = { posted: [], postMessage(m) { this.posted.push(m); } };
      }
    },
    registerProcessor(name, cls) { g.registered = cls; },
  };
  vm.createContext(g);
  vm.runInContext(source, g);
  const p = new g.registered();
  const player = {
    p,
    g,
    send(m) { p.port.onmessage({ data: m }); },
    // push sends ms of sound, 20 ms to a packet, every sample `value`.
    push(ms, { burst = false, value = 0.5, channels = 1 } = {}) {
      for (let left = ms; left > 0; left -= 20) {
        const n = (Math.min(left, 20) * 48000) / 1000;
        const data = Array.from({ length: channels }, () => new Float32Array(n).fill(value));
        this.send({ type: 'audio', burst, data });
        burst = false;
      }
    },
    // run plays for ms, moving the clock on, and gives back the left channel played.
    run(ms) {
      const out = [];
      const calls = Math.ceil((ms / 1000) * sampleRate / quantum);
      for (let i = 0; i < calls; i++) {
        const frame = [new Float32Array(quantum), new Float32Array(quantum)];
        p.process([], [frame]);
        out.push(...frame[0]);
        g.currentTime += quantum / sampleRate;
      }
      return out;
    },
    levelMs() { return p.level / 48; },
    // stream plays for ms while 20 ms packets arrive in real time, calling look after each
    // quantum.
    stream(ms, look = () => {}) {
      const calls = Math.round((ms / 1000) * sampleRate / quantum);
      for (let i = 0; i < calls; i++) {
        this.fed = (this.fed || 0) + quantum * 48000 / sampleRate;
        while (this.fed >= 960) {
          this.push(20);
          this.fed -= 960;
        }
        this.run(quantum / sampleRate * 1000);
        look();
      }
    },
    // rate is how much faster than real time playback runs: 0.005 is 0.5% faster.
    rate() { return p.step / (48000 / sampleRate) - 1; },
  };
  return player;
}

test('a burst start primes to the target and is never an underrun', () => {
  const a = load();
  a.push(60, { burst: true });
  a.run(10);
  assert.equal(a.p.playing, false, 'waits to fill to the target while packets keep coming');
  a.push(100);
  a.run(3);
  assert.equal(a.p.playing, true, 'plays once the target is held');
  a.run(400); // the burst ends and the buffer runs dry
  assert.equal(a.p.playing, false);
  a.push(20, { burst: true });
  a.run(200);
  assert.equal(a.p.underruns, 0);
  assert.equal(a.p.target, 150);
});

test('a short burst plays once nothing more comes', () => {
  const a = load();
  a.push(40, { burst: true });
  a.run(150);
  assert.equal(a.levelMs() < 40, true, 'it played what it had');
});

test('running dry mid-burst is an underrun and raises the target 50 ms', () => {
  const a = load();
  a.push(160, { burst: true });
  a.run(200); // plays it all and runs dry
  a.push(20); // the burst carries on: the link was late
  assert.equal(a.p.underruns, 1);
  assert.equal(a.p.target, 200);
  a.run(200); // it waits out the prime, plays the 20 ms and runs dry again
  a.push(20);
  assert.equal(a.p.underruns, 2);
  assert.equal(a.p.target, 250);
});

test('the target eases down 10 ms for each 5 s without an underrun, to 150 ms', () => {
  const a = load();
  a.push(160, { burst: true });
  a.run(200);
  a.push(20);
  assert.equal(a.p.target, 200);
  a.run(4900);
  assert.equal(a.p.target, 200);
  a.run(200);
  assert.equal(a.p.target, 190);
  a.run(5000);
  assert.equal(a.p.target, 180);
  a.run(60000);
  assert.equal(a.p.target, 150);
});

test('a format keeps what is buffered, and its first packet is a burst start', () => {
  const a = load();
  a.send({ type: 'format', channels: 1 });
  a.push(200, { burst: true });
  a.run(20);
  assert.equal(a.p.playing, true);
  const held = a.levelMs();
  a.send({ type: 'format', channels: 2 }); // the codec reopened in stereo
  assert.equal(a.levelMs(), held, 'the sound already here still plays');
  assert.equal(a.p.playing, true);
  a.run(400); // it plays out and runs dry
  a.push(20, { channels: 2 }); // the new format's first packet, not marked by the server
  assert.equal(a.p.underruns, 0);
  // The flag is used up: running dry again mid-burst is an underrun.
  a.run(300);
  a.push(20, { channels: 2 });
  assert.equal(a.p.underruns, 1);
});

test('clear, for hang-up and sound off, empties the buffer', () => {
  const a = load();
  a.push(200, { burst: true });
  a.run(20);
  a.send({ type: 'clear' });
  assert.equal(a.p.level, 0);
  assert.equal(a.p.playing, false);
});

test('a buffer 300 ms past its target skips to the target with a short crossfade', () => {
  const a = load();
  a.push(160, { burst: true, value: 1 });
  a.run(1);
  assert.equal(a.p.playing, true);
  a.push(140, { value: 1 }); // 297 ms held: not yet 300 past the target
  assert.equal(Math.round(a.levelMs()), 297);
  a.push(160, { value: 0 }); // past 450: the oldest goes, leaving the target, all zeros
  assert.equal(a.levelMs(), 150);
  const out = a.run(20);
  assert.ok(out[0] > 0.95, `starts from where it was playing, not a jump (${out[0]})`);
  let steepest = 0;
  for (let i = 1; i < out.length; i++) steepest = Math.max(steepest, Math.abs(out[i] - out[i - 1]));
  assert.ok(steepest < 1 / 200, `no click: the steepest step is ${steepest}`);
  const faded = out.findIndex((x) => x === 0);
  assert.ok(faded >= 5 * 48 && faded <= 10 * 48 + 1, `fades over 5 to 10 ms (${faded / 48} ms)`);
  assert.ok(out.slice(faded).every((x) => x === 0), 'then plays the new sound');
});

test('playback runs up to 0.5% fast to bring a full buffer down to its target', () => {
  const a = load();
  a.push(400, { burst: true }); // 250 ms over the target, short of a skip
  a.run(1);
  let fastest = 0;
  let steepest = 0;
  let last = a.rate();
  a.stream(60000, () => {
    fastest = Math.max(fastest, a.rate());
    steepest = Math.max(steepest, Math.abs(a.rate() - last));
    last = a.rate();
  });
  assert.ok(fastest > 0.004, `it trims (${fastest})`);
  assert.ok(fastest <= 0.005 + 1e-12, `by 0.5% at most (${fastest})`);
  assert.ok(steepest < 0.00005, `smoothly, not wobbling (${steepest} in one quantum)`);
  assert.ok(a.levelMs() < a.p.target + 60, `it drained toward the target (${a.levelMs()} ms)`);
  assert.equal(a.p.underruns, 0);
});

test('playback runs up to 0.5% slow to fill a low buffer, at any context rate', () => {
  for (const rate of [48000, 44100]) {
    const a = load(rate);
    a.push(40, { burst: true });
    a.run(110); // too short for the target: it plays once nothing more comes
    assert.equal(a.p.playing, true);
    let slowest = 0;
    a.stream(10000, () => { slowest = Math.min(slowest, a.rate()); });
    assert.ok(slowest < -0.004, `it trims (${slowest})`);
    assert.ok(slowest >= -0.005 - 1e-12, `by 0.5% at most (${slowest})`);
  }
});
