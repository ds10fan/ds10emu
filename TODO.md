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
  own dispatch and the ROM's own knob ranges;
- **the browser build is an editor.** It works on a `ds10_song` - the same
  record the ROM's files are a byte image of - starting from the ROM's own
  blank template `ds10init.dat`. Notes go in on the piano roll and the drum
  grid through the decompiled hit tests, appear on the screen where the
  hardware puts them, sound as they are entered, and save as a `.dat` the
  hardware would load.

  `make test-webedit` is the end-to-end check: it enters a song through the
  grid's own hit tests, checks exactly the right cell lights, watches the play
  cursor walk, hears the pattern and hears silence after clearing it, exports,
  loads the file back through the game's parser, and renders the same file with
  `songplay` - which shares none of the browser plumbing and is itself
  bit-exact against the original. They agree sample for sample.

Remaining, roughly in order:

- **What drives the display lists.** `data/pages.bin` is now a display list
  per page - which cell, where, with what matrix - and the sprites are
  generated from it by the decompiled expander rather than replayed. Six pages
  come out pixel-exact that way, and the synth edit knobs turn because their
  cell's matrix is ours to set.

  What is still missing is the code that *produces* those lists: each screen's
  constructor and its per-frame update. The lists here were recovered from one
  captured frame per page (`tools/pages.py` inverts the expansion), so a page
  shows the state it was captured in until something is wired to change it.
  All twelve are now read and listed in `recon.md` §16 with their geometry
  tables. A constructor is a walk over a table, one widget per entry. Nine of
  the screens have their controls wired.

  What that does *not* give is the per-frame update: what each screen redraws
  when its state changes. The display lists still come from a captured frame,
  so a page shows the state it was captured in except where something is wired
  to move it.

- **Text.** `90_MAP`'s unattributed sprites are text - the session name, the
  tempo, the step count. Their tiles are past the end of CHARACTER_S and match
  no asset, and the top screen's OBJ VRAM holds 125 KB of data only 13 KB of
  which is a file: the glyphs are rasterised into VRAM at runtime from the
  NFTR fonts. That means an NFTR renderer and a glyph cache, not a lookup.


- **The 3D text layer.** Captions are textured quads through the geometry
  engine, orthographic and axis-aligned, from `3D_TEX` and `3D_MESS_TEX`. A
  small textured-quad rasteriser, not a polygon pipeline.
- **The screens' redraw passes.** The per-frame update's shape is decompiled
  (`recon.md` §16): a four-state machine whose running state takes the widget
  under the pen from the global registry, maps its name to a knob, pushes the
  value into the machine and redraws. Both widget redraws are decompiled too -
  the knob's sweep and the slider's span.

  The mixer's two are read (`recon.md` §16): one drives the level meters from
  the sequencer's step velocities, the other handles the mute and solo rows.
  Its fader handles move with their volume.

  60_SYN_SEQ's (`0x02051074`) and 33_DRUM_SELECTER's (`0x02057f74`) are read
  too - the first dispatches on a sub-mode, one handler per page variant; the
  second commits a drum choice and re-renders the drum buffers.

  62_DRUM_SEQ's (`0x0205a384`) and the settings screen's (`0x0203ac3c`) are
  read as well - the first is the sequencer's shape with four rows, the second
  is a one-shot setup rather than a loop.

  90_MAP's (`0x02063fd0`) is read as well, and like the settings screen's it is
  one-shot setup rather than a loop - it allocates the app's shared singletons
  on first use.

  All eleven are read (`recon.md` §16). The four editor screens' tails turn out
  to be two calls each - fetch the machine, call the constructor - with all the
  weight in the shared prologue; 52_FX's is traced in full as the example of a
  running state machine.

  What is not reimplemented from them is the redraw of readouts that follow a
  control rather than being one, beyond the mixer's meters and faders.

- **The rest of the behaviour.** Wired now: the map's boxes, the keyboard, the
  synth edit and patch-bay knobs, the KAOSS pad, the mixer, the FX page, both
  step grids, and the transport. Their hit boxes come from the decompiled
  tables, not from measurements.

  Not wired: each page's own top-bar buttons - CLR, the play button, the track
  selector. Those are not placed by the shared helper the other screens use, so
  their rectangles are inside the update's sub-states rather than in a table,
  and they have not been read. The browser page drives those functions from
  HTML controls instead, which is the main way it still does not respond like
  the original. `32_PATTERN` and `31_SONG` are drawn but not interactive; the
  arrangement is edited from HTML too.
- **Which octave the keyboard starts on.** The layout says it starts on an F;
  F3 is an assumption, not a reading.

## 7. Longer term

- **Fuzz the vector corpus.** Coverage is only what I sweep, and the inputs are
  chosen by my understanding of the code. Random patches, notes and timings would
  attack that bias. Offered several times, never done.
