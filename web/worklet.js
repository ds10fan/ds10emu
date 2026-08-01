/* The synth engine, running on the audio thread.
 *
 * It used to render on the main thread and queue AudioBuffers ahead of the
 * clock. That competes with the 60 fps canvas loop, and on a phone it lost:
 * the pump kept rendering until it was a fifth of a second ahead, never got
 * there, and locked the page. Here the browser pulls 128 frames at a time and
 * a slow frame elsewhere cannot touch it.
 *
 * The module is the same wasm the page runs, built with no preloaded files -
 * the tables are compiled in and a song arrives over the port - so it needs
 * neither a filesystem nor fetch, neither of which a worklet has.
 *
 * The page keeps its own copy of the engine for editing and drawing; this one
 * exists to make sound. They are kept in step by the page posting the whole
 * song, which is 12 KB, whenever an edit changes it.
 */
import DS10Audio from "./ds10audio.js";

class DS10Processor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.api = null;
    this.pending = [];
    this.cursor = -1;
    this.port.onmessage = (e) => this.handle(e.data);

    DS10Audio().then((M) => {
      const c = (n, r, a) => M.cwrap(n, r, a);
      this.M = M;
      this.api = {
        audioInit: c("ds10_web_audio_init", "number", []),
        songBytes: c("ds10_web_song_bytes", "number", ["number", "number"]),
        render: c("ds10_web_audio", "number", ["number"]),
        buf: c("ds10_web_audio_buf", "number", []),
        rate: c("ds10_web_rate", "number", []),
        note: c("ds10_web_note", null, ["number", "number", "number"]),
        knobSet: c("ds10_web_knob_set", null, ["number", "number"]),
        mixerSet: c("ds10_web_mixer_set", null, ["number", "number"]),
        drumPlay: c("ds10_web_drum_hit_play", null, ["number"]),
        play: c("ds10_web_seq_play", null, ["number"]),
        setPattern: c("ds10_web_set_pattern", null, ["number"]),
        setTrack: c("ds10_web_set_track", null, ["number"]),
        setSongMode: c("ds10_web_set_song_mode", null, ["number"]),
        setTempo: c("ds10_web_set_tempo", null, ["number"]),
        setSwing: c("ds10_web_set_swing", null, ["number"]),
        cursor: c("ds10_web_cursor", "number", []),
        malloc: c("malloc", "number", ["number"]),
        free: c("free", null, ["number"]),
      };
      this.api.audioInit();
      for (const m of this.pending) this.handle(m);
      this.pending = [];
      this.port.postMessage({type: "ready", rate: this.api.rate()});
    }).catch((e) => {
      this.port.postMessage({type: "error", what: String(e && e.message || e)});
    });
  }

  handle(m) {
    if (!this.api) { this.pending.push(m); return; }
    const a = this.api;
    switch (m.type) {
    case "song": {
      /* The page's whole song, after any edit. Cheap enough to send in full:
       * 12 KB, and it keeps the two copies from ever drifting. */
      const p = a.malloc(m.bytes.length);
      this.M.HEAPU8.set(m.bytes, p);
      a.songBytes(p, m.bytes.length);
      a.free(p);
      a.setPattern(m.pattern | 0);
      a.setTrack(m.track | 0);
      a.setSongMode(m.songMode ? 1 : 0);
      a.play(m.playing ? 1 : 0);
      break;
    }
    case "note":       a.note(m.voice | 0, m.note | 0, m.on ? 1 : 0); break;
    case "knob":       a.setTrack(m.track | 0); a.knobSet(m.id, m.value); break;
    case "mixer":      a.mixerSet(m.id, m.value); break;
    case "drum":       a.drumPlay(m.n | 0); break;
    case "play":       a.play(m.on ? 1 : 0); break;
    case "pattern":    a.setPattern(m.n | 0); break;
    case "track":      a.setTrack(m.n | 0); break;
    case "songMode":   a.setSongMode(m.on ? 1 : 0); break;
    case "tempo":      a.setTempo(m.bpm | 0); break;
    case "swing":      a.setSwing(m.percent | 0); break;
    default: break;
    }
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    if (!this.api || !out || !out.length) return true;
    const n = out[0].length;                 /* 128, a whole number of the
                                              * engine's 64-sample frames */
    const got = this.api.render(n);
    if (!got) return true;
    const src = new Float32Array(this.M.HEAPU8.buffer, this.api.buf(), got * 2);
    const l = out[0], r = out.length > 1 ? out[1] : out[0];
    for (let i = 0; i < got && i < n; i++) {
      l[i] = src[i * 2];
      r[i] = src[i * 2 + 1];
    }
    /* Tell the page where the transport is, but only when it moves. */
    const c = this.api.cursor();
    if (c !== this.cursor) {
      this.cursor = c;
      this.port.postMessage({type: "cursor", step: c});
    }
    return true;
  }
}

registerProcessor("ds10", DS10Processor);
