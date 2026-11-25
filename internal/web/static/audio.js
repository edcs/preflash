// The device's sound. Off by default: a tap on the speaker starts it, since iPad Safari only lets
// a page make sound from a tap. While on, the page listens on /audio, decodes each 20 ms Opus
// packet with the vendored WASM decoder, and posts the samples to audio-worklet.js, which plays
// them from a buffer that adapts to the link. It uses reconnecting() and store from app.js.
'use strict';

(() => {
  const speaker = document.getElementById('speaker');
  const OpusDecoder = window['opus-decoder'].OpusDecoder;
  const frameSamples = 960; // 20 ms at 48 kHz
  const maxConceal = 50; // a gap longer than 1 s isn't filled

  let wanted = store.get('preflash.sound') === 'on';
  let ctx = null; // the AudioContext, made on the first tap and kept
  let node = null; // the worklet's node, once its module has loaded
  let socket = null; // the reconnecting /audio socket while sound is on
  let current = null; // its connection now: only this one's messages and close count
  let decoder = null; // a fresh one for each format
  let channels = 1;
  let lastSeq = -1; // the last packet decoded under this format
  let lastFrame = null; // its samples, for a fade when concealment isn't there
  let playing = false;
  let resuming = false; // a resume is under way

  // What the worklet last said, for the tests and anyone looking from the console.
  const stats = { level: 0, target: 0, underruns: 0, playing: false };
  window.preflashAudio = { stats };

  // ---- The button ----------------------------------------------------------------------------

  const titles = {
    off: 'Sound is off. Tap to hear the device.',
    resume: 'Sound was on. Tap to turn it back on.',
    starting: 'Sound is on, starting.',
    idle: 'Sound is on. The device is quiet.',
    playing: 'Sound is on. Tap to turn it off.',
  };

  function show() {
    let s = 'off';
    if (!wanted) s = 'off';
    else if (!ctx || (ctx.state !== 'running' && !resuming)) s = 'resume';
    else if (!node || ctx.state !== 'running') s = 'starting';
    else s = playing ? 'playing' : 'idle';
    speaker.dataset.state = s;
    speaker.setAttribute('aria-pressed', String(wanted && s !== 'resume'));
    speaker.title = titles[s];
  }

  // As with the zoom controls, a tap mustn't take the focus, and the iPad's keyboard, from the
  // terminal. The click is still the user's gesture that lets sound start.
  speaker.addEventListener('pointerdown', (e) => e.preventDefault());
  speaker.addEventListener('mousedown', (e) => e.preventDefault());
  speaker.addEventListener('click', () => {
    const on = speaker.dataset.state === 'off' || speaker.dataset.state === 'resume';
    if (on) start(); else stop();
  });

  // ---- On and off ----------------------------------------------------------------------------

  // start runs inside the tap: the context has to be made, or resumed, there.
  function start() {
    wanted = true;
    store.set('preflash.sound', 'on');
    if (!ctx) {
      try {
        ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
      } catch {
        ctx = new AudioContext(); // the worklet resamples
      }
      ctx.addEventListener('statechange', settle);
      ctx.audioWorklet.addModule('audio-worklet.js').then(() => {
        node = new AudioWorkletNode(ctx, 'preflash-audio', { numberOfInputs: 0, outputChannelCount: [2] });
        node.port.onmessage = (e) => heard(e.data);
        node.connect(ctx.destination);
        if (wanted) listen();
        show();
      }, (err) => {
        console.warn('audio: the player did not load:', err);
        ctx = null;
        wanted = false;
        show();
      });
    }
    resume();
    if (node) listen();
    show();
  }

  function resume() {
    resuming = true;
    ctx.resume().catch(() => {}).finally(() => {
      resuming = false;
      settle();
    });
  }

  // settle closes /audio while sound is on but the context can't play, as when iPad Safari
  // suspended it and wants a tap: nothing comes over the link until that tap.
  function settle() {
    if (wanted && ctx && ctx.state !== 'running' && !resuming) hangUp();
    else if (wanted && node && ctx.state === 'running') listen(); // it resumed without a tap
    show();
  }

  function hangUp() {
    if (socket) socket.close();
    socket = null;
    current = null;
    forget();
    if (node) node.port.postMessage({ type: 'clear' });
    playing = false;
  }

  function stop() {
    wanted = false;
    store.set('preflash.sound', null);
    hangUp();
    if (ctx) ctx.suspend().catch(() => {});
    show();
  }

  function heard(m) {
    if (m.type !== 'stats') return;
    Object.assign(stats, { level: m.level, target: m.target, underruns: m.underruns, playing: m.playing });
    if (m.playing !== playing) {
      playing = m.playing;
      show();
    }
  }

  // ---- The socket ----------------------------------------------------------------------------

  function listen() {
    if (socket) return;
    socket = reconnecting('/audio', 'audio', (ws) => {
      ws.binaryType = 'arraybuffer';
      current = ws;
      ws.addEventListener('message', (e) => {
        if (ws !== current) return;
        if (typeof e.data !== 'string') {
          packet(e.data);
          return;
        }
        let m;
        try { m = JSON.parse(e.data); } catch { return; }
        switch (m.type) {
          case 'format': format(m); break;
          case 'stop':
            // A stop with no format before it says nothing new.
            if (!decoder) break;
            forget();
            node.port.postMessage({ type: 'stop' });
            break;
        }
      });
      // A new connection starts with a format, if there is sound to come. A socket ended for
      // silence may close for real much later, once its replacement is playing: that close is
      // ignored.
      ws.addEventListener('close', () => {
        if (ws !== current) return;
        current = null;
        forget();
      });
    });
  }

  // ---- Decoding ------------------------------------------------------------------------------

  function forget() {
    const d = decoder;
    if (d && d.waiting) d.ready.then(() => d.free());
    else if (d) d.free();
    decoder = null;
    lastSeq = -1;
    lastFrame = null;
  }

  function format(m) {
    forget();
    channels = m.channels === 2 ? 2 : 1;
    // The WASM is compiled once the script loads, so a new decoder is ready at once in practice;
    // packets that come before it are decoded when it is.
    const d = new OpusDecoder({ channels, sampleRate: 48000 });
    d.waiting = [];
    decoder = d;
    d.ready.then(() => {
      if (decoder !== d) return;
      const waiting = d.waiting;
      d.waiting = null;
      for (const data of waiting) packet(data);
    });
    node.port.postMessage({ type: 'format', channels });
  }

  function packet(data) {
    const d = decoder;
    if (!d || data.byteLength < 6) return;
    if (d.waiting) {
      d.waiting.push(data);
      return;
    }
    const view = new DataView(data);
    const seq = view.getUint32(0);
    let burst = (view.getUint8(4) & 1) !== 0;
    if (lastSeq >= 0 && seq <= lastSeq) return; // a repeat, or out of order
    if (!burst && lastSeq >= 0 && seq > lastSeq + 1) {
      // With the player quiet, the gap was most likely a lost burst start: the sound starts
      // afresh here, with nothing to conceal and no underrun.
      if (!playing) burst = true;
      else conceal(Math.min(seq - lastSeq - 1, maxConceal));
    }
    lastSeq = seq;
    const out = d.decodeFrame(new Uint8Array(data, 5));
    if (out.samplesDecoded === 0) return;
    lastFrame = out.channelData.map((c) => c.slice());
    send(lastFrame, burst);
  }

  // conceal fills missing packets with the decoder's loss concealment, one 20 ms frame for each.
  // The decoder conceals when given an empty packet, and makes up to 120 ms at a time.
  function conceal(missing) {
    while (missing > 0) {
      const out = decoder.decodeFrame(new Uint8Array(0));
      const frames = Math.min(Math.floor(out.samplesDecoded / frameSamples), missing);
      if (frames === 0) {
        fade(missing);
        return;
      }
      send(out.channelData.map((c) => c.slice(0, frames * frameSamples)), false);
      missing -= frames;
    }
  }

  // fade stands in for concealment, should the decoder give none: the last frame, fading out
  // over the first missing packet, then quiet.
  function fade(missing) {
    if (!lastFrame) return;
    const n = lastFrame[0].length;
    const faded = lastFrame.map((c) => c.map((x, i) => x * (1 - i / n)));
    send(faded, false);
    for (let i = 1; i < missing; i++) send(lastFrame.map(() => new Float32Array(n)), false);
  }

  // send posts the samples to the worklet. Each channel is copied, so its buffer can be given away.
  function send(channelData, burst) {
    const data = channelData.map((c) => c.slice());
    node.port.postMessage({ type: 'audio', burst, data }, data.map((c) => c.buffer));
  }

  // ---- Waking --------------------------------------------------------------------------------

  // A tab that wakes checks its socket at once. iPad Safari may have suspended the context while
  // it slept; resuming without a tap may be refused, and then the button asks for one.
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState !== 'visible' || !wanted || !ctx) return;
    if (socket) socket();
    if (ctx.state !== 'running') resume();
  });

  show();
})();
