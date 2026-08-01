/* Drives the WebAssembly build.
 *
 * The C side owns everything that matters: the song being edited, the synth,
 * and both framebuffers. This moves pointer events into it, moves pixels out
 * of it, and keeps a handful of HTML controls in step. Nothing about the song
 * is stored here - every read goes back to the C side, so what is on screen
 * and what would be written to a file cannot drift apart.
 */
"use strict";

/* The pages under /data/mainbg that are whole screens. Those whose sprite
 * layer is known reproduce the hardware exactly; the rest draw their
 * background only, which is why they look bare. */
const BOTTOM_PAGES = [
  "60_SYN_SEQ_PIANO", "81_KEY_i", "61_SYN_EDIT", "70_PATCH", "30_MIXER",
  "71_KAOSS", "52_FX", "32_PATTERN", "51_ALL_DRUMS", "53_DRUMEDIT",
  "54_DRUM_FX", "55_DRUMEDITMAP", "60_SYN_SEQ_GATE", "60_SYN_SEQ_PAN",
  "60_SYN_SEQ_VOL", "60_SYN_SEQ_KAOSS", "62_DRUM_SEQ_GATE",
  "63_DRUM_EDIT_PATCH", "33_DRUM_SELECTER", "01_MAIN_MENU", "15_OPTION",
  "31_SONG", "90_MAP",
];
/* The ROM's own songs. "ds10init" is its blank template, which is what a new
 * song starts from. */
const SONGS = [
  "ds10init", "ds10demo00", "ds10demo01", "ds10demo02", "ds10demo03",
  "ds10demo05", "ds10demo06", "ds10demo07",
  "ds10pdemo00", "ds10pdemo01", "ds10pdemo02", "ds10pdemo03",
];
const TOP_PAGES = [
  "90_MAP", "AD0_TITLE", "AD1_KORG", "AD2_PROCYONSTUDIO", "AD3_AQI",
  "AD4_CAVIA", "01_MAIN_MENU",
];
const TRACKS = ["SYN 1", "SYN 2", "DRUM 1", "DRUM 2", "DRUM 3", "DRUM 4"];
/* The sequencer's six sub-modes, in the order its button steps them. */
const MODES = ["NOTE", "GATE", "VOLUME", "PAN", "KAOSS 1", "KAOSS 2"];

const $ = (id) => document.getElementById(id);
const status = $("status");
const canvases = {top: $("top"), bottom: $("bottom")};
const ctx = {
  top: canvases.top.getContext("2d"),
  bottom: canvases.bottom.getContext("2d"),
};

let api = null;
let audio = null;      /* AudioContext, once the user has interacted */
let octave = 0;
let held = null;       /* the note currently sounding, if any */
let drag = null;       /* {kind, id, from, y} while something is being dragged */
let paint = null;      /* {track, note} while dragging across the step grid */
let kaoss = false;

function fill(select, names, initial) {
  for (const n of names) {
    const o = document.createElement("option");
    o.value = o.textContent = typeof n === "string" ? n : n[0];
    o.textContent = typeof n === "string" ? n : n[1];
    select.appendChild(o);
  }
  if (initial !== undefined) select.value = initial;
}

function blit(which, ptr, w, h) {
  const bytes = new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, w * h * 4);
  ctx[which].putImageData(new ImageData(bytes, w, h), 0, 0);
}

/* One frame: let the C side draw the live parts of the page over it, then
 * copy both screens out. This runs continuously, so the play cursor moves and
 * every edit shows up without anything here having to know what changed. */
function draw() {
  api.render();
  const w = api.width(), h = api.height();
  blit("top", api.top(), w, h);
  blit("bottom", api.bottom(), w, h);
}

function loop() {
  draw();
  requestAnimationFrame(loop);
}

function pageOf(which) {
  return $(which === "bottom" ? "pick-bottom" : "pick-top").value;
}

