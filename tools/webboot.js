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

/* index.html hides TextDecoder over the module's load, because Chrome refuses
 * to decode from the resizable ArrayBuffer WebAssembly.Memory hands back. Do
 * the same here, or this harness is not loading the module the way the page
 * does - and the difference is exactly what hid a build that could not run in
 * a browser at all. */
const RealTextDecoder = global.TextDecoder;
if (!/delete self\.TextDecoder/.test(
        fs.readFileSync(repo + '/web/index.html', 'utf8'))) {
  console.error('FAIL: index.html no longer hides TextDecoder over ds10.js');
  process.exit(1);
}
delete global.TextDecoder;

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
global.window = {
  confirm: () => true,
  addEventListener(ev, fn) { (listeners.window = listeners.window || {})[ev] = fn; },
};
global.navigator = {userAgent: 'webboot'};
global.fetch = () => Promise.resolve({ok: true});
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

/* Put back a TextDecoder that refuses to work. The module was loaded with
 * the global hidden, so it decodes UTF-8 itself; if any string path still
 * reaches for a decoder this turns the browser's restriction into a failure
 * here, where it can be seen. */
let decoderCalls = 0;
global.TextDecoder = class {
  decode() {
    decoderCalls++;
    throw new TypeError("The provided ArrayBuffer value must not be resizable");
  }
};
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
  /* And exercise the string paths: page names in, knob and page names out. */
  try {
    const page = M.cwrap('ds10_web_page', 'number', ['string', 'string']);
    const mapHit = M.cwrap('ds10_web_map_hit', 'string', ['number', 'number']);
    const knobName = M.cwrap('ds10_web_knob_name', 'string', ['number']);
    for (const p of ['61_SYN_EDIT', '30_MIXER', '60_SYN_SEQ_PIANO'])
      page(p, '90_MAP');
    if (mapHit(30, 33) !== '60_SYN_SEQ_PIANO')
      throw new Error('a string came back wrong: ' + mapHit(30, 33));
    if (!knobName(11)) throw new Error('an empty knob name');
  } catch (e) {
    console.error('FAIL: a string across the boundary threw');
    console.error('  ' + (e && e.message ? e.message : e));
    process.exit(1);
  }
  if (decoderCalls) {
    console.error('FAIL: the module called TextDecoder ' + decoderCalls
                  + ' times; Chrome would have thrown');
    process.exit(1);
  }
  console.log('PASS: the page starts up, no TextDecoder, status '
              + JSON.stringify(status));
  /* The page's audio timer would otherwise keep node alive for ever. */
  process.exit(0);
};
M.onRuntimeInitialized = boot;

process.on('exit', () => {
  if (!ran) {
    console.error('FAIL: onRuntimeInitialized never fired');
    process.exitCode = 1;
  }
});
