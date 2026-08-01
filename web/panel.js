/* Every capability, as plain HTML.
 *
 * The DS screens are the goal, but a control there can fail three ways that
 * look identical from outside: a wrong hit box, a broken function underneath,
 * or a browser event that never arrived. This surface removes two of them.
 * Each control calls exactly the entry point the canvas handler calls, so
 * binding a coordinate later is additive.
 *
 * Everything is generated rather than written out, and every control reads
 * its value back from the C side, so the panel cannot drift from the song.
 */
"use strict";

/* The mixer's twenty-five knobs. 0 is which channels are routed through the
 * effect and 1..8 are the effect's own parameters - the mixer forwards those
 * straight to it. 9..14 are the pans and 17..22 the volumes. */
const MIX_ROUTE = 0, MIX_TYPE = 1, MIX_TIME = 2, MIX_RATIO = 3;
const MIX_LFO = 4, MIX_DEPTH = 5, MIX_SYNC = 6, MIX_FB = 7, MIX_DRYWET = 8;
const ROUTE_NAME = ["SYN 1", "SYN 2", "SYN 1+2", "DRUMS", "ALL", "OFF"];
const FX_TYPE_NAME = ["DELAY", "FLANGER", "CHORUS"];
/* Which three parameters each type shows, from 52_FX's own table: the page
 * puts them at the same four positions and swaps which are live. */
const FX_PARAMS = [
  [[MIX_TIME, "DELAY TIME"], [MIX_RATIO, "L/R RATIO"], [MIX_FB, "FEEDBACK"]],
  [[MIX_LFO, "LFO FREQ"], [MIX_DEPTH, "DEPTH"]],
  [[MIX_LFO, "LFO FREQ"], [MIX_DEPTH, "DEPTH"], [MIX_FB, "FEEDBACK"]],
];
const MIXER_NAME = [
  "FX ROUTE", "FX TYPE", "DELAY TIME", "L/R RATIO", "LFO FREQ", "DEPTH",
  "SYNC", "FEEDBACK", "DRY/WET",
  "PAN SYN1", "PAN SYN2", "PAN DRUM1", "PAN DRUM2", "PAN DRUM3", "PAN DRUM4",
  "MIX 15", "MIX 16",
  "VOL SYN1", "VOL SYN2", "VOL DRUM1", "VOL DRUM2", "VOL DRUM3", "VOL DRUM4",
  "MIX 23", "MIX 24",
];
const NOTE_NAME_P = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A",
                     "A#", "B"];
const GATE_NAME = ["", "25%", "50%", "75%", "100%", "LEGATO", "RETRIG"];

/* The pad's scales.
 *
 * The names are the ROM's own, read out of the string table at 0x02084e54 -
 * all thirty-one of them, in order. The intervals are not: the ROM does not
 * keep them as plain runs and I could not find the table, so these are the
 * conventional sets. The seven modes, the pentatonics, the blues scales,
 * whole tone and the interval-only ones are unambiguous; RAGA1 to RAGA3 are
 * KORG's own naming and those three are guesses.
 */
