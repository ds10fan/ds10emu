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

/* Reporting.
 *
 * Two black canvases and a status stuck on "loading" say nothing, and on a
 * phone there is no console to look in. So everything - the breadcrumbs of
 * startup, anything thrown, anything the module prints - goes both to the
 * status line and to the server, which appends it to out/weblog.txt.
 */
/* Batched. One fetch per line saturated the browser's six connections to the
 * host and starved the 4 MB asset download, which then stalled at 99% and
 * never booted - the diagnostics broke the thing they were diagnosing. */
let logQueue = [];
let logTimer = null;

function logFlush() {
  logTimer = null;
  if (!logQueue.length) return;
  const body = logQueue.join("\n");
  logQueue = [];
  try {
    fetch("/log", {method: "POST", body: body, keepalive: true})
      .catch(() => {});
  } catch (e) { /* nothing useful to do if even this fails */ }
}

function logLine(line) {
  logQueue.push(line);
  if (logQueue.length > 60) logQueue.shift();
  if (!logTimer) logTimer = setTimeout(logFlush, 600);
}

function report(line) {
  const el = document.getElementById("status");
  if (el) el.textContent = String(line).slice(0, 300);
  logLine(String(line));
  /* Errors are worth a connection of their own, in case the page then dies. */
  logFlush();
}

function trace(line) { logLine("  . " + line); }

window.addEventListener("error", (e) => {
  report("THROW " + (e.message || e.error) + "  at " + (e.filename || "?")
         + ":" + (e.lineno || 0)
         + (e.error && e.error.stack
            ? "\n" + e.error.stack.split("\n").slice(0, 4).join("\n") : ""));
});
window.addEventListener("unhandledrejection", (e) => {
  const r = e.reason;
  report("REJECT " + (r && r.message ? r.message : r)
         + (r && r.stack ? "\n" + r.stack.split("\n").slice(0, 4).join("\n")
                         : ""));
});

trace("page loaded, " + navigator.userAgent);
trace("WebAssembly " + (typeof WebAssembly) + ", Module "
      + (typeof Module) + ", calledRun " + (typeof Module !== "undefined"
                                            && !!Module.calledRun));

if (typeof Module === "undefined") {
  report("ds10.js did not load - no Module");
} else {
  let lastPct = -1;
  Module.setStatus = (t) => {
    const el = document.getElementById("status");
    if (el && t) el.textContent = t;
    /* Progress goes to the screen every tick but to the log at quarters:
     * this fires sixty times a second while the assets come down. */
    const m = /\((\d+)\/(\d+)\)/.exec(t || "");
    if (m) {
      const pct = Math.floor(+m[1] * 4 / +m[2]) * 25;
      if (pct !== lastPct) { lastPct = pct; trace("downloading " + pct + "%"); }
      return;
    }
    if (t) trace("setStatus " + t);
  };
  Module.printErr = (t) => {
    trace("stderr " + t);
    if (/abort|error|failed|exception/i.test(String(t))) report("MODULE " + t);
  };
  Module.onAbort = (t) => report("ABORT " + t);
}

/* If the runtime never comes up, say so rather than sitting on "loading". */
setTimeout(() => {
  if (!api) report("STALLED: the runtime did not start within 20s (calledRun "
                   + (typeof Module !== "undefined" && !!Module.calledRun)
                   + ")");
}, 20000);

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
let playing = false;
let cursor = -1;   /* the step the worklet reports */

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

/* The panel reads everything back from the C side, so anything the screens
 * change has to poke it. */
function repanel() { if (typeof panelSync === "function") panelSync(); }

function goto(page) {
  $("pick-bottom").value = page;
  show();
}

/* Audio.
 *
 * The engine runs on the audio thread in an AudioWorklet, not here. Rendering
 * on the main thread competed with the canvas loop and, on a phone, lost
 * badly enough to lock the page. This side only sends messages.
 *
 * The context is opened at the engine's own 32768 Hz so nothing has to
 * resample; the browser handles the final conversion to the device rate.
 *
 * The worklet has its own copy of the engine, so an edit here has to be sent
 * there. The whole song goes over on any change - 12 KB, and it cannot drift.
 */
