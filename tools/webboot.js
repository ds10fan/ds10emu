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
  const kids = [];
  let text = '';
  return {
    tag: id,
    id: id,
    value: '',
    /* Setting textContent removes the children, as it does in a browser.
     * Without that a section that rebuilds itself appears never to, and the
     * old controls stay in the tree. */
    get textContent() { return text; },
    set textContent(v) { text = v; kids.length = 0; },
    files: [],
    style: {},
    dataset: {},
    width: 256,
    height: 192,
    children: kids,
    classList: {add() {}, remove() {}, toggle() {}},
    setAttribute(k, v) { if (k === "value") this.value = v; },
    appendChild(c) { kids.push(c); return c; },
    _on: {},
    dispatch(ev, e) {
      for (const fn of (this._on[ev] || [])) fn(e || {target: this});
    },
    addEventListener(ev, fn) {
      (listeners[id] = listeners[id] || {})[ev] = fn;
      (this._on[ev] = this._on[ev] || []).push(fn);
    },
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
  /* app.js and panel.js share a scope in the browser, so concatenate them -
   * the panel is generated at run time, and a missing entry point in it only
   * shows when it is built. */
  const src = fs.readFileSync(repo + '/web/saves.js', 'utf8') + '\n'
            + fs.readFileSync(repo + '/web/app.js', 'utf8') + '\n'
            + fs.readFileSync(repo + '/web/panel.js', 'utf8')
            /* The three share a scope in the browser; reach into it. */
            + '\nglobalThis.__saved = typeof SAVED_SONGS !== "undefined" '
            + '? SAVED_SONGS : [];\n';
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
  /* The panel is generated, so a missing entry point only shows when it
   * runs. Count what it built, or a stub DOM that silently swallows the
   * whole thing would pass. */
  const panel = els.panel ? els.panel.children.length : 0;
  if (panel < 10) {
    console.error('FAIL: the panel built ' + panel + ' sections, expected 10');
    process.exit(1);
  }

  /* Exercise the controls the panel generates. A section that builds but does
   * nothing looks the same as one that works. */
  const all = [];
  (function walk(e) {
    all.push(e);
    for (const c of e.children || []) walk(c);
  })(els.panel);
  const byText = (t) => all.find((e) => e.textContent === t && e._on.click);
  const fails2 = [];
  const cw = (n, r, a) => M.cwrap(n, r, a);
  const mixerGet = cw('ds10_web_mixer_get', 'number', ['number']);
  const laneKnob = cw('ds10_web_lane_knob', 'number', ['number']);

  for (const [label, id, want] of [['SYN 1', 0, 0], ['DRUMS', 0, 3],
                                   ['OFF', 0, 5], ['FLANGER', 1, 1],
                                   ['CHORUS', 1, 2], ['DELAY', 1, 0]]) {
    const b = byText(label);
    if (!b) { fails2.push('no button "' + label + '"'); continue; }
    b.dispatch('click');
    if (mixerGet(id) !== want)
      fails2.push(label + ' set mixer ' + id + ' to ' + mixerGet(id)
                  + ', wanted ' + want);
  }
  /* The two KAOSS lane pickers. */
  const sels = all.filter((e) => e.tag === '<select>' && e._on.change);
  const lane = sels.filter((e) => e.children.length === 31);
  if (lane.length < 2) fails2.push('the kaoss lane pickers are missing');
  else {
    lane[0].dispatch('change', {target: {value: '11'}});
    lane[1].dispatch('change', {target: {value: '12'}});
    if (laneKnob(0) !== 11 || laneKnob(1) !== 12)
      fails2.push('the lane knobs did not take: ' + laneKnob(0) + '/'
                  + laneKnob(1));
  }
  if (fails2.length) {
    for (const f of fails2) console.error('FAIL: ' + f);
    process.exit(1);
  }
  /* One of the saved songs must load, or the whole songs/ directory is
   * listed and unreachable. */
  try {
    const song = cw('ds10_web_song', 'number', ['string']);
    const getName = cw('ds10_web_get_name', 'string', []);
    const saved = globalThis.__saved || [];
    if (!saved.length) throw new Error('no saved songs listed');
    if (song(saved[0][0]) !== 0)
      throw new Error('cannot load ' + saved[0][0]);
    console.log('  saves: ' + saved.length + ' listed, first is '
                + JSON.stringify(getName()));
  } catch (e) {
    console.error('FAIL: the saved songs: ' + (e.message || e));
    process.exit(1);
  }
  /* SYNC is a switch and belongs to the delay alone. */
  {
    const mixerGet2 = cw('ds10_web_mixer_get', 'number', ['number']);
    const all2 = [];
    const walk2 = (e) => { all2.push(e); for (const c of e.children || []) walk2(c); };
    const findSync = () => {
      all2.length = 0;
      walk2(els.panel);
      return all2.find((e) => /^SYNC/.test(e.textContent || '') && e._on.click);
    };
    byText('DELAY').dispatch('click');
    const sw = findSync();
    if (!sw) { console.error('FAIL: no SYNC switch on the delay'); process.exit(1); }
    const was = mixerGet2(6);
    sw.dispatch('click');
    if (mixerGet2(6) === was) {
      console.error('FAIL: the SYNC switch did not toggle');
      process.exit(1);
    }
    if (mixerGet2(6) !== 0 && mixerGet2(6) !== 1) {
      console.error('FAIL: SYNC took ' + mixerGet2(6) + ', wanted 0 or 1');
      process.exit(1);
    }
    byText('CHORUS').dispatch('click');
    if (findSync()) {
      console.error('FAIL: SYNC is showing on the chorus');
      process.exit(1);
    }
    byText('DELAY').dispatch('click');
  }
  console.log('  panel: ' + panel + ' sections, route/type/lanes all respond');
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
