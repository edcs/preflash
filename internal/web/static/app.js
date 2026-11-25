// The Preflash web page: the terminal app on the left, the simulated device on the right.
// It talks to the server over two sockets, /terminal and /device, and each reconnects on its own.
'use strict';

const $ = (id) => document.getElementById(id);
const main = $('app');
const termEl = $('term');
const stopped = $('stopped');
const statusEl = $('status');
const stage = $('stage');
const screen = $('screen');
const panel = $('panel');
const strip = $('strip');
const banner = $('banner');

// ---- Sockets -------------------------------------------------------------------------------

const socketUp = { terminal: false, device: false };
let everDown = false;

function showBanner() {
  banner.hidden = !everDown || (socketUp.terminal && socketUp.device);
}

// reconnecting opens path and opens it again whenever it closes, waiting 0.5 s and then twice
// as long each time, up to 5 s. It returns a function that skips the wait.
function reconnecting(path, name, setup) {
  let delay = 500;
  let timer = null;
  let ws = null;
  function open() {
    timer = null;
    const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(`${scheme}//${location.host}${path}`);
    setup(ws);
    ws.addEventListener('open', () => {
      delay = 500;
      socketUp[name] = true;
      showBanner();
    });
    ws.addEventListener('close', (e) => {
      if (e.reason) console.info(`${path} closed: ${e.reason}`);
      socketUp[name] = false;
      everDown = true;
      showBanner();
      timer = setTimeout(open, delay);
      delay = Math.min(delay * 2, 5000);
    });
  }
  open();
  return function now() {
    if (timer !== null) {
      clearTimeout(timer);
      open();
    }
  };
}

function sendJSON(ws, value) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(value));
}

// ---- Terminal ------------------------------------------------------------------------------

// The TUI paints its own dark ground, so the terminal stays dark in both themes.
const term = new Terminal({
  fontFamily: 'ui-monospace, "SF Mono", Menlo, Consolas, monospace',
  fontSize: 13,
  cursorBlink: false,
  scrollback: 1000,
  theme: {
    background: '#0e1113',
    foreground: '#e7ecef',
    cursor: '#5fd4e8',
    cursorAccent: '#0e1113',
    selectionBackground: 'rgba(95, 212, 232, 0.3)',
  },
});
const fit = new FitAddon.FitAddon();
term.loadAddon(fit);
term.open(termEl);

// The server's own emulator answers the app's queries, even with no tab open, so xterm.js
// must not answer them a second time. Each handler returns true, which tells xterm.js the
// sequence is dealt with. None of these draws anything.
const queries = [
  { final: 'c' },                                   // primary device attributes
  { prefix: '>', final: 'c' },                      // secondary device attributes
  { final: 'n' },                                   // device status, cursor position
  { prefix: '?', final: 'n' },                      // DEC device status
  { prefix: '?', final: 'u' },                      // kitty keyboard flags
  { intermediates: '$', final: 'p' },               // ANSI mode request
  { prefix: '?', intermediates: '$', final: 'p' },  // DEC mode request
];
for (const id of queries) term.parser.registerCsiHandler(id, () => true);
// OSC 10, 11 and 12 ask for the colours with "?", and set them otherwise.
for (const n of [10, 11, 12]) term.parser.registerOscHandler(n, (data) => /^[?;]+$/.test(data));

let termWS = null;
let lastSent = 0; // when this tab last sent keys or its size
const encoder = new TextEncoder();

function sendSize() {
  if (!termWS || termWS.readyState !== WebSocket.OPEN) return;
  termWS.send(JSON.stringify({ type: 'resize', cols: term.cols, rows: term.rows }));
  lastSent = performance.now();
}

function sendBytes(bytes) {
  if (!termWS || termWS.readyState !== WebSocket.OPEN) return;
  // The pty takes the size of the tab that last typed or resized. If this tab has been
  // quiet, another may have changed it, so say this tab's size first.
  if (performance.now() - lastSent > 1000) sendSize();
  termWS.send(bytes);
  lastSent = performance.now();
}

function sendKeys(text) {
  sendBytes(encoder.encode(text));
}

term.onData(sendKeys);
// Some mouse reports come as raw bytes rather than text.
term.onBinary((data) => sendBytes(Uint8Array.from(data, (ch) => ch.charCodeAt(0) & 0xff)));
term.onResize(sendSize);

function refit() {
  if (termEl.clientWidth > 0 && termEl.clientHeight > 0) fit.fit();
}

const reconnectTerminal = reconnecting('/terminal', 'terminal', (ws) => {
  ws.binaryType = 'arraybuffer';
  ws.addEventListener('open', () => {
    termWS = ws;
    // A fresh connection starts with a snapshot of the whole screen.
    term.reset();
    stopped.hidden = true;
    refit();
    sendSize();
  });
  ws.addEventListener('message', (e) => {
    if (typeof e.data !== 'string') {
      term.write(new Uint8Array(e.data));
      return;
    }
    let m;
    try { m = JSON.parse(e.data); } catch { return; }
    switch (m.type) {
      case 'started':
        // A snapshot of the new run's screen follows.
        term.reset();
        stopped.hidden = true;
        sendSize();
        break;
      case 'exited':
        stopped.hidden = false;
        break;
      case 'error':
        console.warn('terminal:', m.text);
        break;
    }
  });
  ws.addEventListener('close', () => {
    if (termWS === ws) termWS = null;
  });
});