let worklet = null;

function sendSong() {
  if (!worklet) return;
  const n = api.export();
  if (n <= 0) return;
  trace("sendSong " + n + " bytes, playing " + playing);
  worklet.port.postMessage({
    type: "song",
    bytes: Module.HEAPU8.slice(api.exportBuf(), api.exportBuf() + n),
    pattern: api.pattern(),
    track: api.track(),
    songMode: !!api.songMode(),
    playing: playing,
  });
}

function send(m) { if (worklet) worklet.port.postMessage(m); }

/* Start or stop. Both engines are told: the worklet's makes the sound, and
 * this one silences its own voices so a preview note does not hang. */
function transport(on) {
  trace("transport " + (on ? "start" : "stop"));
  playing = !!on;
  api.seqPlay(playing ? 1 : 0);
  send({type: "play", on: playing});
  if (!playing) { cursor = -1; api.setCursor(-1); }
}

async function startAudio() {
  if (audio) {
    if (audio.state === "suspended") await audio.resume();
    return;
  }
  if (api.audioInit() < 0) { report("audioInit failed: " + api.error()); return; }
  try {
    audio = new AudioContext({sampleRate: api.rate(), latencyHint: "interactive"});
    await audio.audioWorklet.addModule("worklet.js");
    worklet = new AudioWorkletNode(audio, "ds10", {
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
    });
    worklet.port.onmessage = (e) => {
      const m = e.data;
      if (m.type === "cursor") { cursor = m.step; api.setCursor(m.step); }
      else if (m.type === "ready") trace("worklet ready at " + m.rate + " Hz");
      else if (m.type === "error") report("worklet: " + m.what);
      else if (m.type === "loaded")
        trace("worklet took a song: rc " + m.rc + " len " + m.len
              + " playing " + m.playing);
      else if (m.type === "level")
        trace("worklet peak " + m.peak + " cursor " + m.cursor
              + " note-ons per track " + (m.notes || []).join("/"));
    };
    worklet.connect(audio.destination);
    sendSong();
    trace("audio started: context " + audio.sampleRate + " Hz, state "
          + audio.state + ", destination channels "
          + audio.destination.maxChannelCount);
  } catch (e) {
    audio = null;
    report("audio failed: " + (e && e.message ? e.message : e));
  }
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
  drag = {kind: kind, id: id, from: from, y: pointerY(ev), moves: 0};
  outcome = kind + " " + id + " grabbed at " + from;
  /* Say what was grabbed and what it is set to, so a touch that has not
   * turned anything yet is still visibly a touch on that control. */
  say((kind === "knob" ? api.knobName(id) : mixerName(id)) + "  " + from
      + "   (drag up or down)");
  try {
    ev.target.setPointerCapture(ev.pointerId);
  } catch (e) {
    trace("setPointerCapture failed: " + e.message);
  }
}

function dragTo(ev) {
  /* Whether moves arrive at all is the difference between "the knob is dead"
   * and "the knob needs a drag", and the two look identical from outside. */
  if (drag.moves++ === 0) trace("first move on " + drag.kind + " " + drag.id);

  /* Up increases, and a full screen height covers the range about twice,
   * which is roughly what the device feels like. */
  const moved = (drag.y - pointerY(ev)) / 96;

  if (drag.kind === "knob") {
    const lo = api.knobMin(drag.id), n = api.knobRange(drag.id);
    const v = Math.max(lo, Math.min(lo + n - 1,
                                    Math.round(drag.from + moved * n)));
    api.knobSet(drag.id, v);
    send({type: "knob", track: api.track(), id: drag.id, value: v});
    drag.dirty = 1;
    say(api.knobName(drag.id) + "  " + v + "   (" + lo + ".."
        + (lo + n - 1) + ")");
  } else {
    const v = Math.max(0, Math.min(127, Math.round(drag.from + moved * 128)));
    api.mixerSet(drag.id, v);
    send({type: "mixer", id: drag.id, value: v});
    say(mixerName(drag.id) + "   " + v);
  }
}

