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

const MIXER_NAME = [
  "FX TYPE", "FX 1", "FX 2", "FX 3", "FX 4", "FX 5", "FX 6", "FX 7", "FX 8",
  "PAN SYN1", "PAN SYN2", "PAN DRUM1", "PAN DRUM2", "PAN DRUM3", "PAN DRUM4",
  "MIX 15", "MIX 16",
  "VOL SYN1", "VOL SYN2", "VOL DRUM1", "VOL DRUM2", "VOL DRUM3", "VOL DRUM4",
  "MIX 23", "MIX 24",
];
const NOTE_NAME_P = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A",
                     "A#", "B"];
const GATE_NAME = ["", "25%", "50%", "75%", "100%", "LEGATO", "RETRIG"];

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
    onchange: () => { if (commit) commit(); },
  });
  r.dataset.sync = "1";
  r.sync = () => { r.value = String(get()); out.textContent = r.value; };
  return el("label", {cls: "ctl"}, [el("span", {cls: "nm", text: name}), r,
                                    out]);
}

function button(name, fn, id) {
  return el("button", id ? {text: name, onclick: fn, id: id}
                         : {text: name, onclick: fn});
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
  host.appendChild(group("transport", [
    playBtn, modeBtn, bars,
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
    button("copy to next", () => {
      const to = (api.pattern() + 1) % 16;
      api.patternCopy(api.pattern(), to);
      sendSong();
      panelSync();
    }),
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

  /* --- mixer and effect ------------------------------------------------ */
  const mix = [];
  for (let m = 0; m < 25; m++)
    mix.push(slider(MIXER_NAME[m], 0, 127, () => api.mixerGet(m),
                    (v) => {
                      api.mixerSet(m, v);
                      send({type: "mixer", id: m, value: v});
                    },
                    sendSong));
  host.appendChild(group("mixer and effect", mix));

  /* --- drums ------------------------------------------------------------ */
  const drums = [];
  for (let d = 0; d < 4; d++)
    drums.push(button("DRUM " + (d + 1), () => send({type: "drum", n: d})));
  host.appendChild(group("drums", [
    el("div", {cls: "hint", text: "audition"}),
    el("div", {cls: "row"}, drums),
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
  host.appendChild(group("keyboard", [el("div", {cls: "keys"}, keys)]));

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
  syncers.push(rebuildChain);
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