function show() {
  const bottom = pageOf("bottom"), top = pageOf("top");
  if (api.page(bottom, top) < 0) {
    status.textContent = "error: " + api.error();
    return;
  }
  say(bottom + "  /  " + top);
}

function say(s) { status.textContent = s; }

function goto(page) {
  $("pick-bottom").value = page;
  show();
}

/* Audio. The engine runs at 32768 Hz and the browser resamples. Blocks are
 * rendered ahead of the clock and queued, which is enough for a keyboard. */
function startAudio() {
  /* A context made before the user has touched anything comes up suspended,
   * so every entry point that can make a sound comes through here. */
  if (audio) {
    if (audio.state === "suspended") audio.resume();
    return;
  }
  if (api.audioInit() < 0) { say("error: " + api.error()); return; }
  audio = new AudioContext();
  const rate = api.rate();
  const BLOCK = 2048;
  let next = audio.currentTime;

  setInterval(() => {
    /* Keep about a fifth of a second queued. */
    while (next < audio.currentTime + 0.2) {
      const n = api.audioRender(BLOCK);
      if (!n) return;
      const src = new Float32Array(Module.HEAPU8.buffer, api.audioBuf(), n * 2);
      const b = audio.createBuffer(2, n, rate);
      const l = b.getChannelData(0), r = b.getChannelData(1);
      for (let i = 0; i < n; i++) { l[i] = src[i * 2]; r[i] = src[i * 2 + 1]; }
      const s = audio.createBufferSource();
      s.buffer = b;
      s.connect(audio.destination);
      const at = Math.max(next, audio.currentTime);
      s.start(at);
      next = at + n / rate;
    }
  }, 40);
}

function screenPos(ev) {
  const r = ev.target.getBoundingClientRect();
  return [Math.floor((ev.clientX - r.left) * 256 / r.width),
          Math.floor((ev.clientY - r.top) * 192 / r.height)];
}

function pointerY(ev) {
  const r = ev.target.getBoundingClientRect();
  return (ev.clientY - r.top) * 192 / r.height;
}

/* A drag starts from what the control is already set to and moves relatively.
 * Jumping the value to wherever the finger landed would destroy a patch on
 * the first touch, which is not what the hardware does. */
function startDrag(ev, kind, id, from) {
  drag = {kind: kind, id: id, from: from, y: pointerY(ev)};
  ev.target.setPointerCapture(ev.pointerId);
}

function dragTo(ev) {
  /* Up increases, and a full screen height covers the range about twice,
   * which is roughly what the device feels like. */
  const moved = (drag.y - pointerY(ev)) / 96;

  if (drag.kind === "knob") {
    const lo = api.knobMin(drag.id), n = api.knobRange(drag.id);
    const v = Math.max(lo, Math.min(lo + n - 1,
                                    Math.round(drag.from + moved * n)));
    api.knobSet(drag.id, v);
    say(api.knobName(drag.id) + "  " + v + "   (" + lo + ".."
        + (lo + n - 1) + ")");
  } else {
    const v = Math.max(0, Math.min(127, Math.round(drag.from + moved * 128)));
    api.mixerSet(drag.id, v);
    say(mixerName(drag.id) + "   " + v);
  }
}

/* The pad drives whichever two knobs the pattern's automation lanes name -
 * across the ROM's songs that is almost always cutoff and peak. */
function kaossSet(h) {
  const kx = api.laneKnob(0), ky = api.laneKnob(1);
  api.knobSet(kx, h & 0xFF);
  api.knobSet(ky, h >> 8);
  say("KAOSS  " + api.knobName(kx) + " " + (h & 0xFF) + "   "
      + api.knobName(ky) + " " + (h >> 8));
}

function mixerName(id) {
  if (id >= 17 && id < 23) return "VOLUME " + (id - 16);
  if (id >= 9 && id < 15) return "PAN " + (id - 8);
  return "FX " + id;
}

