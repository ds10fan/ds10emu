/* Drives the WebAssembly build: pick a page, render it, put the pixels on the
 * canvases, and play the synth when the keyboard page is touched. The C side
 * owns the framebuffers and the audio; this only moves bytes. */
"use strict";

/* The pages under /data/mainbg that are whole screens. Those whose sprite
 * layer is known reproduce the hardware exactly; the rest draw their
 * background only, which is why they look bare. */
const BOTTOM_PAGES = [
  "81_KEY_i", "61_SYN_EDIT", "70_PATCH", "30_MIXER", "60_SYN_SEQ_PIANO",
  "71_KAOSS", "52_FX", "32_PATTERN", "51_ALL_DRUMS", "53_DRUMEDIT",
  "54_DRUM_FX", "55_DRUMEDITMAP", "60_SYN_SEQ_GATE", "60_SYN_SEQ_PAN",
  "60_SYN_SEQ_VOL", "60_SYN_SEQ_KAOSS", "62_DRUM_SEQ_GATE",
  "63_DRUM_EDIT_PATCH", "33_DRUM_SELECTER", "01_MAIN_MENU", "15_OPTION",
  "31_SONG", "90_MAP",
];
const TOP_PAGES = [
  "90_MAP", "AD0_TITLE", "AD1_KORG", "AD2_PROCYONSTUDIO", "AD3_AQI",
  "AD4_CAVIA", "01_MAIN_MENU",
];

const status = document.getElementById("status");
const canvases = {
  top: document.getElementById("top"),
  bottom: document.getElementById("bottom"),
};
const ctx = {
  top: canvases.top.getContext("2d"),
  bottom: canvases.bottom.getContext("2d"),
};

let api = null;
let audio = null;      /* AudioContext, once the user has interacted */
let octave = 0;
let held = null;       /* the note currently sounding, if any */
let drag = null;       /* {id, value, y} while a knob is being turned */
let playing = false;

function fill(select, names, initial) {
  for (const n of names) {
    const o = document.createElement("option");
    o.value = o.textContent = n;
    select.appendChild(o);
  }
  select.value = initial;
}

function blit(which, ptr, w, h) {
  const bytes = new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, w * h * 4);
  ctx[which].putImageData(new ImageData(bytes, w, h), 0, 0);
}

function draw() {
  api.render();
  const w = api.width(), h = api.height();
  blit("top", api.top(), w, h);
  blit("bottom", api.bottom(), w, h);
}

function pageOf(which) {
  return document.getElementById(
    which === "bottom" ? "pick-bottom" : "pick-top").value;
}

function show() {
  const bottom = pageOf("bottom"), top = pageOf("top");
  if (api.page(bottom, top) < 0) {
    status.textContent = "error: " + api.error();
    return;
  }
  draw();
  status.textContent = bottom + "  /  " + top;
}

/* Audio. The engine runs at 32768 Hz and the browser resamples. Blocks are
 * rendered ahead of the clock and queued, which is enough for a keyboard. */
