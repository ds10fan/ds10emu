/* webaudio - run the AudioWorklet processor outside a browser.
 *
 *   make web
 *   node tools/webaudio.js
 *
 * The worklet is the only thing that makes sound, and nothing was testing it:
 * webedit drives the page's copy of the engine, which is not the copy you
 * hear. The first version silently did nothing at all, because it allocated a
 * buffer for the incoming song with cwrap("malloc") and the audio build
 * exports no allocator - every song message threw inside the audio thread,
 * where no log could see it, and the sequencer simply never played.
 *
 * This stubs the two globals a worklet gets, loads the real worklet.js, and
 * puts a song through it.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const repo = path.resolve(__dirname, '..');
const dir = repo + '/web/build';

let fails = 0;
function check(ok, what) {
  if (!ok) { console.error('FAIL: ' + what); fails++; }
}

let processor = null;
global.AudioWorkletProcessor = class {
  constructor() {
    const self = this;
    this.port = {
      postMessage(m) { (self._out = self._out || []).push(m); },
      set onmessage(fn) { self._in = fn; },
      get onmessage() { return self._in; },
    };
  }
};
global.registerProcessor = (_name, cls) => { processor = cls; };
global.sampleRate = 32768;

(async () => {
  process.chdir(dir);
  await import(dir + '/worklet.js');
  check(!!processor, 'worklet.js registered a processor');
  if (!processor) process.exit(1);

  const p = new processor();
  /* The module loads asynchronously; messages before that are queued. */
  const song = fs.readFileSync(repo + '/data/nitrofs/demo/ds10pdemo00.dat');
  p.port.onmessage({data: {
    type: 'song', bytes: new Uint8Array(song),
    pattern: 0, track: 0, songMode: true, playing: true,
  }});

  for (let i = 0; i < 200 && !p.api; i++)
    await new Promise((r) => setTimeout(r, 25));
  check(!!p.api, 'the audio module came up inside the worklet');
  if (!p.api) {
    console.error('  it reported: ' + JSON.stringify(p._out));
    process.exit(1);
  }
  const err = (p._out || []).find((m) => m.type === 'error');
  check(!err, 'no error from the worklet: ' + JSON.stringify(err));

  /* Pull audio the way the browser does, 128 frames at a time. */
  const out = [new Float32Array(128), new Float32Array(128)];
  let peak = 0, cursors = new Set();
  for (let block = 0; block < 400; block++) {
    p.process([], [out]);
    for (let i = 0; i < 128; i++)
      peak = Math.max(peak, Math.abs(out[0][i]), Math.abs(out[1][i]));
    for (const m of p._out || [])
      if (m.type === 'cursor') cursors.add(m.step);
  }
  check(peak > 0.01, 'the worklet makes a sound, peak ' + peak.toFixed(3));
  check(cursors.size >= 4,
        'the transport advances, saw ' + cursors.size + ' steps');

  /* And stops when told to. Not instantly: the drums are one-second buffers
   * and the effect has a delay line, so what has to be true is that it decays
   * rather than that it cuts. */
  p.port.onmessage({data: {type: 'play', on: false}});
  const tail = [];
  for (let w = 0; w < 6; w++) {
    let hi = 0;
    for (let block = 0; block < 128; block++) {     /* ~0.5 s per window */
      p.process([], [out]);
      for (let i = 0; i < 128; i++)
        hi = Math.max(hi, Math.abs(out[0][i]), Math.abs(out[1][i]));
    }
    tail.push(hi);
  }
  check(tail[tail.length - 1] < peak / 20,
        'and decays to silence: ' + tail.map((v) => v.toFixed(3)).join(' -> ')
        + ' from a peak of ' + peak.toFixed(3));

  if (fails) { console.error('FAIL: ' + fails + ' checks'); process.exit(1); }
  console.log('PASS: the worklet loads a song, plays it and stops');
  process.exit(0);
})().catch((e) => {
  console.error('FAIL: ' + (e && e.stack ? e.stack.split('\n').slice(0, 3)
                                              .join('\n') : e));
  process.exit(1);
});