function noteOff() {
  if (held === null) return;
  api.note(0, held, 0);
  held = null;
}

/* The step grid. A tap toggles the cell; dragging on from it paints the same
 * thing across the row, which is how a run of steps gets entered. */
function gridTouch(x, y, first) {
  const h = api.seqHit(x, y);
  if (h < 0) return;
  const step = h & 0xFF, row = h >> 8;
  const note = api.rowNote(row);
  const track = api.track() < 2 ? api.track() : 0;

  if (first) paint = {track: track, note: api.seqNote(track, step) === note
                                          ? -1 : note};
  api.seqStep(paint.track, step, paint.note);
  /* Sound the note as it goes in, so a melody can be entered by ear. */
  if (paint.note >= 0 && paint.note !== held) {
    noteOff();
    api.note(paint.track, paint.note, 1);
    held = paint.note;
  }
  say("step " + (step + 1) + (paint.note < 0 ? "  cleared"
                              : "  note " + noteName(paint.note)));
}

/* A value page: the row under the finger is the value. Dragging across sets
 * the neighbouring steps to whatever row the finger is on, which is how a
 * ramp gets drawn. */
function valueTouch(x, y, first) {
  const h = api.seqHit(x, y);
  if (h < 0) return;
  const step = h & 0xFF, row = h >> 8;
  if (first) paint = {};
  api.setStepRow(step, row);
  say(MODES[api.seqMode()] + "  step " + (step + 1) + "  "
      + api.stepValue(api.track() < 2 ? api.track() : 0, step));
}

function drumTouch(x, y, first) {
  const h = api.drumseqHit(x, y);
  if (h < 0) return;
  const step = h & 0xFF, drum = h >> 8;
  const track = 2 + drum;

  if (first) paint = {track: track,
                      note: api.seqNote(track, step) >= 0 ? -1 : 60};
  api.seqStep(paint.track, step, paint.note);
  if (first && paint.note >= 0) api.drumPlay(drum);
  say("DRUM" + (drum + 1) + "  step " + (step + 1)
      + (paint.note < 0 ? "  off" : "  on"));
}

const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A",
                    "A#", "B"];
function noteName(n) {
  return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1);
}

function onMove(ev) {
  if (kaoss) {
    const [x, y] = screenPos(ev);
    const h = api.kaossHit(x, y);
    if (h >= 0) kaossSet(h);
    return;
  }
  if (paint) {
    const [x, y] = screenPos(ev);
    if (pageOf("bottom") === "51_ALL_DRUMS") drumTouch(x, y, false);
    else if (api.seqMode()) valueTouch(x, y, false);
    else gridTouch(x, y, false);
    return;
  }
  if (drag) dragTo(ev);
}