function startAudio() {
  if (audio) return;
  if (api.audioInit() < 0) {
    status.textContent = "error: " + api.error();
    return;
  }
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

function setKnob(id, value) {
  const lo = api.knobMin(id), n = api.knobRange(id);
  const v = Math.max(lo, Math.min(lo + n - 1, Math.round(value)));
  api.knobSet(id, v);
  draw();
  status.textContent = api.knobName(id) + "  " + v + "   ("
    + lo + ".." + (lo + n - 1) + ")";
  return v;
}

function onMove(ev) {
  if (!drag) return;
  const r = ev.target.getBoundingClientRect();
  const y = (ev.clientY - r.top) * 192 / r.height;
  /* Up increases. A full screen height covers the whole range twice, which
   * is about what the device feels like. */
  if (drag.mixer) {
    const v = Math.max(0, Math.min(127,
      Math.round(drag.start + (drag.y - y) * 128 / 96)));
    api.mixerSet(drag.id, v);
    status.textContent = (drag.id < 9 ? "FX parameter " + drag.id + " "
      : drag.id < 17 ? "PAN " + (drag.id - 8)
      : "VOLUME " + (drag.id - 16)) + "   " + v;
    return;
  }
  const span = api.knobRange(drag.id);
  drag.value = setKnob(drag.id, drag.start + (drag.y - y) * span / 96);
}

function noteOff() {
  if (held === null) return;
  api.note(0, held, 0);
  held = null;
}

function onDown(which, ev) {
  const page = pageOf(which);
  const [x, y] = screenPos(ev);

  if (page === "90_MAP") {
    const next = api.mapHit(x, y);
    if (next) {
      document.getElementById("pick-bottom").value = next;
      show();
    }
    return;
  }
  if (page === "60_SYN_SEQ_PIANO") {
    startAudio();
    const h = api.seqHit(x, y);
    if (h < 0) return;
    const step = h & 0xFF, row = h >> 8;
    /* Twelve rows, top is the highest note. C4 sits at the bottom. */
    const note = 60 + (11 - row);
    const cur = api.seqNote(0, step);
    api.seqStep(0, step, cur === note ? -1 : note);
    if (!playing) { playing = true; api.seqPlay(1); }
    status.textContent = "step " + (step + 1)
      + (cur === note ? "  cleared" : "  note " + note);
    return;
  }
  if (page === "52_FX") {
    startAudio();
    const id = api.fxHit(x, y, 0);   /* delay; the type selector is unwired */
    if (id >= 0) {
      const r = ev.target.getBoundingClientRect();
      drag = {id: id, mixer: true, start: 64,
              y: (ev.clientY - r.top) * 192 / r.height};
      api.mixerSet(id, 64);
      status.textContent = "FX parameter " + id + "   64";
      ev.target.setPointerCapture(ev.pointerId);
    }
    return;
  }
  if (page === "30_MIXER") {
    startAudio();
    const id = api.mixerHit(x, y);
    if (id >= 0) {
      const r = ev.target.getBoundingClientRect();
      drag = {id: id, mixer: true, start: 64,
              y: (ev.clientY - r.top) * 192 / r.height};
      api.mixerSet(id, 64);
      status.textContent = (id < 17 ? "PAN " : "VOLUME ")
        + ((id < 17 ? id - 9 : id - 17) + 1) + "   64";
      ev.target.setPointerCapture(ev.pointerId);
    }
    return;
  }
  if (page === "61_SYN_EDIT") {
    startAudio();
    const id = api.knobHit(x, y);
    if (id >= 0) {
      const r = ev.target.getBoundingClientRect();
      drag = {id: id, start: api.knobMin(id) + api.knobRange(id) / 2,
              y: (ev.clientY - r.top) * 192 / r.height};
      setKnob(id, drag.start);
      ev.target.setPointerCapture(ev.pointerId);
    }
    return;
  }
  if (page.startsWith("81_KEY")) {
    startAudio();
    const b = api.keyButton(x, y);
    if (b === 0 || b === 1) {
      octave += b ? 1 : -1;
      if (octave < -3) octave = -3;
      if (octave > 3) octave = 3;
      status.textContent = "octave " + (octave > 0 ? "+" : "") + octave;
      return;
    }
    const note = api.keyHit(x, y, octave);
    if (note >= 0) {
      noteOff();
      api.note(0, note, 1);
      held = note;
      status.textContent = page + "   note " + note;
    }
  }
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
    knobSet: Module.cwrap("ds10_web_knob_set", null, ["number", "number"]),
    mixerHit: Module.cwrap("ds10_web_mixer_hit", "number",
                           ["number", "number"]),
    mixerSet: Module.cwrap("ds10_web_mixer_set", null, ["number", "number"]),
    fxHit: Module.cwrap("ds10_web_fx_hit", "number",
                        ["number", "number", "number"]),
    seqHit: Module.cwrap("ds10_web_seq_hit", "number", ["number", "number"]),
    seqStep: Module.cwrap("ds10_web_seq_step", null,
                          ["number", "number", "number"]),
    seqNote: Module.cwrap("ds10_web_seq_note", "number",
                          ["number", "number"]),
    seqPlay: Module.cwrap("ds10_web_seq_play", null, ["number"]),
    seqTempo: Module.cwrap("ds10_web_seq_tempo", null, ["number"]),
  };
  if (api.init() < 0) {
    status.textContent = "error: " + api.error();
    return;
  }
  fill(document.getElementById("pick-bottom"), BOTTOM_PAGES, "81_KEY_i");
  fill(document.getElementById("pick-top"), TOP_PAGES, "90_MAP");
  for (const id of ["pick-bottom", "pick-top"])
    document.getElementById(id).addEventListener("change", show);
  for (const which of ["top", "bottom"]) {
    canvases[which].addEventListener("pointerdown", (e) => onDown(which, e));
    canvases[which].addEventListener("pointermove", onMove);
    canvases[which].addEventListener("pointerup", () => { noteOff(); drag = null; });
    canvases[which].addEventListener("pointerleave", () => { noteOff(); drag = null; });
  }
  /* Space starts and stops the sequencer; z and x shift the keyboard by an
   * octave, as the OCT buttons do. */
  document.addEventListener("keydown", (e) => {
    if (e.key === "z") { octave--; status.textContent = "octave " + octave; }
    if (e.key === "x") { octave++; status.textContent = "octave " + octave; }
    if (e.key === " ") {
      e.preventDefault();
      startAudio();
      playing = !playing;
      api.seqPlay(playing ? 1 : 0);
      status.textContent = playing ? "playing" : "stopped";
    }
  });
  show();
};
