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
    mixerSet: c('ds10_web_mixer_set', null, ['number', 'number']),
    mixerGet: c('ds10_web_mixer_get', 'number', ['number']),
    seqHit: c('ds10_web_seq_hit', 'number', ['number', 'number']),
    setTrack: c('ds10_web_set_track', null, ['number']),
    setPattern: c('ds10_web_set_pattern', null, ['number']),
    patternClear: c('ds10_web_pattern_clear', null, ['number']),
    factoryPatch: c('ds10_web_factory_patch', 'number', ['number']),
    mapHit: c('ds10_web_map_hit', 'string', ['number', 'number']),
    mapSynth: c('ds10_web_map_synth', 'number', ['number', 'number']),
    patternHit: c('ds10_web_pattern_hit', 'number', ['number', 'number']),
    mapButtonHit: c('ds10_web_mapbutton_hit', 'number', ['number', 'number']),
    clearHit: c('ds10_web_clear_hit', 'number', ['number', 'number']),
    laneKnob: c('ds10_web_lane_knob', 'number', ['number']),
    pattern: c('ds10_web_pattern', 'number', []),
    track: c('ds10_web_track', 'number', []),
    seqMode: c('ds10_web_seq_mode', 'number', []),
    seqModeNext: c('ds10_web_seq_mode_next', 'string', []),
    seqModePage: c('ds10_web_seq_mode_page', 'string', []),
    seqModeHit: c('ds10_web_seq_mode_hit', 'number', ['number', 'number']),
    stepValue: c('ds10_web_step_value', 'number', ['number', 'number']),
    setStepRow: c('ds10_web_set_step_row', null, ['number', 'number']),
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

  /* The map's per-synth pairs. Half its boxes come in twos, and which of the
   * pair was touched is what picks the track the page then edits. */
  {
    const box = (n, x, y) => {
      check(api.mapHit(x, y) === n, 'the map box at ' + x + ',' + y + ' is '
            + n + ', got ' + api.mapHit(x, y));
      return api.mapSynth(x, y);
    };
    check(box('60_SYN_SEQ_PIANO', 30, 33) === 0, 'SYN1 SEQ is synth 1');
    check(box('60_SYN_SEQ_PIANO', 30, 95) === 1, 'SYN2 SEQ is synth 2');
    check(box('61_SYN_EDIT', 140, 33) === 0, 'SYN1 EDIT is synth 1');
    check(box('61_SYN_EDIT', 140, 95) === 1, 'SYN2 EDIT is synth 2');
    check(box('70_PATCH', 157, 62) === 0, 'SYN1 PATCH is synth 1');
    check(box('70_PATCH', 157, 124) === 1, 'SYN2 PATCH is synth 2');
    check(box('30_MIXER', 221, 33) === -1, 'the mixer is not per-synth');
    check(box('51_ALL_DRUMS', 34, 157) === -1, 'nor the drums');
    check(api.mapHit(100, 180) === '', 'and a gap opens nothing');
  }

  /* 32_PATTERN's sixteen buttons, and the marker that follows the selected one. */
  {
    check(api.patternHit(30, 40) === 0, 'the first pattern button');
    check(api.patternHit(100, 40) === 1, 'the second');
    check(api.patternHit(230, 40) === 3, 'the fourth');
    check(api.patternHit(30, 70) === 4, 'the second row');
    check(api.patternHit(230, 140) === 15, 'the last');
    check(api.patternHit(30, 10) === -1, 'above the grid is nothing');
    check(api.patternHit(30, 170) === -1, 'below it too');

    api.page('32_PATTERN', '90_MAP');
    const first = screen();
    api.setPattern(api.patternHit(100, 70));
    check(api.pattern() === 5, 'tapping the fifth box selects pattern 6');
    check(Buffer.compare(Buffer.from(screen()),
                         Buffer.from(first)) !== 0, 'and the marker moves');
    api.setPattern(0);
    check(Buffer.compare(Buffer.from(screen()),
                         Buffer.from(first)) === 0, 'and comes back');
    api.page('60_SYN_SEQ_PIANO', '90_MAP');
  }

  /* The value sub-modes. The page's own button steps them, and each one edits
   * a different byte of the step through the same grid. */
  {
    const NAMES = ["60_SYN_SEQ_PIANO", "60_SYN_SEQ_GATE", "60_SYN_SEQ_VOL",
                   "60_SYN_SEQ_PAN", "60_SYN_SEQ_KAOSS", "60_SYN_SEQ_KAOSS"];
    check(api.seqMode() === 0, 'starts on the note grid');
    for (let m = 1; m < 6; m++) {
      const page = api.seqModeNext();
      check(api.seqMode() === m, 'the button steps to mode ' + m);
      check(page === NAMES[m], 'mode ' + m + ' is ' + NAMES[m] + ', got ' + page);
    }
    check(api.seqModeNext() === NAMES[0], 'and wraps back to the note grid');
    check(api.seqModeHit(46, 8) !== 0, 'the button is where it was found');
    /* The other two the probe found on that bar, and the gaps between. */
    check(api.mapButtonHit(240, 8) !== 0, 'the map button');
    check(api.mapButtonHit(240, 15) !== 0, 'on both rows');
    check(api.mapButtonHit(220, 8) === 0, 'and not where nothing responded');
    check(api.clearHit(140, 8) !== 0, 'the clear button');
    check(api.clearHit(128, 8) === 0, 'and not left of it');
    check(api.seqModeHit(140, 8) === 0, 'the two do not overlap');
    check(api.mapButtonHit(140, 8) === 0, 'nor those two');
    /* The lanes name the knobs the pad drives. */
    check(api.laneKnob(0) >= 0 && api.laneKnob(0) < 31, 'lane 0 names a knob');
    check(api.laneKnob(1) >= 0 && api.laneKnob(1) < 31, 'lane 1 names a knob');
    check(api.seqModeHit(20, 8) === 0, 'and not where nothing responded');

    /* Gate: five rows, LEGATO down to 25%, so row 2 is the default 3. */
    api.seqModeNext();                    /* to GATE */
    api.page(api.seqModePage(), '90_MAP');
    check(api.stepValue(0, MELODY[0][0]) === 3, 'the default gate shows');
    const noGate = screen();
    api.setStepRow(MELODY[0][0], 0);      /* LEGATO */
    check(api.stepValue(0, MELODY[0][0]) === 5, 'LEGATO is gate 5');
    check(Buffer.compare(Buffer.from(screen()),
                         Buffer.from(noGate)) !== 0, 'and the screen moves');
    api.setStepRow(MELODY[0][0], 2);      /* back to 75% */
    check(api.stepValue(0, MELODY[0][0]) === 3, 'and back');

    /* Velocity: twelve rows, and every value the ROM's songs use. */
    api.seqModeNext();
    api.page(api.seqModePage(), '90_MAP');
    const WANT = [127, 115, 103, 92, 80, 69, 57, 46, 34, 23, 11, 0];
    for (let r = 0; r < 12; r++) {
      api.setStepRow(MELODY[0][0], r);
      check(api.stepValue(0, MELODY[0][0]) === WANT[r],
            'row ' + r + ' is velocity ' + WANT[r] + ', got '
            + api.stepValue(0, MELODY[0][0]));
    }
    api.setStepRow(MELODY[0][0], 0);

    /* Pan: five rows with the centre on the default. */
    api.seqModeNext();
    api.page(api.seqModePage(), '90_MAP');
    api.setStepRow(MELODY[0][0], 2);
    check(api.stepValue(0, MELODY[0][0]) === 64, 'the centre row is pan 64');
    api.setStepRow(MELODY[0][0], 0);
    check(api.stepValue(0, MELODY[0][0]) === 0, 'L2 is pan 0');
    api.setStepRow(MELODY[0][0], 4);
    check(api.stepValue(0, MELODY[0][0]) === 127, 'R2 is pan 127');
    api.setStepRow(MELODY[0][0], 2);

    /* A rest has no value marker, which is what the captures show. */
    check(api.stepValue(0, 1) === -1, 'a rest has no value to draw');

    while (api.seqMode()) api.seqModeNext();
    api.page('60_SYN_SEQ_PIANO', '90_MAP');
  }

  /* Drums, on their own tracks and their own page. Those markers are on the
   * 3D plane rather than in OAM, so this is a separate path through the
   * renderer and worth its own check. */
  check(api.page('51_ALL_DRUMS', '90_MAP') === 0, 'the drum page opens');
  const noDrums = screen();
  for (let s = 0; s < 16; s += 4) api.seqStep(2, s, 60);
  check(api.seqNote(2, 0) === 60, 'a drum step is set');
  const withDrums = screen();
  let lit = 0;
  for (let r = 0; r < 4; r++)
    for (let s = 0; s < 16; s++)
      if (cellInk(withDrums, s, r) !== cellInk(noDrums, s, r)) lit++;
  check(lit === 4, 'four drum cells lit, got ' + lit);
  for (let s = 0; s < 16; s += 4)
    check(cellInk(withDrums, s, 0) > cellInk(noDrums, s, 0),
          'drum 1 step ' + s + ' shows');
  api.page('60_SYN_SEQ_PIANO', '90_MAP');

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

  /* A factory patch has to land in the pattern, not only in the voice, or the
   * next bar line pushes the pattern's own patch back over it. */
  {
    const before = api.knobGet(11);
    let moved = false;
    for (let n = 0; n < 8 && !moved; n++) {
      api.factoryPatch(n);
      if (api.knobGet(11) !== before) moved = true;
    }
    check(moved, 'a factory patch reaches the pattern');
  }
  api.song('ds10init');
  api.page('60_SYN_SEQ_PIANO', '90_MAP');
  for (const [s, r] of MELODY) api.seqStep(0, s, api.rowNote(r));

  /* The mixer's faders follow the stored volumes. The handle is the brightest
   * thing in the channel strip, so where its row of ink is says where it is. */
  api.page('30_MIXER', '90_MAP');
  function handleRow(ch) {
    const fb = screen();
    let best = -1, bestInk = 0;
    for (let y = 80; y < 190; y++) {
      let ink = 0;
      for (let x = 10 + 43 * ch; x < 10 + 43 * ch + 32; x++) {
        const i = (y * 256 + x) * 4;
        ink += fb[i] + fb[i + 1] + fb[i + 2];
      }
      if (ink > bestInk) { bestInk = ink; best = y; }
    }
    return best;
  }
  api.mixerSet(17, 127);
  const top = handleRow(0);
  api.mixerSet(17, 0);
  const bottom = handleRow(0);
  check(bottom > top + 40, 'the fader moves down as the volume falls: '
        + top + ' -> ' + bottom);
  api.mixerSet(17, 127);
  check(handleRow(0) === top, 'and comes back');

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

  /* The cursor has to move on the screen, not just in the state - that is
   * the feedback you enter notes against. Find the brightest column in the
   * band above the grid and watch it walk. */
  function cursorColumn() {
    const fb = screen();
    let best = -1, bestInk = -1;
    for (let s = 0; s < 16; s++) {
      let ink = 0;
      for (let y = 18; y < 30; y++)
        for (let x = X0 + s * CELL; x < X0 + s * CELL + CELL; x++) {
          const i = (y * 256 + x) * 4;
          ink += fb[i] + fb[i + 1] + fb[i + 2];
        }
      if (ink > bestInk) { bestInk = ink; best = s; }
    }
    return best;
  }
  {
    const seen = new Set();
    for (let i = 0; i < 24; i++) {
      seen.add(cursorColumn());
      peak(1);
    }
    check(seen.size >= 4,
          'the cursor walks the grid, saw ' + seen.size + ' columns');
    check(cursorColumn() === api.cursor(),
          'the drawn cursor is on the step the transport reports');
  }

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

  const repo = require('path').resolve(__dirname, '..');
  fs.writeFileSync(repo + '/out/webedit.dat', file);

  /* Load what was just written back through the game's own parser and play
   * it, then render the same song with songplay - which has nothing to do
   * with the browser build and is itself bit-exact against the original. If
   * the two agree sample for sample, the file is a real song and not just
   * bytes that happen to pass a hash. */
  M.FS.writeFile('/data/nitrofs/demo/webedit.dat', file);
  check(api.song('webedit') === 0, 'the exported file loads back');
  check(api.songBars() === 2, 'and still has two bars');
  for (const [s, r] of MELODY)
    check(api.seqNote(0, s) === api.rowNote(r), 'step ' + s + ' survived');
  check(api.getName() === 'WEBED', 'the name survived');

  const mine = [];
  for (let b = 0; b < 40; b++) {
    const k = api.audioRender(2048);
    const f = new Float32Array(M.HEAPU8.buffer, api.audioBuf(), k * 2);
    for (let i = 0; i < k * 2; i++) mine.push(Math.round(f[i] * 32768));
  }

  const run = require('child_process').spawnSync(
    repo + '/out/songplay',
    ['-b', '2', '-o', repo + '/out/webedit_song.wav', repo + '/out/webedit.dat'],
    {cwd: repo});   /* it reads data/tables.bin relative to where it runs */
  check(run.status === 0, 'songplay renders the exported file: '
        + (run.stderr || '').toString().trim());
  if (run.status === 0) {
    const wav = fs.readFileSync(repo + '/out/webedit_song.wav');
    const ref = new Int16Array(wav.buffer, wav.byteOffset + 44,
                               (wav.length - 44) >> 1);
    const len = Math.min(ref.length, mine.length);
    let bad = 0;
    for (let i = 0; i < len; i++) if (ref[i] !== mine[i]) bad++;
    check(len > 40000, 'songplay produced a whole song, ' + len + ' samples');
    check(bad === 0, bad + ' of ' + len + ' samples differ from songplay');
  }

  if (fails) {
    console.error('FAIL: ' + fails + ' checks');
    process.exit(1);
  }
  console.log('PASS: entered a song in the browser build, wrote ' + n
              + ' bytes, and songplay renders it identically');
};
