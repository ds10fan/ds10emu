/* websong - check the browser build against songplay, sample for sample.
 *
 *   make web
 *   ./out/songplay -b 3 -o out/ref_demo.wav data/nitrofs/demo/ds10demo01.dat
 *   node tools/websong.js
 *
 * Both sides run the same decompiled engine, so a difference is a difference
 * in how the browser build sets it up - which is the kind of mistake that
 * stays invisible until someone listens. Getting this to zero turned up two:
 * the tempo broadcast has to be dispatched, not just posted, and the voices
 * have to be back at their constructor state before a song's patches load,
 * because the knob handler's side effects depend on what was there before.
 */
'use strict';
const fs = require('fs');
const path = '/home/ai/p/ds10/web/build';
process.chdir(path);
const M = require(path + '/ds10.js');

M.onRuntimeInitialized = () => {
  const c = (n, r, a) => M.cwrap(n, r, a);
  c('ds10_web_init', 'number', [])();
  if (c('ds10_web_song', 'number', ['string'])('ds10demo01') < 0) {
    console.error('cannot load the song');
    process.exit(2);
  }
  const render = c('ds10_web_audio', 'number', ['number']);
  const buf = c('ds10_web_audio_buf', 'number', []);
  const out = [];
  for (let b = 0; b < 60; b++) {
    const k = render(2048);
    const f = new Float32Array(M.HEAPU8.buffer, buf(), k * 2);
    for (let i = 0; i < k * 2; i++) out.push(Math.round(f[i] * 32768));
  }
  const got = Int16Array.from(out);

  const wav = fs.readFileSync('/home/ai/p/ds10/out/ref_demo.wav');
  const ref = new Int16Array(wav.buffer, wav.byteOffset + 44,
                             (wav.length - 44) >> 1);
  const n = Math.min(ref.length, got.length);
  let bad = 0, first = -1;
  for (let i = 0; i < n; i++)
    if (ref[i] !== got[i]) { if (first < 0) first = i; bad++; }
  console.log((bad ? 'FAIL' : 'PASS') + ': ' + n + ' samples, ' + bad +
              ' differ' + (bad ? ' (first at ' + first + ')' : ''));
  process.exit(bad ? 1 : 0);
};
