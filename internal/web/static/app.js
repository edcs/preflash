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

// A socket that has said nothing for this long is taken as dead. The server pings every 15 s,
// and a page that slept, or lost its forward, may sit on a socket that looks open but isn't.
const silenceLimit = 40000;

// reconnecting opens path and opens it again whenever it closes, waiting 0.5 s and then twice
// as long each time, up to 5 s. It returns a function that checks the socket at once and skips
// the wait.
function reconnecting(path, name, setup) {
  let delay = 500;
  let timer = null;
  let ws = null;
  let heard = 0; // when ws last said anything
  function open() {
    timer = null;
    const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const sock = new WebSocket(`${scheme}//${location.host}${path}`);
    ws = sock;
    heard = performance.now();
    setup(sock);
    sock.addEventListener('open', () => {
      heard = performance.now();
      socketUp[name] = true;
      showBanner();
    });
    sock.addEventListener('message', () => { heard = performance.now(); });
    // The wait starts again only once the server has said something, so one that takes the
    // connection and drops it at once isn't asked twice a second forever.
    sock.addEventListener('message', () => { delay = 500; }, { once: true });
    let ended = false;
    sock.addEventListener('close', (e) => {
      // A silent socket is ended here before the browser gets round to it, and its own close
      // may follow much later.
      if (ended) return;
      ended = true;
      if (e.reason) console.info(`${path} closed: ${e.reason}`);
      socketUp[name] = false;
      everDown = true;
      showBanner();
      timer = setTimeout(open, delay);
      delay = Math.min(delay * 2, 5000);
    });
  }
  // check ends a socket that has been silent too long. Closing a dead one can take the browser
  // minutes, so everything listening hears the close now.
  function check() {
    if (!ws || ws.readyState === WebSocket.CLOSED || performance.now() - heard <= silenceLimit) return;
    const dead = ws;
    dead.close();
    dead.dispatchEvent(new CloseEvent('close', { code: 4000, reason: 'silent too long' }));
  }
  open();
  setInterval(check, 5000);
  return function now() {
    check();
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
  // Cancelling pointerdown keeps the focus, and so the on-screen keyboard, on the terminal.
  // The key goes on pointerup, so a sideways swipe that scrolls the row (and so cancels the
  // pointer) sends nothing.
  button.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    button.classList.add('pressed');
  });
  button.addEventListener('pointerup', () => {
    if (!button.classList.contains('pressed')) return;
    button.classList.remove('pressed');
    press();
  });
  for (const type of ['pointercancel', 'pointerleave']) {
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
let lastBoard = '';

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
  if (s.board) lastBoard = s.board;
  // Without an app, the overlay on the panel says why, and the line keeps only the board.
  if (!s.connected) {
    statusEl.textContent = lastBoard;
    return;
  }
  const alert = (text) => {
    const span = document.createElement('span');
    span.className = 'alert';
    span.textContent = text;
    return span;
  };
  statusEl.append(`${s.board} · `);
  statusEl.append(s.power === 'download' ? alert('download mode') : (s.power || 'on'));
  statusEl.append(` · ${s.fps ?? 0} fps`);
  if (s.crashed) statusEl.append(' · ', alert('crashed'));
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
const deviceCol = document.querySelector('.device-col');
const deviceHead = document.querySelector('.device-head');
const keysRow = document.querySelector('.keys');
const zoomOut = $('zoom-out');
const zoomIn = $('zoom-in');
const zoomFit = $('zoom-fit');
const zoomLabel = $('zoom-label');

// Storage is a convenience: a private window, or a browser that refuses it, still gets a page.
const store = {
  get(key) {
    try { return localStorage.getItem(key); } catch { return null; }
  },
  set(key, value) {
    try {
      if (value === null) localStorage.removeItem(key); else localStorage.setItem(key, String(value));
    } catch { /* not kept */ }
  },
};

// 100% is one panel pixel to one CSS pixel, as the Mac window draws it. The chosen zoom is a
// ceiling: a column too small for it shows the panel smaller, and says so.
const zoomSteps = [0.25, 0.33, 0.5, 0.67, 0.75, 1, 1.5, 2, 3, 4];
let zoom = 1;
let fitMode = false; // follow the column instead of a chosen zoom
let shownScale = 1; // what the panel is drawn at now

{
  const saved = store.get('preflash.zoom');
  if (saved === 'fit') fitMode = true;
  else if (zoomSteps.includes(Number(saved))) zoom = Number(saved);
}

const bezel = 20; // the stage's padding on both sides
const colGap = 10; // between the head, the stage and the strip
const minDevice = 200; // the device side is never squeezed below this
const minTermCols = 40;
const minTermRows = 10;

// The terminal's cell, measured from its font, so the split can leave it enough columns.
const cell = (() => {
  const ctx = document.createElement('canvas').getContext('2d');
  ctx.font = `${term.options.fontSize}px ${term.options.fontFamily}`;
  return { w: ctx.measureText('M').width || 8, h: Math.ceil(term.options.fontSize * 1.3) };
})();

function contentBox() {
  const cs = getComputedStyle(main);
  return {
    w: main.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight),
    h: main.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom),
  };
}