$('start').addEventListener('click', () => {
  sendJSON(termWS, { type: 'start' });
  term.focus();
});

// ---- Key row -------------------------------------------------------------------------------

// The iPad's on-screen keyboard has none of these keys.
function keyBytes(key) {
  const arrow = term.modes.applicationCursorKeysMode ? '\x1bO' : '\x1b[';
  switch (key) {
    case 'esc': return '\x1b';
    case 'tab': return '\t';
    case 'ctrl-c': return '\x03';
    case 'ctrl-r': return '\x12';
    case 'up': return arrow + 'A';
    case 'down': return arrow + 'B';
    case 'right': return arrow + 'C';
    case 'left': return arrow + 'D';
    case 'slash': return '/';
  }
  return '';
}

for (const button of document.querySelectorAll('.keys button')) {
  const press = () => {
    sendKeys(keyBytes(button.dataset.key));
    term.focus();
  };
  // Acting on pointerdown, and cancelling it, keeps the focus, and so the on-screen keyboard,
  // on the terminal.
  button.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    button.classList.add('pressed');
    press();
  });
  for (const type of ['pointerup', 'pointercancel', 'pointerleave']) {
    button.addEventListener(type, () => button.classList.remove('pressed'));
  }
  button.addEventListener('mousedown', (e) => e.preventDefault());
  // A click with no pointer behind it comes from the keyboard.
  button.addEventListener('click', (e) => {
    if (e.detail === 0) press();
  });
}

// ---- Device --------------------------------------------------------------------------------

let state = { connected: false, buttons: [] };
let devWS = null;
let pictureURL = null;
let shownKey = '';

function applyState(s) {
  state = s;
  describe();
  screen.classList.toggle('connected', !!s.connected);
  screen.classList.toggle('off', !!s.connected && s.power === 'off');
  // The same square-root response as the Mac window, so a low backlight is dim but visible.
  const backlight = Math.min(Math.max(s.backlight ?? 100, 0), 100);
  panel.style.filter = s.connected && s.power !== 'off' ? `brightness(${Math.sqrt(backlight / 100)})` : '';
  renderStrip(s.buttons || []);
  const key = `${s.width}x${s.height}@${s.board_rotation}`;
  if (s.width && s.height && key !== shownKey) {
    shownKey = key;
    layout();
  }
}

function describe() {
  const s = state;
  statusEl.textContent = '';
  if (!s.connected) {
    statusEl.textContent = 'No app running';
    return;
  }
  const power = s.power === 'download' ? 'on' : (s.power || 'on');
  statusEl.append(`${s.board || 'board'} · ${power} · ${s.fps ?? 0} fps`);
  for (const [on, text] of [[s.power === 'download', 'download mode'], [s.crashed, 'crashed']]) {
    if (!on) continue;
    const alert = document.createElement('span');
    alert.className = 'alert';
    alert.textContent = text;
    statusEl.append(' · ', alert);
  }
}

function showPicture(blob, ws) {
  const old = pictureURL;
  pictureURL = URL.createObjectURL(blob);
  panel.src = pictureURL;
  // Ack once the picture is decoded, so a slow page asks for frames no faster than it shows them.
  panel.decode().catch(() => {}).then(() => {
    if (old) URL.revokeObjectURL(old);
    sendJSON(ws, { type: 'ack' });
  });
}

const reconnectDevice = reconnecting('/device', 'device', (ws) => {
  ws.binaryType = 'blob';
  ws.addEventListener('open', () => { devWS = ws; });
  ws.addEventListener('message', (e) => {
    if (typeof e.data !== 'string') {
      showPicture(e.data, ws);
      return;
    }
    let m;
    try { m = JSON.parse(e.data); } catch { return; }
    switch (m.type) {
      case 'state': applyState(m); break;
      case 'frame': break; // its picture is the next message
      case 'error': console.warn('device:', m.text); break;
    }
  });
  ws.addEventListener('close', () => {
    if (devWS !== ws) return;
    devWS = null;
    // The server lets go of whatever this connection held.
    touch = null;
    held.clear();
    for (const b of strip.children) b.classList.remove('held');
  });
});

// ---- Layout --------------------------------------------------------------------------------

const wideQuery = matchMedia('(min-width: 900px)');