function onDown(which, ev) {
  const page = pageOf(which);
  const [x, y] = screenPos(ev);

  if (page === "90_MAP") {
    const next = api.mapHit(x, y);
    if (!next) return;
    /* Half the map's boxes come in pairs, one per synth, and which of the
     * pair was touched is what picks the track the page then edits. */
    const synth = api.mapSynth(x, y);
    if (synth >= 0) {
      api.setTrack(synth);
      api.seqFollow();
    }
    goto(next);
    syncControls();
    return;
  }
  startAudio();
  if (page.startsWith("60_SYN_SEQ_") || page === "51_ALL_DRUMS") {
    /* The top bar's right-hand button brings the map to this screen, which
     * is what the L shoulder does on the device. */
    if (api.mapButtonHit(x, y)) { goto("90_MAP"); return; }
    if (api.clearHit(x, y)) {
      /* The original raises a modal CLEAR ALL DATA prompt here. That prompt
       * is not drawn yet, so this asks in the browser instead. */
      if (window.confirm("CLEAR ALL DATA?")) {
        api.patternClear(-1);
        say("pattern " + (api.pattern() + 1) + " cleared");
      }
      return;
    }
  }
  if (page.startsWith("60_SYN_SEQ_")) {
    /* The page's own sub-mode button, which steps NOTE, GATE, VOL, PAN and
     * the two automation lanes. */
    if (api.seqModeHit(x, y)) {
      goto(api.seqModeNext());
      say(MODES[api.seqMode()]);
      return;
    }
    if (api.seqMode() === 0) gridTouch(x, y, true);
    else valueTouch(x, y, true);
    ev.target.setPointerCapture(ev.pointerId);
    return;
  }
  if (page === "51_ALL_DRUMS") {
    drumTouch(x, y, true);
    ev.target.setPointerCapture(ev.pointerId);
    return;
  }
  if (page === "32_PATTERN") {
    const n = api.patternHit(x, y);
    if (n >= 0) {
      api.setPattern(n);
      api.seqFollow();
      syncControls();
      say("pattern " + (n + 1));
    }
    return;
  }
  if (page === "33_DRUM_SELECTER") {
    const n = api.drumHit(x, y);
    if (n >= 0) { api.drumPlay(n); say("DRUM" + (n + 1)); }
    return;
  }
  if (page === "70_PATCH" || page === "63_DRUM_EDIT_PATCH") {
    const id = api.patchHit(x, y);
    if (id >= 0) startDrag(ev, "knob", id, api.knobGet(id));
    return;
  }
  if (page === "71_KAOSS") {
    const h = api.kaossHit(x, y);
    if (h < 0) return;
    kaoss = true;
    kaossSet(h);
    ev.target.setPointerCapture(ev.pointerId);
    return;
  }
  if (page === "52_FX" || page === "54_DRUM_FX") {
    const id = api.fxHit(x, y, 0);   /* delay; the type selector is unwired */
    if (id >= 0) startDrag(ev, "mixer", id, api.mixerGet(id));
    return;
  }
  if (page === "30_MIXER") {
    const id = api.mixerHit(x, y);
    if (id >= 0) startDrag(ev, "mixer", id, api.mixerGet(id));
    return;
  }
  if (page === "61_SYN_EDIT" || page === "53_DRUMEDIT") {
    const id = api.knobHit(x, y);
    if (id >= 0) startDrag(ev, "knob", id, api.knobGet(id));
    return;
  }
  if (page.startsWith("81_KEY")) {
    const b = api.keyButton(x, y);
    if (b === 0 || b === 1) {
      setOctave(octave + (b ? 1 : -1));
      return;
    }
    const note = api.keyHit(x, y, octave);
    if (note >= 0) {
      noteOff();
      api.note(api.track() < 2 ? api.track() : 0, note, 1);
      held = note;
      say("note " + noteName(note));
    }
  }
}

function setOctave(n) {
  octave = Math.max(-3, Math.min(3, n));
  say("octave " + (octave > 0 ? "+" : "") + octave);
}

/* The HTML controls are a view of the C side's state, so they are refilled
 * from it rather than tracked here. */
function syncControls() {
  $("pick-pattern").value = String(api.pattern());
  $("pick-track").value = String(api.track());
  $("pick-steps").value = String(api.patternSteps());
  $("bpm").value = api.getTempo();
  $("swing").value = api.getSwing();
  $("name").value = api.getName();
  $("mode").textContent = api.songMode() ? "song" : "pattern";
  $("mode").classList.toggle("on", !!api.songMode());
  $("play").textContent = api.cursor() >= 0 ? "stop" : "play";
  $("play").classList.toggle("on", api.cursor() >= 0);
}

function loadSong(name) {
  if (api.song(name) < 0) { say("error: " + api.error()); return; }
  startAudio();
  /* Scroll the roll to whatever the loaded pattern actually uses, or the
   * notes would be off the top or bottom of the window and it would look
   * empty. The ROM's own songs run from 34 to 80. */
  api.seqFollow();
  syncControls();
  say(name + "   " + api.songBars() + " bars");
}

