/* webboot - run web/app.js against the real module with a stubbed DOM.
 *
 *   make web
 *   node tools/webboot.js
 *
 * The page can fail in ways no other test sees: tools/webedit.js drives the
 * wasm directly and never parses app.js, so a throw in the page's own startup
 * leaves both canvases black and the status stuck on "loading" with nothing
 * to show for it. This runs the actual file the browser runs, with just
 * enough of a DOM under it, and reports the first thing that goes wrong.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const repo = path.resolve(__dirname, '..');
const dir = repo + '/web/build';

/* The elements index.html declares, so a missing one is an error here rather
 * than a null dereference. */
const IDS = [];
for (const m of fs.readFileSync(repo + '/web/index.html', 'utf8')
                  .matchAll(/id="([^"]+)"/g))
  IDS.push(m[1]);

const listeners = {};
function element(id) {
  return {
    id: id,
    value: '',
    textContent: '',
    files: [],
    style: {},
    width: 256,
    height: 192,
    classList: {add() {}, remove() {}, toggle() {}},
    appendChild() {},
    addEventListener(ev, fn) { (listeners[id] = listeners[id] || {})[ev] = fn; },
    getContext() {
      return {putImageData() {}, createImageData() { return {}; }};
    },
    getBoundingClientRect() {
      return {left: 0, top: 0, width: 512, height: 384};
    },
    setPointerCapture() {},
  };
}

const els = {};
for (const id of IDS) els[id] = element(id);

/* The module decides web-versus-node from globalThis.window when it loads, so
 * nothing may define window until after it is required. */
process.chdir(dir);
const M = require(dir + '/ds10.js');

global.document = {
  getElementById(id) {
    if (!els[id]) throw new Error('no element with id "' + id + '"');
    return els[id];
  },
  createElement(tag) { return element('<' + tag + '>'); },
  addEventListener(ev, fn) { (listeners.document = listeners.document || {})[ev] = fn; },
};
global.window = {confirm: () => true};
global.ImageData = class { constructor(d, w, h) { this.data = d; this.width = w; this.height = h; } };
global.AudioContext = class {
  constructor() { this.currentTime = 0; this.state = 'running'; this.destination = {}; }
  createBuffer(ch, n) {
    const d = [new Float32Array(n), new Float32Array(n)];
    return {getChannelData: (i) => d[i]};
  }
  createBufferSource() { return {connect() {}, start() {}}; }
  resume() {}
};
global.requestAnimationFrame = () => 0;   /* one frame is enough */
global.URL = {createObjectURL: () => 'blob:', revokeObjectURL() {}};
global.Blob = class {};

global.Module = M;

let ran = false;
const boot = () => {
  ran = true;
  /* The page's own startup, verbatim. A throw here is what leaves the real
   * page black. */
  const src = fs.readFileSync(repo + '/web/app.js', 'utf8');
  const run = new Function(src);
  try {
    run();
    /* app.js sets its own onRuntimeInitialized; the runtime is already up, so
     * call it the way the browser would have. */
    if (typeof M.onRuntimeInitialized === 'function' &&
        M.onRuntimeInitialized !== boot)
      M.onRuntimeInitialized();
  } catch (e) {
    console.error('FAIL: app.js threw during startup');
    console.error('  ' + (e && e.stack ? e.stack.split('\n').slice(0, 4)
                                              .join('\n  ') : e));
    process.exit(1);
  }
  const status = els.status ? els.status.textContent : '';
  if (/^error|loading/i.test(status)) {
    console.error('FAIL: the page came up with status ' + JSON.stringify(status));
    process.exit(1);
  }
  /* And a few of the things a first touch would do. */
  try {
    const down = listeners.bottom && listeners.bottom.pointerdown;
    if (!down) throw new Error('nothing listening for pointerdown');
    down({target: els.bottom, clientX: 200, clientY: 200, pointerId: 1});
  } catch (e) {
    console.error('FAIL: a touch on the bottom screen threw');
    console.error('  ' + (e && e.stack ? e.stack.split('\n').slice(0, 3)
                                              .join('\n  ') : e));
    process.exit(1);
  }
  console.log('PASS: the page starts up, status ' + JSON.stringify(status));
};
M.onRuntimeInitialized = boot;

process.on('exit', () => {
  if (!ran) {
    console.error('FAIL: onRuntimeInitialized never fired');
    process.exitCode = 1;
  }
});