// layout sizes the panel to fit the device column: under half the width beside the terminal, or half
// the height above it.
function layout() {
  const cs = getComputedStyle(main);
  const w = main.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
  const h = main.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom);
  const panelW = state.width || 480;
  const panelH = state.height || 480;
  const turn = (((state.board_rotation || 0) % 360) + 360) % 360;
  const sideways = turn === 90 || turn === 270;
  const shownW = sideways ? panelH : panelW;
  const shownH = sideways ? panelW : panelH;
  const bezel = 20;
  const around = statusEl.offsetHeight + strip.offsetHeight + 2 * 10 + bezel;
  const roomW = (wideQuery.matches ? w * 0.45 : w) - bezel;
  const roomH = (wideQuery.matches ? h : h * 0.5) - around;
  let scale = Math.max(Math.min(roomW / shownW, roomH / shownH), 0.05);
  if (scale >= 2) scale = Math.floor(scale); // whole pixels when there's room
  screen.style.width = `${Math.round(shownW * scale)}px`;
  screen.style.height = `${Math.round(shownH * scale)}px`;
  panel.style.width = `${panelW * scale}px`;
  panel.style.height = `${panelH * scale}px`;
  panel.style.transform = `translate(-50%, -50%) rotate(${turn}deg)`;
  panel.classList.toggle('pixelated', scale >= 2);
}

// The body follows the visual viewport, which shrinks when the iPad's keyboard comes up.
function followViewport() {
  const vv = window.visualViewport;
  if (!vv) return;
  document.body.style.setProperty('--vv-height', `${vv.height}px`);
  document.body.style.setProperty('--vv-top', `${vv.offsetTop}px`);
  window.scrollTo(0, 0);
}

if (window.visualViewport) {
  visualViewport.addEventListener('resize', () => { followViewport(); layout(); refit(); });
  visualViewport.addEventListener('scroll', followViewport);
  followViewport();
}
new ResizeObserver(() => layout()).observe(main);
new ResizeObserver(() => requestAnimationFrame(refit)).observe(termEl);
wideQuery.addEventListener('change', layout);
layout();

// A tab that wakes doesn't wait out its backoff.
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') {
    reconnectTerminal();
    reconnectDevice();
  }
});

// ---- Touch on the panel --------------------------------------------------------------------

let touch = null; // the pointer pressing the panel: { id, u, v }
let moveQueued = false;

function panelPoint(e) {
  const r = screen.getBoundingClientRect();
  const clamp = (f) => Math.min(Math.max(f, 0), 1);
  return { u: clamp((e.clientX - r.left) / r.width), v: clamp((e.clientY - r.top) / r.height) };
}

function sendPointer(down) {
  if (!touch) return;
  sendJSON(devWS, { type: 'pointer', u: touch.u, v: touch.v, down });
}

screen.addEventListener('pointerdown', (e) => {
  if (touch || !state.connected) return;
  e.preventDefault();
  screen.setPointerCapture(e.pointerId);
  touch = { id: e.pointerId, ...panelPoint(e) };
  sendPointer(true);
});

screen.addEventListener('pointermove', (e) => {
  if (!touch || e.pointerId !== touch.id) return;
  Object.assign(touch, panelPoint(e));
  // Moves go at most once a frame.
  if (moveQueued) return;
  moveQueued = true;
  requestAnimationFrame(() => {
    moveQueued = false;
    sendPointer(true);
  });
});

function lift(e) {
  if (!touch || e.pointerId !== touch.id) return;
  if (e.type !== 'lostpointercapture') Object.assign(touch, panelPoint(e));
  sendPointer(false);
  touch = null;
}
for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) {
  screen.addEventListener(type, lift);
}
screen.addEventListener('contextmenu', (e) => e.preventDefault());

// ---- Buttons -------------------------------------------------------------------------------

const held = new Set(); // buttons this page is pressing

function setButton(name, down) {
  if (down) held.add(name); else held.delete(name);
  sendJSON(devWS, { type: 'button', name, down });
}

function makeButton(name) {
  const b = document.createElement('button');
  b.type = 'button';
  b.textContent = name;
  b.dataset.name = name;
  let pointer = null;
  b.addEventListener('pointerdown', (e) => {
    if (pointer !== null) return;
    e.preventDefault();
    pointer = e.pointerId;
    b.classList.add('held', 'down');
    setButton(name, true);
  });
  const release = (e) => {
    if (e.pointerId !== pointer) return;
    pointer = null;
    b.classList.remove('held');
    b.classList.toggle('down', !!(state.buttons || []).find((x) => x.name === name && x.down));
    if (held.has(name)) setButton(name, false);
  };
  for (const type of ['pointerup', 'pointercancel', 'pointerleave']) {
    b.addEventListener(type, release);
  }
  b.addEventListener('contextmenu', (e) => e.preventDefault());
  return b;
}

function renderStrip(buttons) {
  const names = buttons.map((b) => b.name).join('\n');
  if (names !== strip.dataset.names) {
    for (const name of held) setButton(name, false);
    strip.replaceChildren(...buttons.map((b) => makeButton(b.name)));
    strip.dataset.names = names;
    layout();
  }
  buttons.forEach((b, i) => {
    const el = strip.children[i];
    el.title = b.wiring || '';
    el.classList.toggle('down', !!b.down || el.classList.contains('held'));
  });
}