/* Writing the file out. The C side produces the same byte image the game's
 * own serialiser would, MD5 header and all. */
function save() {
  const n = api.export();
  if (n <= 0) { say("nothing to save"); return; }
  const bytes = Module.HEAPU8.slice(api.exportBuf(), api.exportBuf() + n);
  const url = URL.createObjectURL(new Blob([bytes],
                                           {type: "application/octet-stream"}));
  const a = document.createElement("a");
  a.href = url;
  a.download = (api.getName() || "song").replace(/[^A-Za-z0-9]/g, "") + ".dat";
  a.click();
  URL.revokeObjectURL(url);
  say("saved " + n + " bytes");
}

Module.onRuntimeInitialized = () => {
  api = {
    init: Module.cwrap("ds10_web_init", "number", []),
    page: Module.cwrap("ds10_web_page", "number", ["string", "string"]),
    render: Module.cwrap("ds10_web_render", null, []),
    top: Module.cwrap("ds10_web_top", "number", []),
    bottom: Module.cwrap("ds10_web_bottom", "number", []),
    width: Module.cwrap("ds10_web_width", "number", []),
    height: Module.cwrap("ds10_web_height", "number", []),
    error: Module.cwrap("ds10_web_error", "string", []),
    mapHit: Module.cwrap("ds10_web_map_hit", "string", ["number", "number"]),
    mapSynth: Module.cwrap("ds10_web_map_synth", "number",
                           ["number", "number"]),
    patternHit: Module.cwrap("ds10_web_pattern_hit", "number",
                             ["number", "number"]),
    laneKnob: Module.cwrap("ds10_web_lane_knob", "number", ["number"]),
    mapButtonHit: Module.cwrap("ds10_web_mapbutton_hit", "number",
                               ["number", "number"]),
    clearHit: Module.cwrap("ds10_web_clear_hit", "number",
                           ["number", "number"]),
    pattern: Module.cwrap("ds10_web_pattern", "number", []),
    keyHit: Module.cwrap("ds10_web_key_hit", "number",
                         ["number", "number", "number"]),
    keyButton: Module.cwrap("ds10_web_key_button", "number",
                            ["number", "number"]),
    audioInit: Module.cwrap("ds10_web_audio_init", "number", []),
    audioRender: Module.cwrap("ds10_web_audio", "number", ["number"]),
    audioBuf: Module.cwrap("ds10_web_audio_buf", "number", []),
    rate: Module.cwrap("ds10_web_rate", "number", []),
    note: Module.cwrap("ds10_web_note", null, ["number", "number", "number"]),
    knobHit: Module.cwrap("ds10_web_knob_hit", "number", ["number", "number"]),
    knobName: Module.cwrap("ds10_web_knob_name", "string", ["number"]),
    knobMin: Module.cwrap("ds10_web_knob_min", "number", ["number"]),
    knobRange: Module.cwrap("ds10_web_knob_range", "number", ["number"]),
    knobGet: Module.cwrap("ds10_web_knob_get", "number", ["number"]),
    knobSet: Module.cwrap("ds10_web_knob_set", null, ["number", "number"]),
    mixerHit: Module.cwrap("ds10_web_mixer_hit", "number",
                           ["number", "number"]),
    mixerGet: Module.cwrap("ds10_web_mixer_get", "number", ["number"]),
    mixerSet: Module.cwrap("ds10_web_mixer_set", null, ["number", "number"]),
    fxHit: Module.cwrap("ds10_web_fx_hit", "number",
                        ["number", "number", "number"]),
    seqHit: Module.cwrap("ds10_web_seq_hit", "number", ["number", "number"]),
    drumseqHit: Module.cwrap("ds10_web_drumseq_hit", "number",
                             ["number", "number"]),
    kaossHit: Module.cwrap("ds10_web_kaoss_hit", "number",
                           ["number", "number"]),
    patchHit: Module.cwrap("ds10_web_patch_hit", "number",
                           ["number", "number"]),
    drumHit: Module.cwrap("ds10_web_drum_hit", "number",
                          ["number", "number"]),
    drumPlay: Module.cwrap("ds10_web_drum_hit_play", null, ["number"]),
    factoryPatch: Module.cwrap("ds10_web_factory_patch", "number",
                               ["number"]),
    seqStep: Module.cwrap("ds10_web_seq_step", null,
                          ["number", "number", "number"]),
    seqNote: Module.cwrap("ds10_web_seq_note", "number",
                          ["number", "number"]),
    seqPlay: Module.cwrap("ds10_web_seq_play", null, ["number"]),
    song: Module.cwrap("ds10_web_song", "number", ["string"]),
    songBars: Module.cwrap("ds10_web_song_bars", "number", []),
    cursor: Module.cwrap("ds10_web_cursor", "number", []),
    rowNote: Module.cwrap("ds10_web_row_note", "number", ["number"]),
    seqTop: Module.cwrap("ds10_web_seq_top", "number", []),
    setSeqTop: Module.cwrap("ds10_web_set_seq_top", null, ["number"]),
    seqFollow: Module.cwrap("ds10_web_seq_follow", null, []),
    seqMode: Module.cwrap("ds10_web_seq_mode", "number", []),
    seqModeHit: Module.cwrap("ds10_web_seq_mode_hit", "number",
                             ["number", "number"]),
    seqModeNext: Module.cwrap("ds10_web_seq_mode_next", "string", []),
    seqModePage: Module.cwrap("ds10_web_seq_mode_page", "string", []),
    stepValue: Module.cwrap("ds10_web_step_value", "number",
                            ["number", "number"]),
    setStepRow: Module.cwrap("ds10_web_set_step_row", null,
                             ["number", "number"]),
    pattern: Module.cwrap("ds10_web_pattern", "number", []),
    setPattern: Module.cwrap("ds10_web_set_pattern", null, ["number"]),
    track: Module.cwrap("ds10_web_track", "number", []),
    setTrack: Module.cwrap("ds10_web_set_track", null, ["number"]),
    patternSteps: Module.cwrap("ds10_web_pattern_steps", "number", []),
    setPatternSteps: Module.cwrap("ds10_web_set_pattern_steps", null,
                                  ["number"]),
    patternClear: Module.cwrap("ds10_web_pattern_clear", null, ["number"]),
    songMode: Module.cwrap("ds10_web_song_mode", "number", []),
    setSongMode: Module.cwrap("ds10_web_set_song_mode", null, ["number"]),
    getTempo: Module.cwrap("ds10_web_get_tempo", "number", []),
    setTempo: Module.cwrap("ds10_web_set_tempo", null, ["number"]),
    getSwing: Module.cwrap("ds10_web_get_swing", "number", []),
    setSwing: Module.cwrap("ds10_web_set_swing", null, ["number"]),
    getName: Module.cwrap("ds10_web_get_name", "string", []),
    setName: Module.cwrap("ds10_web_set_name", null, ["string"]),
    chainSet: Module.cwrap("ds10_web_chain_set", null, ["number", "number"]),
    export: Module.cwrap("ds10_web_export", "number", []),
    exportBuf: Module.cwrap("ds10_web_export_buf", "number", []),
  };
  if (api.init() < 0) { say("error: " + api.error()); return; }

  fill($("pick-song"), SONGS, "ds10init");
  fill($("pick-bottom"), BOTTOM_PAGES, "60_SYN_SEQ_PIANO");
  fill($("pick-top"), TOP_PAGES, "90_MAP");
  fill($("pick-pattern"),
       Array.from({length: 16}, (_, i) => [String(i), String(i + 1)]), "0");
  fill($("pick-track"), TRACKS.map((n, i) => [String(i), n]), "0");
  fill($("pick-steps"),
       Array.from({length: 16}, (_, i) => [String(i + 1), String(i + 1)]),
       "16");

  $("pick-song").addEventListener("change",
                                  (e) => loadSong(e.target.value));
  for (const id of ["pick-bottom", "pick-top"])
    $(id).addEventListener("change", show);
  $("pick-pattern").addEventListener("change", (e) => {
    api.setPattern(+e.target.value);
    api.seqFollow();
    syncControls();
    say("pattern " + (+e.target.value + 1));
  });
  $("pick-track").addEventListener("change", (e) => {
    api.setTrack(+e.target.value);
    api.seqFollow();
    say(TRACKS[+e.target.value]);
  });
  $("pick-steps").addEventListener("change",
                                   (e) => api.setPatternSteps(+e.target.value));
  $("bpm").addEventListener("change", (e) => api.setTempo(+e.target.value));
  $("swing").addEventListener("change", (e) => api.setSwing(+e.target.value));
  $("name").addEventListener("change", (e) => api.setName(e.target.value));
  $("save").addEventListener("click", save);
  /* Opening a file drops it into the preloaded filesystem under a fixed name
   * and loads that, so it goes through exactly the path the ROM's own songs
   * take - the game's parser, hash check included. */
  $("load").addEventListener("change", async (e) => {
    const f = e.target.files[0];
    if (!f) return;
    const bytes = new Uint8Array(await f.arrayBuffer());
    try {
      Module.FS.writeFile("/data/nitrofs/demo/opened.dat", bytes);
    } catch (err) {
      say("cannot stage the file: " + err);
      return;
    }
    if (api.song("opened") < 0) { say(f.name + ": " + api.error()); return; }
    startAudio();
    api.seqFollow();
    syncControls();
    say(f.name + "   " + api.songBars() + " bars");
  });
  $("clear").addEventListener("click", () => {
    api.patternClear(-1);
    say("pattern " + (api.pattern() + 1) + " cleared");
  });
  $("oct-down").addEventListener("click", () => setOctave(octave - 1));
  $("oct-up").addEventListener("click", () => setOctave(octave + 1));
  $("mode").addEventListener("click", () => {
    api.setSongMode(!api.songMode());
    syncControls();
  });
  $("play").addEventListener("click", () => {
    startAudio();
    api.seqPlay(api.cursor() >= 0 ? 0 : 1);
    syncControls();
  });

  for (const which of ["top", "bottom"]) {
    canvases[which].addEventListener("pointerdown", (e) => onDown(which, e));
    canvases[which].addEventListener("pointermove", onMove);
    for (const ev of ["pointerup", "pointerleave", "pointercancel"])
      canvases[which].addEventListener(ev, () => {
        noteOff();
        drag = null;
        paint = null;
        kaoss = false;
      });
  }

  document.addEventListener("keydown", (e) => {
    if (e.target.tagName === "INPUT" || e.target.tagName === "SELECT") return;
    if (e.key === "z") setOctave(octave - 1);
    if (e.key === "x") setOctave(octave + 1);
    if (e.key === "[") { api.setSeqTop(api.seqTop() - 12); say("scrolled"); }
    if (e.key === "]") { api.setSeqTop(api.seqTop() + 12); say("scrolled"); }
    if (e.key >= "1" && e.key <= "9") {
      startAudio();
      const n = e.key.charCodeAt(0) - 49;
      if (api.factoryPatch(n) === 0) say("factory patch " + n);
    }
    if (e.key === " ") {
      e.preventDefault();
      startAudio();
      api.seqPlay(api.cursor() >= 0 ? 0 : 1);
      syncControls();
    }
  });

  /* Come up on the blank template, so the thing is ready to be played into
   * rather than needing a song chosen first. No AudioContext yet: the
   * browser wants a gesture before one will run. */
  if (api.song("ds10init") < 0) say("error: " + api.error());
  else { api.seqFollow(); syncControls(); }
  show();
  loop();
};