const SCALES = [
  ["CHROMA", [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]],
  ["IONIAN", [0, 2, 4, 5, 7, 9, 11]],
  ["DORIAN", [0, 2, 3, 5, 7, 9, 10]],
  ["PHRYGI", [0, 1, 3, 5, 7, 8, 10]],
  ["LYDIAN", [0, 2, 4, 6, 7, 9, 11]],
  ["MIXLYD", [0, 2, 4, 5, 7, 9, 10]],
  ["AEOLIA", [0, 2, 3, 5, 7, 8, 10]],
  ["LOCRIA", [0, 1, 3, 5, 6, 8, 10]],
  ["MBLUES", [0, 2, 3, 4, 7, 9]],
  ["mBLUES", [0, 3, 5, 6, 7, 10]],
  ["DIM", [0, 2, 3, 5, 6, 8, 9, 11]],
  ["COMDIM", [0, 1, 3, 4, 6, 7, 9, 10]],
  ["MPENTA", [0, 2, 4, 7, 9]],
  ["mPENTA", [0, 3, 5, 7, 10]],
  ["RAGA1", [0, 1, 4, 5, 7, 8, 11]],
  ["RAGA2", [0, 2, 3, 6, 7, 9, 10]],
  ["RAGA3", [0, 1, 3, 6, 7, 8, 11]],
  ["SPANISH", [0, 1, 3, 4, 5, 7, 8, 10]],
  ["GYPSY", [0, 2, 3, 6, 7, 8, 11]],
  ["ARABIA", [0, 1, 4, 5, 7, 8, 11]],
  ["EGYPT", [0, 2, 5, 7, 10]],
  ["HAWAII", [0, 2, 3, 7, 9]],
  ["PELOG", [0, 1, 3, 7, 8]],
  ["JAPAN", [0, 1, 5, 7, 8]],
  ["RYUKYU", [0, 4, 5, 7, 11]],
  ["WHOLE", [0, 2, 4, 6, 8, 10]],
  ["m3RD", [0, 3]],
  ["M3RD", [0, 4]],
  ["4TH", [0, 5]],
  ["5TH", [0, 7]],
  ["OCTAVE", [0]],
];
const KEY_NAME = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#",
                  "B"];
/* OFF, HLD and SMT, as the pad's own button cycles them. HLD applies once a
 * step; SMT applies continuously. */
const PAD_RATE = ["OFF", "HLD", "SMT"];

let panelSync = () => {};

function el(tag, attrs, kids) {
  const e = document.createElement(tag);
  for (const k in (attrs || {})) {
    if (k === "text") e.textContent = attrs[k];
    else if (k === "cls") e.className = attrs[k];
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), attrs[k]);
    else e.setAttribute(k, attrs[k]);
  }
  for (const c of kids || []) e.appendChild(c);
  return e;
}

function group(title, kids) {
  return el("details", {open: ""}, [el("summary", {text: title})].concat(
    [el("div", {cls: "panel-body"}, kids)]));
}

/* A labelled slider that reads and writes through the C side.
 *
 * `set` runs on every movement and must be cheap - it is the live sound.
 * `commit` runs once when the finger lifts, for anything expensive: posting
 * the whole song to the worklet on every pixel of a drag reloaded its engine
 * continuously, which reset the voices and cut whatever was sounding.
 */
function slider(name, lo, hi, get, set, commit) {
  const out = el("span", {cls: "val", text: String(get())});
  const r = el("input", {
    type: "range", min: String(lo), max: String(hi), value: String(get()),
    oninput: (e) => { set(+e.target.value); out.textContent = e.target.value; },
    onchange: () => { trace("panel: " + name + " = " + r.value);
                      if (commit) commit(); },
  });
  r.dataset.sync = "1";
  r.sync = () => { r.value = String(get()); out.textContent = r.value; };
  return el("label", {cls: "ctl"}, [el("span", {cls: "nm", text: name}), r,
                                    out]);
}

/* Traced, so the log says which control was used. The canvas records every
 * touch; without this the panel was the one surface that went unrecorded, and
 * a transport that stopped on its own could not be told from one the user
 * stopped. */
/* A switch, for the settings that are one bit rather than a range. */
function toggle(name, get, set) {
  const b = button(name, () => { set(get() ? 0 : 1); panelSync(); });
  b.sync = () => b.classList.toggle("on", !!get());
  return b;
}

function button(name, fn, id) {
  const wrapped = (e) => { trace("panel: " + name); fn(e); };
  return el("button", id ? {text: name, onclick: wrapped, id: id}
                         : {text: name, onclick: wrapped});
}

