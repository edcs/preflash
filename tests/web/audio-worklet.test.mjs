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
