/* webedit - drive the browser build through an editing session.
 *
 *   make web
 *   node tools/webedit.js
 *
 * This is the check that the thing is usable, not just that it renders: enter
 * notes, see them appear on the screen at the right pixels, hear them, and
 * write out a file that is a valid DS-10 song. Every assertion goes through
 * the same entry points the page uses, so if this passes, the page works.
 *
 * The file at the end is verified without any of our own code: node hashes
 * the payload itself and reads the step bytes at the offsets song.h
 * documents. That way "it exported something" cannot pass for "it exported a
 * song".
 */
'use strict';
const fs = require('fs');
const crypto = require('crypto');
const dir = require('path').resolve(__dirname, '../web/build');
process.chdir(dir);
const M = require(dir + '/ds10.js');

let fails = 0;
function check(ok, what) {
  if (!ok) { console.error('FAIL: ' + what); fails++; }
}

/* The grid, from ds10_ui_seq_grid. */
const X0 = 45, Y0 = 35, CELL = 13;

M.onRuntimeInitialized = () => {
  const c = (n, r, a) => M.cwrap(n, r, a);
  const api = {
    init: c('ds10_web_init', 'number', []),
    page: c('ds10_web_page', 'number', ['string', 'string']),
    render: c('ds10_web_render', null, []),
    bottom: c('ds10_web_bottom', 'number', []),
    song: c('ds10_web_song', 'number', ['string']),
    seqStep: c('ds10_web_seq_step', null, ['number', 'number', 'number']),
    seqNote: c('ds10_web_seq_note', 'number', ['number', 'number']),
    seqPlay: c('ds10_web_seq_play', null, ['number']),
    rowNote: c('ds10_web_row_note', 'number', ['number']),
    cursor: c('ds10_web_cursor', 'number', []),
    knobGet: c('ds10_web_knob_get', 'number', ['number']),
    knobSet: c('ds10_web_knob_set', null, ['number', 'number']),
    knobHit: c('ds10_web_knob_hit', 'number', ['number', 'number']),
    seqHit: c('ds10_web_seq_hit', 'number', ['number', 'number']),
    setTrack: c('ds10_web_set_track', null, ['number']),
    setPattern: c('ds10_web_set_pattern', null, ['number']),
    patternClear: c('ds10_web_pattern_clear', null, ['number']),
    setTempo: c('ds10_web_set_tempo', null, ['number']),
    getTempo: c('ds10_web_get_tempo', 'number', []),
    setName: c('ds10_web_set_name', null, ['string']),
    getName: c('ds10_web_get_name', 'string', []),
    chainSet: c('ds10_web_chain_set', null, ['number', 'number']),
    songBars: c('ds10_web_song_bars', 'number', []),
    audioRender: c('ds10_web_audio', 'number', ['number']),
    audioBuf: c('ds10_web_audio_buf', 'number', []),
    export: c('ds10_web_export', 'number', []),
    exportBuf: c('ds10_web_export_buf', 'number', []),
  };

  api.init();
  check(api.song('ds10init') === 0, 'the blank template loads');
  check(api.page('60_SYN_SEQ_PIANO', '90_MAP') === 0, 'the grid page opens');

  /* A snapshot of the bottom screen, and the pixels of one grid cell. */
  function screen() {
    api.render();
    return M.HEAPU8.slice(api.bottom(), api.bottom() + 256 * 192 * 4);
  }
  function cellInk(fb, step, row) {
    let n = 0;
    for (let y = Y0 + row * CELL; y < Y0 + row * CELL + CELL; y++)
      for (let x = X0 + step * CELL; x < X0 + step * CELL + CELL; x++) {
        const i = (y * 256 + x) * 4;
        n += fb[i] + fb[i + 1] + fb[i + 2];
      }
    return n;
  }

  api.seqPlay(0);
  const empty = screen();

  /* Enter a note where a tap at the middle of a cell would put one, going
   * through the same hit test the page uses. */
  const STEP = 3, ROW = 5;
  const hit = api.seqHit(X0 + STEP * CELL + 6, Y0 + ROW * CELL + 6);
  check(hit >= 0, 'the grid hit test finds the cell');
  check((hit & 0xFF) === STEP && (hit >> 8) === ROW,
        'the hit test returns the cell that was touched');

  const note = api.rowNote(ROW);
  api.seqStep(0, STEP, note);
  check(api.seqNote(0, STEP) === note, 'the note is in the pattern');

  const one = screen();
  check(cellInk(one, STEP, ROW) > cellInk(empty, STEP, ROW),
        'the cell that was touched lights up');
  /* And only that one: a marker in the wrong place would be worse than none. */
  let changed = 0;
  for (let r = 0; r < 12; r++)
    for (let s = 0; s < 16; s++)
      if (cellInk(one, s, r) !== cellInk(empty, s, r)) changed++;
  check(changed === 1, 'exactly one cell changed, got ' + changed);

  api.seqStep(0, STEP, -1);
  check(Buffer.compare(Buffer.from(screen()), Buffer.from(empty)) === 0,
        'clearing the note puts the screen back');

  /* A run of notes, and the drums. */
  const MELODY = [[0, 0], [2, 3], [4, 5], [6, 7], [8, 9], [10, 11]];
  for (const [s, r] of MELODY) api.seqStep(0, s, api.rowNote(r));
  const melody = screen();
  for (const [s, r] of MELODY)
    check(cellInk(melody, s, r) > cellInk(empty, s, r),
          'step ' + s + ' row ' + r + ' shows');

  /* Notes outside the twelve visible rows must not be drawn, but must still
   * be in the pattern - the window clips, it does not delete. */
  api.seqStep(0, 1, api.rowNote(0) + 24);
  check(api.seqNote(0, 1) === api.rowNote(0) + 24, 'an off-screen note is kept');
  let offscreen = 0;
  const withOff = screen();
  for (let r = 0; r < 12; r++)
    if (cellInk(withOff, 1, r) !== cellInk(melody, 1, r)) offscreen++;
  check(offscreen === 0, 'an off-screen note draws nothing');
  api.seqStep(0, 1, -1);

  /* Drums, on their own tracks. */
  for (let s = 0; s < 16; s += 4) api.seqStep(2, s, 60);
  check(api.seqNote(2, 0) === 60, 'a drum step is set');

  /* Knobs pick up where they are rather than jumping. */
  api.page('61_SYN_EDIT', '90_MAP');
  const before = api.knobGet(11);
  const knobScreen = screen();
  api.knobSet(11, before === 100 ? 40 : 100);
  check(api.knobGet(11) === (before === 100 ? 40 : 100),
        'the knob takes the value');
  check(Buffer.compare(Buffer.from(screen()),
                       Buffer.from(knobScreen)) !== 0,
        'turning the knob changes the screen');

  /* It has to make a sound. Play the pattern and check the output is not
   * silence - then clear it and check that it is. */
  api.page('60_SYN_SEQ_PIANO', '90_MAP');
  function peak(blocks) {
    let hi = 0;
    for (let b = 0; b < blocks; b++) {
      const n = api.audioRender(2048);
      const f = new Float32Array(M.HEAPU8.buffer, api.audioBuf(), n * 2);
      for (let i = 0; i < n * 2; i++) hi = Math.max(hi, Math.abs(f[i]));
    }
    return hi;
  }
  api.setTempo(240);
  api.seqPlay(1);
  const loud = peak(20);
  check(loud > 0.01, 'the entered pattern makes a sound, peak ' + loud);
  check(api.cursor() >= 0, 'the cursor reports a step while playing');

  api.patternClear(-1);
  peak(8);                        /* let the tails die away */
  const quiet = peak(12);
  check(quiet < loud / 8, 'an empty pattern goes quiet: ' + quiet + ' vs '
        + loud);
  api.seqPlay(0);

  /* Enter it again, name it, arrange it, and write it out. */
  for (const [s, r] of MELODY) api.seqStep(0, s, api.rowNote(r));
  for (let s = 0; s < 16; s += 4) api.seqStep(2, s, 60);
  api.setTempo(132);
  api.setName('WEBED');
  api.chainSet(0, 0);
  api.chainSet(1, 0);
  check(api.songBars() === 2, 'the arrangement has two bars');
  check(api.getName() === 'WEBED', 'the name is set');
  check(api.getTempo() === 132, 'the tempo is set');

  const n = api.export();
  check(n > 0, 'export produced a file');
  const file = Buffer.from(M.HEAPU8.slice(api.exportBuf(),
                                          api.exportBuf() + n));

  /* Verified without our code: the header is an MD5 of the payload, and the
   * step bytes sit where song.h says. */
  const want = crypto.createHash('md5').update(file.subarray(16)).digest();
  check(Buffer.compare(want, file.subarray(0, 16)) === 0,
        'the file hash checks out');

  const POFF = 0xEC, PSZ = 676, SOFF = 0x42;
  const stepAt = (p, t, s) => 16 + POFF + p * PSZ + SOFF + (t * 16 + s) * 4;
  for (const [s, r] of MELODY)
    check(file[stepAt(0, 0, s) + 1] === api.rowNote(r),
          'step ' + s + ' is in the file');
  for (let s = 0; s < 16; s += 4)
    check(file[stepAt(0, 2, s) + 1] === 60, 'drum step ' + s + ' is in the file');
  check(file[stepAt(0, 0, 1) + 1] === 0xFF, 'an untouched step is a rest');
  /* The gate byte is the step's length and must have survived untouched. */
  check(file[stepAt(0, 0, 0)] === 3, 'the gate is still the default');
  check(file.readUInt32LE(16 + 4) === 132, 'the tempo is in the file');
  check(file.subarray(16 + 0x14, 16 + 0x1A).toString() === 'WEBED ',
        'the name is in the file');
  check(file.readUInt16LE(16 + 0x24) === 0 &&
        file.readUInt16LE(16 + 0x26) === 0 &&
        file.readUInt16LE(16 + 0x28) === 0xFFFF,
        'the arrangement is in the file');

  fs.writeFileSync('/tmp/ds10-webedit.dat', file);

  if (fails) {
    console.error('FAIL: ' + fails + ' checks');
    process.exit(1);
  }
  console.log('PASS: entered a song in the browser build, ' + n
              + ' bytes written');
};