// The panel as shown: its own size, turned by the board's rotation.
function panelShape() {
  const w = state.width || 480;
  const h = state.height || 480;
  const turn = (((state.board_rotation || 0) % 360) + 360) % 360;
  const sideways = turn === 90 || turn === 270;
  return { w, h, turn, shownW: sideways ? h : w, shownH: sideways ? w : h };
}

// The most the device side may take along the split, leaving the terminal its minimum. The
// scrollbar and the column's insets are allowed for.
function maxDevice(content, wide) {
  const gaps = 2 * parseFloat(getComputedStyle(main).columnGap || 12);
  if (wide) return content.w - gaps - Math.ceil(minTermCols * cell.w) - 32;
  return content.h - gaps - keysRow.offsetHeight - minTermRows * cell.h - 12;
}

// What the device side needs, along the split, to show the panel at the chosen zoom.
function deviceNeed(shape, wide) {
  const z = fitMode ? 1 : zoom;
  if (wide) return Math.max(shape.shownW * z + bezel, zoomOut.parentElement.offsetWidth);
  return shape.shownH * z + bezel + deviceHead.offsetHeight + strip.offsetHeight + 2 * colGap;
}

// deviceSize is the device side's size along the split: what the panel needs, within limits.
function deviceSize(content, wide, shape) {
  const axis = wide ? content.w : content.h;
  const size = Math.min(deviceNeed(shape, wide), axis * 0.6);
  return Math.max(Math.min(size, maxDevice(content, wide)), minDevice);
}

// layout sizes the device side, then draws the panel at the chosen zoom or smaller, to fit.
function layout() {
  const content = contentBox();
  const wide = wideQuery.matches;
  const shape = panelShape();
  const size = Math.round(deviceSize(content, wide, shape));
  deviceCol.style.width = wide ? `${size}px` : '';
  deviceCol.style.height = wide ? '' : `${size}px`;

  const roomW = (wide ? size : content.w) - bezel;
  const roomH = (wide ? content.h : size) - deviceHead.offsetHeight - strip.offsetHeight - 2 * colGap - bezel;
  const fitScale = Math.max(Math.min(roomW / shape.shownW, roomH / shape.shownH), 0.05);
  const scale = fitMode ? fitScale : Math.min(zoom, fitScale);
  shownScale = scale;
  screen.style.width = `${Math.round(shape.shownW * scale)}px`;
  screen.style.height = `${Math.round(shape.shownH * scale)}px`;
  panel.style.width = `${shape.w * scale}px`;
  panel.style.height = `${shape.h * scale}px`;
  panel.style.transform = `translate(-50%, -50%) rotate(${shape.turn}deg)`;
  panel.classList.toggle('pixelated', scale >= 2);

  const fitted = fitMode || scale < zoom - 0.001;
  zoomLabel.textContent = `${Math.round(scale * 100)}%${fitted ? ' · fit' : ''}`;
  zoomFit.setAttribute('aria-pressed', String(fitMode));
  zoomIn.disabled = !fitMode && zoom >= zoomSteps[zoomSteps.length - 1];
  zoomOut.disabled = scale <= zoomSteps[0] + 0.001;
}

function setZoom(next, fit) {
  zoom = next;
  fitMode = fit;
  store.set('preflash.zoom', fit ? 'fit' : zoom);
  layout();
}

// + goes up from the chosen zoom (or from the fitted size), − down from what is shown, so each
// press makes a visible step where the column allows.
function stepZoom(dir) {
  const eps = 0.001;
  const next = dir > 0
    ? zoomSteps.find((z) => z > (fitMode ? shownScale : zoom) + eps)
    : [...zoomSteps].reverse().find((z) => z < shownScale - eps);
  if (next !== undefined) setZoom(next, false);
}

for (const [button, act] of [
  [zoomOut, () => stepZoom(-1)],
  [zoomIn, () => stepZoom(1)],
  [zoomFit, () => setZoom(zoom, !fitMode)],
]) {
  // As with the key row, a tap mustn't take the focus, and the iPad's keyboard, from the terminal.
  button.addEventListener('pointerdown', (e) => e.preventDefault());
  button.addEventListener('mousedown', (e) => e.preventDefault());
  button.addEventListener('click', act);
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

// A tab that wakes drops a socket that went silent while it slept, and doesn't wait out its
// backoff.
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