/* The pad drives whichever two knobs the pattern's automation lanes name -
 * across the ROM's songs that is almost always cutoff and peak. */
function kaossSet(h) {
  const kx = api.laneKnob(0), ky = api.laneKnob(1);
  api.knobSet(kx, h & 0xFF);
  api.knobSet(ky, h >> 8);
  send({type: "knob", track: api.track(), id: kx, value: h & 0xFF});
  send({type: "knob", track: api.track(), id: ky, value: h >> 8});
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
  send({type: "note", voice: 0, note: held, on: 0});
  held = null;
}

/* The step grid. A tap toggles the cell; dragging on from it paints the same
 * thing across the row, which is how a run of steps gets entered. */
function gridTouch(x, y, first) {
  const h = api.seqHit(x, y);
  outcome = "seqHit " + h;
  if (h < 0) return;
  const step = h & 0xFF, row = h >> 8;
  const note = api.rowNote(row);
  const track = api.track() < 2 ? api.track() : 0;

  if (first) paint = {track: track, note: api.seqNote(track, step) === note
                                          ? -1 : note};
  api.seqStep(paint.track, step, paint.note);
  sendSong();
  /* Sound the note as it goes in, so a melody can be entered by ear. */
  if (paint.note >= 0 && paint.note !== held) {
    noteOff();
    send({type: "note", voice: paint.track, note: paint.note, on: 1});
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
  outcome = "valueHit " + h;
  if (h < 0) return;
  const step = h & 0xFF, row = h >> 8;
  if (first) paint = {};
  api.setStepRow(step, row);
  sendSong();
  say(MODES[api.seqMode()] + "  step " + (step + 1) + "  "
      + api.stepValue(api.track() < 2 ? api.track() : 0, step));
}

function drumTouch(x, y, first) {
  const h = api.drumseqHit(x, y);
  outcome = "drumseqHit " + h;
  if (h < 0) return;
  const step = h & 0xFF, drum = h >> 8;
  const track = 2 + drum;

  if (first) paint = {track: track,
                      note: api.seqNote(track, step) >= 0 ? -1 : 60};
  api.seqStep(paint.track, step, paint.note);
  sendSong();
  if (first && paint.note >= 0) send({type: "drum", n: drum});
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

let outcome = "";

function onDown(which, ev) {
  const page = pageOf(which);
  const [x, y] = screenPos(ev);

  outcome = "nothing";
  try {
    onDownInner(which, ev, page, x, y);
  } finally {
    repanel();
    trace("touch " + which + " (" + x + "," + y + ") on " + page
          + " -> " + outcome);
  }
}

function onDownInner(which, ev, page, x, y) {

  if (page === "90_MAP") {
    const next = api.mapHit(x, y);
    outcome = "map -> " + (next || "nothing");
    if (!next) return;
    /* Half the map's boxes come in pairs, one per synth, and which of the
     * pair was touched is what picks the track the page then edits. */
    const synth = api.mapSynth(x, y);
    if (synth >= 0) {
      api.setTrack(synth);
      send({type: "track", n: synth});
      api.seqFollow();
    }
    goto(next);
    syncControls();
    return;
  }
  startAudio();
  /* Every page carries the same button at the right of its top bar, and it
   * brings the map to this screen - the L shoulder's job on the device.
   * Wiring it on the sequencer alone left every other page with no way back,
   * which is what "eventually you cannot swap the screens" was. */
  if (page !== "90_MAP" && api.mapButtonHit(x, y)) {
    outcome = "map button";
    goto("90_MAP");
    return;
  }
  if ((page.startsWith("60_SYN_SEQ_") || page === "51_ALL_DRUMS") &&
      api.clearHit(x, y)) {
    /* The original raises a modal CLEAR ALL DATA prompt here. That prompt is
     * not drawn yet, so this asks in the browser instead. */
    outcome = "clear";
    if (window.confirm("CLEAR ALL DATA?")) {
      api.patternClear(-1);
      sendSong();
      say("pattern " + (api.pattern() + 1) + " cleared");
    }
    return;
  }
  if (page.startsWith("60_SYN_SEQ_")) {
    /* The page's own sub-mode button, which steps NOTE, GATE, VOL, PAN and
     * the two automation lanes. */
    if (api.seqModeHit(x, y)) {
      outcome = "sub-mode";
      goto(api.seqModeNext());
      say(MODES[api.seqMode()]);
      return;
    }
    if (api.seqMode() === 0) gridTouch(x, y, true);
    else valueTouch(x, y, true);
    outcome = "grid " + outcome;
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
    outcome = "pattern " + n;
    if (n >= 0) {
      api.setPattern(n);
      send({type: "pattern", n: n});
      api.seqFollow();
      syncControls();
      say("pattern " + (n + 1));
    }
    return;
  }
  if (page === "33_DRUM_SELECTER") {
    const n = api.drumHit(x, y);
    outcome = "drum " + n;
    if (n >= 0) { send({type: "drum", n: n}); say("DRUM" + (n + 1)); }
    return;
  }
  if (page === "70_PATCH" || page === "63_DRUM_EDIT_PATCH") {
    const id = api.patchHit(x, y);
    outcome = "patchHit " + id;
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
    outcome = "fxHit " + id;
    if (id >= 0) startDrag(ev, "mixer", id, api.mixerGet(id));
    return;
  }
  if (page === "30_MIXER") {
    const id = api.mixerHit(x, y);
    outcome = "mixerHit " + id;
    if (id >= 0) startDrag(ev, "mixer", id, api.mixerGet(id));
    return;
  }
  if (page === "61_SYN_EDIT" || page === "53_DRUMEDIT") {
    const id = api.knobHit(x, y);
    outcome = "knobHit " + id;
    if (id >= 0) startDrag(ev, "knob", id, api.knobGet(id));
    return;
  }
  if (page.startsWith("81_KEY")) {
    const b = api.keyButton(x, y);
    outcome = "keyButton " + b;
    if (b === 0 || b === 1) {
      setOctave(octave + (b ? 1 : -1));
      return;
    }
    const note = api.keyHit(x, y, octave);
    outcome = "key note " + note;
    if (note >= 0) {
      noteOff();
      send({type: "note", voice: api.track() < 2 ? api.track() : 0,
            note: note, on: 1});
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
/* The panel is the control surface; this just pokes it. The two rows of HTML
 * controls that used to sit above the screens duplicated it, and two surfaces
 * showing the same state is one more place for them to disagree. */
function syncControls() { repanel(); }

function loadSong(name) {
  if (api.song(name) < 0) { say("error: " + api.error()); return; }
  startAudio();
  /* Scroll the roll to whatever the loaded pattern actually uses, or the
   * notes would be off the top or bottom of the window and it would look
   * empty. The ROM's own songs run from 34 to 80. */
  api.seqFollow();
  playing = true;
  sendSong();
  syncControls();
  say(name + "   " + api.songBars() + " bars");
}

/* Writing the file out. The C side produces the same byte image the game's
 * own serialiser would, MD5 header and all. */
/* Opening a .dat: staged into the preloaded filesystem under a fixed name and
 * loaded from there, so it goes through the same parser and hash check the
 * ROM's own songs do. */
async function openFile(e) {
  const f = e.target.files && e.target.files[0];
  if (!f) return;
  const bytes = new Uint8Array(await f.arrayBuffer());
  try {
    Module.FS.writeFile("/data/nitrofs/demo/opened.dat", bytes);
  } catch (err) {
    report("cannot stage the file: " + err);
    return;
  }
  if (api.song("opened") < 0) { report(f.name + ": " + api.error()); return; }
  startAudio();
  api.seqFollow();
  playing = true;
  sendSong();
  syncControls();
  repanel();
  say(f.name + "   " + api.songBars() + " bars");
}

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

function boot() {
  trace("boot: runtime up, wiring the api");
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
    setLaneKnob: Module.cwrap("ds10_web_set_lane_knob", null,
                              ["number", "number"]),
    drumKnob: Module.cwrap("ds10_web_drum_knob", "number",
                           ["number", "number"]),
    setDrumKnob: Module.cwrap("ds10_web_set_drum_knob", null,
                              ["number", "number", "number"]),
    drumsRerender: Module.cwrap("ds10_web_drums_rerender", null, []),
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
    setCursor: Module.cwrap("ds10_web_set_cursor", null, ["number"]),
    songBytes: Module.cwrap("ds10_web_song_bytes", "number",
                            ["number", "number"]),
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
    chainGet: Module.cwrap("ds10_web_chain_get", "number", ["number"]),
    chainRemove: Module.cwrap("ds10_web_chain_remove", null, ["number"]),
    patternCopy: Module.cwrap("ds10_web_pattern_copy", null,
                              ["number", "number"]),
    transpose: Module.cwrap("ds10_web_transpose", null, ["number", "number"]),
    trackClear: Module.cwrap("ds10_web_track_clear", null, ["number"]),
    stepGate: Module.cwrap("ds10_web_step_gate", "number",
                           ["number", "number"]),
    setStepGate: Module.cwrap("ds10_web_set_step_gate", null,
                              ["number", "number", "number"]),
    stepVel: Module.cwrap("ds10_web_step_vel", "number",
                          ["number", "number"]),
    setStepVel: Module.cwrap("ds10_web_set_step_vel", null,
                             ["number", "number", "number"]),
    stepPan: Module.cwrap("ds10_web_step_pan", "number",
                          ["number", "number"]),
    setStepPan: Module.cwrap("ds10_web_set_step_pan", null,
                             ["number", "number", "number"]),
    export: Module.cwrap("ds10_web_export", "number", []),
    exportBuf: Module.cwrap("ds10_web_export_buf", "number", []),
  };
  trace("boot: api wired");
  if (api.init() < 0) { report("init failed: " + api.error()); return; }
  trace("boot: ui initialised");

  fill($("pick-bottom"), BOTTOM_PAGES, "60_SYN_SEQ_PIANO");
  fill($("pick-top"), TOP_PAGES, "90_MAP");

  for (const id of ["pick-bottom", "pick-top"])
    $(id).addEventListener("change", show);

  for (const which of ["top", "bottom"]) {
    canvases[which].addEventListener("pointerdown", (e) => onDown(which, e));
    canvases[which].addEventListener("pointermove", onMove);
    /* pointerleave is NOT in this list. With the pointer captured the
     * element still gets leave events when the finger crosses its edge, and
     * dropping the drag there makes a knob feel dead the moment you move far
     * enough to turn it - which on a 256-pixel-wide canvas is not far. */
    for (const ev of ["pointerup", "pointercancel"])
      canvases[which].addEventListener(ev, (e) => {
        /* Post the song once the drag ends, not on every pixel of it. */
        if (drag && drag.dirty) sendSong();
        if (drag && drag.moves === 0)
          trace("released " + drag.kind + " " + drag.id + " with no move ("
                + e.type + ")");
        else if (e.type === "pointercancel")
          trace("pointercancel during " + (drag ? "a drag" : "no drag"));
        noteOff();
        drag = null;
        paint = null;
        kaoss = false;
      });
    canvases[which].addEventListener("pointerleave", () => noteOff());
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
      if (api.factoryPatch(n) === 0) { sendSong(); say("factory patch " + n); }
    }
    if (e.key === " ") {
      e.preventDefault();
      startAudio();
      transport(!playing);
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
  /* Every capability as plain HTML, so a control that does nothing on the DS
   * screens can be told apart from a function that does nothing. */
  if (typeof buildPanel === "function") buildPanel();
  trace("boot: first frame drawn, status " + status.textContent);
}

/* If the assets were cached the runtime can be up before this file runs, and
 * an onRuntimeInitialized assigned after the fact is never called - which
 * leaves the page on "loading" with both screens black. */
if (Module.calledRun) boot();
else Module.onRuntimeInitialized = boot;