function buildPanel() {
  const host = document.getElementById("panel");
  if (!host) return;
  const syncers = [];
  const track = () => (api.track() < 2 ? api.track() : 0);

  /* --- transport ------------------------------------------------------ */
  const playBtn = button("play", () => { transport(!playing); panelSync(); });
  const modeBtn = button("pattern mode", () => {
    api.setSongMode(!api.songMode());
    send({type: "songMode", on: !!api.songMode()});
    panelSync();
  });
  const bars = el("span", {cls: "val", text: ""});
  syncers.push(() => {
    playBtn.textContent = playing ? "stop" : "play";
    playBtn.classList.toggle("on", playing);
    modeBtn.textContent = api.songMode() ? "song mode" : "pattern mode";
    modeBtn.classList.toggle("on", !!api.songMode());
    bars.textContent = api.songBars() + " bars";
  });
  const instBtn = button("at the next bar", () => {
    api.setInstantPattern(!api.instantPattern());
    send({type: "instant", on: !!api.instantPattern()});
    panelSync();
  });
  syncers.push(() => {
    const on = !!api.instantPattern();
    instBtn.textContent = on ? "pattern changes now" : "at the next bar";
    instBtn.classList.toggle("on", on);
  });
  const recBtn = button("record", () => {
    api.setRecord(!api.record());
    send({type: "record", on: !!api.record()});
    panelSync();
  });
  syncers.push(() => {
    const on = !!api.record();
    recBtn.textContent = on ? "recording" : "record";
    recBtn.classList.toggle("rec", on);
  });
  host.appendChild(group("transport", [
    playBtn, recBtn, modeBtn, bars, instBtn,
    el("div", {cls: "hint", text: "with the song playing, what you play goes "
                                  + "into the pattern passing under it"}),
    slider("tempo", 20, 300, () => api.getTempo(),
           (v) => { api.setTempo(v); send({type: "tempo", bpm: v}); }),
    slider("swing", 50, 75, () => api.getSwing(),
           (v) => { api.setSwing(v); send({type: "swing", percent: v}); }),
  ]));

  /* --- pattern and track ---------------------------------------------- */
  const patBtns = [];
  for (let i = 0; i < 16; i++) {
    const b = button(String(i + 1), () => {
      api.setPattern(i);
      send({type: "pattern", n: i});
      api.seqFollow();
      panelSync();
    });
    patBtns.push(b);
  }
  const trkBtns = [];
  const TRACK_NAME = ["SYN 1", "SYN 2", "DRUM 1", "DRUM 2", "DRUM 3",
                      "DRUM 4"];
  for (let i = 0; i < 6; i++) {
    const b = button(TRACK_NAME[i], () => {
      api.setTrack(i);
      send({type: "track", n: i});
      api.seqFollow();
      panelSync();
    });
    trkBtns.push(b);
  }
  syncers.push(() => {
    patBtns.forEach((b, i) => b.classList.toggle("on", i === api.pattern()));
    trkBtns.forEach((b, i) => b.classList.toggle("on", i === api.track()));
  });
  host.appendChild(group("pattern", [
    el("div", {cls: "grid16"}, patBtns),
    el("div", {cls: "row"}, trkBtns),
    slider("length", 1, 16, () => api.patternSteps(),
           (v) => { api.setPatternSteps(v); sendSong(); }),
    button("clear pattern", () => {
      api.patternClear(-1);
      sendSong();
      panelSync();
    }),
    el("div", {cls: "row"}, [
      button("copy to next", () => {
        api.patternCopy(api.pattern(), (api.pattern() + 1) % 16);
        sendSong();
        panelSync();
      }),
      button("clear this track", () => {
        api.trackClear(api.track());
        sendSong();
        panelSync();
      }),
      button("oct -", () => { api.transpose(api.track(), -12); sendSong();
                              panelSync(); }),
      button("oct +", () => { api.transpose(api.track(), 12); sendSong();
                              panelSync(); }),
      button("semi -", () => { api.transpose(api.track(), -1); sendSong();
                               panelSync(); }),
      button("semi +", () => { api.transpose(api.track(), 1); sendSong();
                               panelSync(); }),
    ]),
  ]));

  /* --- the sixteen steps of the selected track ------------------------ */
  const stepBtns = [], gateSel = [], velIn = [], panIn = [];
  for (let s = 0; s < 16; s++) {
    const b = button("", () => {
      const t = api.track();
      const n = api.seqNote(t, s);
      api.seqStep(t, s, n >= 0 ? -1 : (t < 2 ? 60 : 60));
      sendSong();
      panelSync();
    });
    stepBtns.push(b);
  }
  const stepDetail = el("div", {cls: "steps"});
  for (let s = 0; s < 16; s++) {
    const note = el("input", {type: "number", min: "-1", max: "127",
      onchange: (e) => {
        api.seqStep(api.track(), s, +e.target.value);
        sendSong();
        panelSync();
      }});
    const gate = el("select", {onchange: (e) => {
      api.setStepGate(api.track(), s, +e.target.value);
      sendSong();
    }});
    for (let g = 1; g <= 6; g++)
      gate.appendChild(el("option", {value: String(g), text: GATE_NAME[g]}));
    const vel = el("input", {type: "number", min: "0", max: "127",
      onchange: (e) => {
        api.setStepVel(api.track(), s, +e.target.value);
        sendSong();
      }});
    const pan = el("input", {type: "number", min: "0", max: "127",
      onchange: (e) => {
        api.setStepPan(api.track(), s, +e.target.value);
        sendSong();
      }});
    gateSel.push(gate); velIn.push(vel); panIn.push(pan);
    stepDetail.appendChild(el("div", {cls: "step"}, [
      el("span", {cls: "nm", text: String(s + 1)}), note, gate, vel, pan,
    ]));
  }
  syncers.push(() => {
    const t = api.track();
    for (let s = 0; s < 16; s++) {
      const n = api.seqNote(t, s);
      stepBtns[s].textContent = n < 0 ? "·"
        : NOTE_NAME_P[n % 12] + (Math.floor(n / 12) - 1);
      stepBtns[s].classList.toggle("on", n >= 0);
      stepBtns[s].classList.toggle("cursor", s === cursor);
      const d = stepDetail.children[s];
      d.children[1].value = String(n);
      gateSel[s].value = String(api.stepGate(t, s));
      velIn[s].value = String(api.stepVel(t, s));
      panIn[s].value = String(api.stepPan(t, s));
    }
  });
  host.appendChild(group("steps", [
    el("div", {cls: "grid16"}, stepBtns),
    el("div", {cls: "hint", text: "note / gate / velocity / pan"}),
    stepDetail,
  ]));

  /* --- the synth's thirty-one knobs ----------------------------------- */
  const knobs = [];
  for (let k = 0; k < 31; k++) {
    const lo = api.knobMin(k), n = api.knobRange(k);
    knobs.push(slider(api.knobName(k) || ("knob " + k), lo, lo + n - 1,
                      () => api.knobGet(k),
                      (v) => {
                        api.knobSet(k, v);
                        send({type: "knob", track: track(), id: k, value: v});
                      },
                      sendSong));
  }
  host.appendChild(group("synth", [
    el("div", {cls: "hint",
               text: "the selected track's patch; both synths have their own"}),
  ].concat(knobs)));

  /* --- mixer ----------------------------------------------------------- */
  const mixCtl = (m) => slider(MIXER_NAME[m], 0, 127, () => api.mixerGet(m),
                               (v) => {
                                 api.mixerSet(m, v);
                                 send({type: "mixer", id: m, value: v});
                               },
                               sendSong);
  const mix = [];
  for (let m = 17; m <= 22; m++) mix.push(mixCtl(m));
  for (let m = 9; m <= 14; m++) mix.push(mixCtl(m));
  host.appendChild(group("mixer", mix));

  /* --- the effect ------------------------------------------------------- */
  const routeBtns = [], typeBtns = [];
  const setMix = (id, v) => {
    api.mixerSet(id, v);
    send({type: "mixer", id: id, value: v});
    sendSong();
  };
  for (let r = 0; r < 6; r++)
    routeBtns.push(button(ROUTE_NAME[r], () => { setMix(MIX_ROUTE, r);
                                                 panelSync(); }));
  for (let t = 0; t < 3; t++)
    typeBtns.push(button(FX_TYPE_NAME[t], () => { setMix(MIX_TYPE, t);
                                                  panelSync(); }));
  /* Rebuilt on a type change, because which three are live depends on it. */
  const fxParams = el("div", {cls: "panel-body"});
  const rebuildFx = () => {
    fxParams.textContent = "";
    const t = Math.max(0, Math.min(2, api.mixerGet(MIX_TYPE)));
    for (const [id, name] of FX_PARAMS[t])
      fxParams.appendChild(slider(name, 0, 127, () => api.mixerGet(id),
                                  (v) => { api.mixerSet(id, v);
                                           send({type: "mixer", id: id,
                                                 value: v}); },
                                  sendSong));
    /* SYNC is a switch, not a range: only 0 and 1 appear anywhere in the
     * ROM's songs, and the dispatcher passes the song's tempo when it is on
     * and literally zero when it is off. It only does anything for a delay -
     * the setter returns early for the other two types - so it is only
     * offered there. */
    if (t === 0) {
      const sw = toggle("SYNC to the song's BPM",
                        () => api.mixerGet(MIX_SYNC),
                        (v) => setMix(MIX_SYNC, v));
      sw.sync();
      fxSync = sw;
      fxParams.appendChild(sw);
    } else {
      fxSync = null;
    }
    fxParams.appendChild(mixCtl(MIX_DRYWET));
  };
  /* Only when the type changes: this rebuilds sliders, and the sync runs on
   * every touch. */
  let shownType = -1, fxSync = null;
  syncers.push(() => {
    const r = api.mixerGet(MIX_ROUTE), t = api.mixerGet(MIX_TYPE);
    routeBtns.forEach((b, i) => b.classList.toggle("on", i === r));
    typeBtns.forEach((b, i) => b.classList.toggle("on", i === t));
    if (t !== shownType) { shownType = t; rebuildFx(); }
    if (fxSync) fxSync.sync();
  });
  host.appendChild(group("effect", [
    el("div", {cls: "hint", text: "which channels go through it"}),
    el("div", {cls: "row"}, routeBtns),
    el("div", {cls: "hint", text: "type"}),
    el("div", {cls: "row"}, typeBtns),
    fxParams,
  ]));

  /* --- the KAOSS pad ---------------------------------------------------- */
  /* Three modes, as the page's own 1/2/3 buttons offer. Each is a pair of
   * knobs; selecting one writes it into the pattern's lane assignments, which
   * is where the song keeps it and what a recording follows.
   *
   * The defaults are the three commonest pairs in the ROM's songs and the
   * saves - cutoff and peak on 3289 patterns, well ahead of anything else.
   * The slots themselves are the page's; the format has room for one pair per
   * pattern, not three. */
  const PAD_SLOT = [
    {note: false, xy: [11, 12], key: 0, scale: 1},
    {note: false, xy: [13, 24], key: 0, scale: 1},
    {note: true, xy: [11, 12], key: 0, scale: 12},
  ];
  let padSlot = 0, padRate = 2, padLastStep = -1;
  const slot = () => PAD_SLOT[padSlot];
  const slotBtns = [];
  for (let i = 0; i < 3; i++)
    slotBtns.push(button(String(i + 1), () => {
      padSlot = i;
      if (!PAD_SLOT[i].note) {
        api.setLaneKnob(0, PAD_SLOT[i].xy[0]);
        api.setLaneKnob(1, PAD_SLOT[i].xy[1]);
        sendSong();
      }
      panelSync();
    }));
  const noteBtn = button("note / gate", () => {
    slot().note = !slot().note;
    if (!slot().note) {
      api.setLaneKnob(0, slot().xy[0]);
      api.setLaneKnob(1, slot().xy[1]);
      sendSong();
    }
    panelSync();
  });
  const rateBtn = button("SMT", () => {
    padRate = (padRate + 1) % 3;
    panelSync();
  });
  /* SET: the key and the scale the note mode quantises to. */
  const keySel = el("select", {onchange: (e) => { slot().key = +e.target.value; }});
  for (let k = 0; k < 12; k++)
    keySel.appendChild(el("option", {value: String(k), text: KEY_NAME[k]}));
  const scaleSel = el("select", {onchange: (e) => {
    slot().scale = +e.target.value;
  }});
  for (let i = 0; i < SCALES.length; i++)
    scaleSel.appendChild(el("option", {value: String(i), text: SCALES[i][0]}));
  const laneSel = [];
  for (let lane = 0; lane < 2; lane++) {
    const sel = el("select", {onchange: (e) => {
      api.setLaneKnob(lane, +e.target.value);
      PAD_SLOT[padSlot][lane] = +e.target.value;   /* the slot remembers it */
      sendSong();
      panelSync();
    }});
    for (let k = 0; k < 31; k++)
      sel.appendChild(el("option", {value: String(k),
                                    text: api.knobName(k) || ("knob " + k)}));
    laneSel.push(sel);
  }
  const padRead = el("span", {cls: "val", text: ""});
  const pad = el("div", {cls: "pad"});
  const padSet = (ev) => {
    const r = pad.getBoundingClientRect();
    const fx = Math.max(0, Math.min(1, (ev.clientX - r.left) / r.width));
    const fy = Math.max(0, Math.min(1, (ev.clientY - r.top) / r.height));
    const vx = Math.round(fx * 127), vy = Math.round((1 - fy) * 127);
    pad.style.backgroundPosition = (fx * 100) + "% " + (fy * 100) + "%";

    /* OFF does nothing, HLD once a step, SMT on every movement. */
    if (padRate === 0) return;
    if (padRate === 1) {
      if (cursor === padLastStep) return;
      padLastStep = cursor;
    }

    if (slot().note) {
      /* Across the pad is the scale, up it is the gate. */
      const sc = SCALES[slot().scale][1];
      const span = sc.length * 2;                  /* two octaves of it */
      const i = Math.min(span - 1, Math.floor(fx * span));
      const n = 48 + slot().key + sc[i % sc.length] + 12 * Math.floor(i / sc.length);
      const gate = 1 + Math.min(4, Math.floor((1 - fy) * 5));
      if (n !== padNote) {
        if (padNote >= 0)
          send({type: "note", voice: track(), note: padNote, on: 0});
        send({type: "note", voice: track(), note: n, on: 1});
        padNote = n;
      }
      padRead.textContent = NOTE_NAME_P[n % 12] + (Math.floor(n / 12) - 1)
                          + "   gate " + GATE_NAME[gate];
      return;
    }
    const kx = api.laneKnob(0), ky = api.laneKnob(1);
    /* Clamp into each knob's own range, as the knob setter would. */
    const put = (k, v) => {
      const lo = api.knobMin(k), n = api.knobRange(k);
      const val = Math.max(lo, Math.min(lo + n - 1,
                                        lo + Math.round(v * n / 127)));
      api.knobSet(k, val);
      send({type: "knob", track: track(), id: k, value: val});
      return val;
    };
    padRead.textContent = api.knobName(kx) + " " + put(kx, vx) + "   "
                        + api.knobName(ky) + " " + put(ky, vy);
    /* And into the automation lanes, if recording. */
    send({type: "lane", lane: 0, value: vx});
    send({type: "lane", lane: 1, value: vy});
  };
  let padNote = -1;
  let padDown = false;
  pad.addEventListener("pointerdown", (e) => {
    padDown = true;
    startAudio();
    pad.setPointerCapture(e.pointerId);
    padSet(e);
  });
  pad.addEventListener("pointermove", (e) => { if (padDown) padSet(e); });
  for (const ev of ["pointerup", "pointercancel"])
    pad.addEventListener(ev, () => {
      if (!padDown) return;
      padDown = false;
      padLastStep = -1;
      if (padNote >= 0) {
        send({type: "note", voice: track(), note: padNote, on: 0});
        padNote = -1;
      }
      sendSong();
    });
  syncers.push(() => {
    laneSel[0].value = String(api.laneKnob(0));
    laneSel[1].value = String(api.laneKnob(1));
    slotBtns.forEach((b, i) => b.classList.toggle("on", i === padSlot));
    noteBtn.classList.toggle("on", slot().note);
    rateBtn.textContent = PAD_RATE[padRate];
    rateBtn.classList.toggle("on", padRate !== 0);
    keySel.value = String(slot().key);
    scaleSel.value = String(slot().scale);
  });
  host.appendChild(group("kaoss pad", [
    el("div", {cls: "hint", text: "three modes, each a pair of knobs"}),
    el("div", {cls: "row"}, slotBtns),
    el("div", {cls: "hint", text: "the two the pattern's automation lanes "
                                  + "drive; recorded while record is on"}),
    el("div", {cls: "row"}, [noteBtn, rateBtn]),
    el("label", {cls: "ctl"}, [el("span", {cls: "nm", text: "x"}), laneSel[0]]),
    el("label", {cls: "ctl"}, [el("span", {cls: "nm", text: "y"}), laneSel[1]]),
    el("div", {cls: "hint", text: "note mode: across is the scale, up is "
                                  + "the gate"}),
    el("label", {cls: "ctl"}, [el("span", {cls: "nm", text: "key"}), keySel]),
    el("label", {cls: "ctl"}, [el("span", {cls: "nm", text: "scale"}),
                               scaleSel]),
    pad, padRead,
  ]));

  /* --- drums ------------------------------------------------------------ */
  const drums = [];
  for (let d = 0; d < 4; d++)
    drums.push(button("DRUM " + (d + 1), () => send({type: "drum", n: d})));
  /* Each drum has its own 31-knob patch, kept in the song's tail rather than
   * per pattern, so it sounds the same all the way through. Editing one means
   * its one-second buffer has to be rendered again. */
  let editDrum = 0;
  const drumSel = [];
  for (let d = 0; d < 4; d++)
    drumSel.push(button("edit " + (d + 1), () => {
      editDrum = d;
      panelSync();
    }));
  const drumKnobs = el("div", {cls: "panel-body"});
  const rebuildDrum = () => {
    drumKnobs.textContent = "";
    for (let k = 0; k < 31; k++) {
      const lo = api.knobMin(k), n = api.knobRange(k);
      drumKnobs.appendChild(
        slider(api.knobName(k) || ("knob " + k), lo, lo + n - 1,
               () => api.drumKnob(editDrum, k),
               (v) => api.setDrumKnob(editDrum, k, v),
               () => { api.drumsRerender(); sendSong();
                       send({type: "drum", n: editDrum}); }));
    }
  };
  let shownDrum = -1;
  syncers.push(() => {
    drumSel.forEach((b, i) => b.classList.toggle("on", i === editDrum));
    if (editDrum !== shownDrum) { shownDrum = editDrum; rebuildDrum(); }
  });
  host.appendChild(group("drums", [
    el("div", {cls: "hint", text: "audition"}),
    el("div", {cls: "row"}, drums),
    el("div", {cls: "hint", text: "which drum the knobs below edit"}),
    el("div", {cls: "row"}, drumSel),
    drumKnobs,
  ]));

  /* --- keyboard --------------------------------------------------------- */
  const keys = [];
  for (let n = 48; n < 84; n++) {
    const name = NOTE_NAME_P[n % 12] + (Math.floor(n / 12) - 1);
    const b = el("button", {
      text: name,
      cls: name.length > 2 ? "black" : "",
      onpointerdown: () => {
        startAudio();
        send({type: "note", voice: track(), note: n, on: 1});
      },
      onpointerup: () => send({type: "note", voice: track(), note: n, on: 0}),
      onpointerleave: () => send({type: "note", voice: track(), note: n,
                                  on: 0}),
    });
    keys.push(b);
  }
  const octLabel = el("span", {cls: "val", text: "0"});
  syncers.push(() => { octLabel.textContent = (octave > 0 ? "+" : "")
                                              + octave; });
  host.appendChild(group("keyboard", [
    el("div", {cls: "row"}, [
      button("oct -", () => { setOctave(octave - 1); panelSync(); }),
      button("oct +", () => { setOctave(octave + 1); panelSync(); }),
      octLabel,
      el("span", {cls: "hint", text: "shifts the DS keyboard page"}),
    ]),
    el("div", {cls: "keys"}, keys),
  ]));

  /* --- the song --------------------------------------------------------- */
  const nameIn = el("input", {maxlength: "6", onchange: (e) => {
    api.setName(e.target.value);
    sendSong();
    panelSync();
  }});
  const songSel = el("select", {onchange: (e) => {
    trace("panel: load " + e.target.value);
    loadSong(e.target.value);
    panelSync();
  }});
  for (const n of SONGS)
    songSel.appendChild(el("option", {value: n, text: n}));
  /* And whatever is in songs/*.SAV, by the names they were saved under. */
  if (typeof SAVED_SONGS !== "undefined")
    for (const [file, label] of SAVED_SONGS)
      songSel.appendChild(el("option", {value: file, text: label}));
  const patchSel = el("select", {onchange: (e) => {
    const n = +e.target.value;
    if (n < 0) return;
    trace("panel: factory patch " + n);
    if (api.factoryPatch(n) === 0) { sendSong(); panelSync(); }
    e.target.value = "-1";
  }});
  patchSel.appendChild(el("option", {value: "-1", text: "factory patch…"}));
  for (let n = 0; n < 24; n++)
    patchSel.appendChild(el("option", {value: String(n),
                                       text: "synth " + (n + 1)}));
  const fileIn = el("input", {type: "file", accept: ".dat",
                              onchange: (e) => openFile(e)});
  syncers.push(() => { nameIn.value = api.getName(); });
  host.appendChild(group("song", [
    el("label", {cls: "ctl"}, [el("span", {cls: "nm", text: "name"}), nameIn]),
    el("label", {cls: "ctl"}, [el("span", {cls: "nm", text: "load"}), songSel]),
    el("label", {cls: "ctl"}, [el("span", {cls: "nm", text: "patch"}),
                               patchSel]),
    el("label", {cls: "ctl"}, [el("span", {cls: "nm", text: "open"}), fileIn]),
    button("save .dat", () => save()),
  ]));

  /* --- the arrangement -------------------------------------------------- */
  const chain = el("div", {cls: "row"});
  const rebuildChain = () => {
    chain.textContent = "";
    const n = api.songBars();
    for (let b = 0; b < n; b++) {
      const sel = el("select", {onchange: (e) => {
        api.chainSet(b, +e.target.value);
        sendSong();
        panelSync();
      }});
      for (let p = 0; p < 16; p++)
        sel.appendChild(el("option", {value: String(p), text: String(p + 1)}));
      sel.value = String(api.chainGet(b));
      chain.appendChild(sel);
    }
    chain.appendChild(button("+", () => {
      api.chainSet(api.songBars(), api.pattern());
      sendSong();
      panelSync();
    }));
    if (n > 0)
      chain.appendChild(button("−", () => {
        api.chainRemove(n - 1);
        sendSong();
        panelSync();
      }));
  };
  let shownBars = -1, shownPat = -1;
  syncers.push(() => {
    /* Bars are selects; rebuilding them all on every touch is wasteful and
     * would drop a half-made choice. */
    const n = api.songBars();
    if (n !== shownBars || api.pattern() !== shownPat) {
      shownBars = n;
      shownPat = api.pattern();
      rebuildChain();
    }
  });
  host.appendChild(group("arrangement", [
    el("div", {cls: "hint", text: "which pattern each bar plays"}), chain,
  ]));

  panelSync = () => { for (const f of syncers) f(); };
  panelSync();
  /* The step cursor moves with the transport, so keep that one live. */
  setInterval(() => {
    const t = api.track();
    for (let s = 0; s < 16; s++)
      stepBtns[s].classList.toggle("cursor", s === cursor);
    void t;
  }, 60);
}
