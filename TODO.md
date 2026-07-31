# TODO

## 1. Close the differential suite's blind spot

**This is the top item.** Four bugs have been found by listening, not by the
tests, and all four sat in the same place: the wiring between components that are
individually verified, on the pattern-change and transport path.

| found by ear | what was wrong |
|---|---|
| "garbled noise" | step record is `gate, note, vel, pan`; I had note and gate swapped |
| "doesn't start on the first beat" | pattern install ran after the tick returned instead of inside it |
| "LFO phase isn't reset on pattern start" | never broadcast `0x1D` PATTERN_CHANGE |
| "wrong pitch when the LFO modulates it" | tempo went to the mixer only, so voices kept 120 BPM |

`make test-songrender` cannot catch any of them: it installs **one pattern up
front and loops it**, so a chain walk never happens, `set_pattern` never runs, and
the tempo broadcast is never compared. The suite is strong on DSP and blind on
everything around it.

The fix is to make the harness walk patterns:

- alternate two patterns on the reinstall, going through the original's own
  `set_pattern` (`0x0206DBE4`) rather than poking `seq[0x40]` — that puts the
  `0x1D` broadcast, the patch reload, the mixer knob push and the lane init under
  comparison;
- drive the tempo through `0x0206DA90` instead of writing `seq +0x10` and
  `seq +0x14` directly, so the opcode-4 broadcast is compared;
- give the C side the same player and install hook `songplay` already uses, so
  what is tested is what actually renders.

A second, cheaper guard worth having: assert in the test that every routed event
was consumed by something. Three of the four bugs would have shown up as an
opcode nobody handled.

## 2. Verify against real hardware

`tools/mksong.py` writes valid song files, so a targeted probe is easy and much
easier to judge than a busy track:

- one pattern, one held note, a BPM-synced triangle LFO at known depth on pitch,
  at a deliberately non-120 tempo — settles the LFO rate at a glance;
- a bare saw scale (`tools/mksong.py` already does this) — settles note-to-pitch;
- one drum track on a single patch — settles the offline drum render, which is
  the biggest inferred piece left.

Record from the device, compare against the same file rendered here.

## 3. Still inferred rather than decompiled

- **The offline drum render.** A drum patch becomes a 0x8000-sample buffer, but
  the original produces it by playing into a recorder the sequencer drives
  (`0x0206EEA0`), which is not decompiled. Both sides here render one second from
  a freshly constructed voice at note 60, so the buffer *content* is verified
  against the original and the driving conditions are not.
- **The sequencer tick rate.** 200 calls per second is derived from the
  accumulator arithmetic, not observed; the tick runs on a thread woken by an OS
  event that has not been traced. `ds10demo01` predicts 164.29 s and the renderer
  produces 164.30 s, which is consistent but not proof.
- **The song player.** The chain walk is mine. The original's is at `0x20331E0`,
  reached through the `"stpt"` message, and is not decompiled.

## 4. Not implemented

- **Automation paths other than the per-step latch.** The processor branches on
  track state `+0x20` (1 or 2) and `+0x21` (0..3); only `1` with `0` is done. The
  others consume a pending value from live knob input, which is editing rather
  than playback, and no ROM demo asks for one.
- **The transport start sequence.** `0x0206EC88` posts `24` PLAY to target `0x60`
  and `5` STOP to `0x70`. PLAY resets a voice's LFO phase and zeroes its envelope;
  starting from the constructor state happens to be equivalent, but it is not the
  same code path.
- **Multiplayer.** Out of scope by request.

## 5. Unnamed bytes

Everything round-trips, so nothing is lost - these are just not understood:

- nine bytes per drum at payload `+0x2BA8`, pushed into a second parameter space
  by `0x0206B9DC`;
- the 12-byte per-track records at payload `+0x25C`, apart from bytes 2 and 3
  which are that track's two automation lane knob ids;
- a pattern's `+0x004..0x041` beyond the two 31-knob synth patches.

## 6. GUI

Done:

- the 2D hardware model, pixel-exact against melonDS over 29 screens
  (`make test-2d`);
- the sprite widget's OAM builder, bit-exact against the game's own setter
  under Unicorn (`make test-obj`);
- the asset containers, and the pages drawing natively and in the browser, six
  of them pixel-exact against the real thing (`make test-page`);
- the browser build plays: the keyboard drives the decompiled synth engine at
  the right pitch, and the synth edit page's knobs turn, through the engine's
  own dispatch and the ROM's own knob ranges.

Remaining, roughly in order:

- **The widget constructors.** `data/pages.bin` is still a captured frame of
  sprites per page rather than generated ones, so a page draws but does not
  animate. The OAM builder underneath is decompiled and verified; what is
  missing is each screen's list of widgets. Scanning main RAM for the objects
  finds them but not the ones that are live: the positions a widget holds are
  in a parent's coordinate space, and the pass that copies a widget's private
  buffer into the shadow OAM at runtime has not been read yet. That pass is the
  next thing to find - `tools/widgets.py` already rebuilds any candidate and
  checks it, so once the transform is known the layouts fall out.
- **The 3D text layer.** Captions are textured quads through the geometry
  engine, orthographic and axis-aligned, from `3D_TEX` and `3D_MESS_TEX`. A
  small textured-quad rasteriser, not a polygon pipeline.
- **The rest of the behaviour.** The map's boxes, the keyboard's keys and the
  synth edit knobs are hit-tested from coordinates measured off the rendered
  screen, which is pixel-exact, rather than from the game's own hit-test code.
  Everything else - the sequencer grid, the mixer, the patch browser, the song
  arranger - is unwired.
- **Which octave the keyboard starts on.** The layout says it starts on an F;
  F3 is an assumption, not a reading.

## 7. Longer term

- **Fuzz the vector corpus.** Coverage is only what I sweep, and the inputs are
  chosen by my understanding of the code. Random patches, notes and timings would
  attack that bias. Offered several times, never done.
