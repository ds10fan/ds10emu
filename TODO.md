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

## 5. The drum buffers - two rounding residuals left

`src/core/drumbuf.c` renders a drum patch into its one-second buffer the way
`0x0206CCC8` does, and `songplay` uses it. recon.md §14 has the recipe, including
the frame that runs before the note - which is what makes noise and the delay come
out where the hardware has them.

`make test-drumbuf` holds it to buffers read out of a **running DS**, not to our
own oracle. Plain dry drums are exact. What is left:

1. **A noise oscillator mixed with another waveform.** `ds10plus_00`'s drum 1 is
   noise plus a triangle at balance 64. Its timing is right - correlation 1.000 at
   lag 0 - but 24% of the sampled window differs, by up to 23 counts, most of it in
   the decay.

   It is **not** the voice's arithmetic. Rendering all four of that song's drum
   patches through our `ds10_voice_render` and through the original's own code under
   Unicorn gives **0 differences in 32768 samples, for every one of the four** - so
   the voice is right and what differs is the state it starts from.

   And the shape of the residual says which state: it is front-loaded and decays
   away - 1100 mismatches in the first 2048 samples, then 226, 41, 6, 2, then none
   at all. That is filter memory. A note-on resets neither the filter nor the
   oscillator phases (`src/core/event.c`), so the pre-note frame leaves the filter
   holding a frame of noise, and our render starts it cold. Rendering a pre-frame
   through the whole voice and restoring only the phases does not reproduce it
   either, so the filter's input during that frame is not simply what our idle voice
   produces - the envelope's state during the pre-frame is the next thing to pin
   down, since it is what the filter's cutoff modulation reads.
2. **The wet mix.** A drum at dry/wet 127 has its echo on the right sample now, and
   3% of its samples differ by 4 counts or less. Likely the same class: where the
   two multiplies by 32764 land.

Both are bounded by the test (24 counts, 25% of samples), so a timing regression
cannot hide behind them - getting the pre-note frame or the delay wrong puts nearly
every sample out by thousands.

Still open beyond that: `ds10plus_00`'s drum 2 (pure noise through a band-pass
filter, 73%) and drum 3 (a resonant filter, attack 37, `vcasrc = eg`, 24% and our
render about 254 samples out). Both are state-sensitive filters, so the suspicion is
the filter's state at the note rather than anything about the recorder.

## 5c. The master gain is never set to 0x7FD7

recon.md §12 has the trace. The hardware's handler at `0x02072DAC` writes
`0x7FD7` straight into the mixer's master gain once its counter reaches 64; our
mixer only ramps, and its fixed point is `0x7F5F`. So every sample of a render
longer than that is 0.99633 of the hardware's, and a clipped sample lands on
`+32606 / -32607` instead of the hardware's `+32726 / -32727`.

Inaudible, but it is a real difference and no test can see it: the mixer vectors
are eight frames, and the oracle does not run the handler either. Whoever wires
the sequencer's message path next should push that message and let the gain be
set the way the machine does it, rather than special-casing a constant.

## 5d. SMT automation is rendered as HLD

The editor record's byte 6 (recon.md §14, `view` position 6) is the automation
object's `enable`: 1 is the `HLD` path, 2 is `SMT`, and 3158 of the 10464 records
here are `SMT`. `ds10_seq_run` runs a lane only when `enable == 1`, and
`songplay.c` installs 1 for every pattern rather than the song's own value, so an
`SMT` pattern's automation jumps between recorded values where the hardware ramps
into them. `0x0206E1EC`'s second path is now read - recon.md §13 has the recipe,
and it is one extra field per lane - so this is wiring, not reversing: carry byte 6
into `ds10_song_pattern`, install it as `enable`, and give `automation()` the
`delta` branch. Worth a vector against the original before it goes in, since
nothing in the suite covers a lane in `SMT` today.

## 5b. Unnamed bytes

Everything round-trips, so nothing is lost - these are just not understood:

- payload `+0x2EFC` (168 bytes, PLUS only) and `+0x2FA4` (56, only its last two
  bytes ever non-zero);
- the second 25-knob mixer space the ROM's own id table implies - volume
  `42 + channel`, pan `34 + channel` - which nothing here models.

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
  step grids, and the top bar every page carries. Their hit boxes come from
  the decompiled tables where there is one, and from the frames the page's own
  tile image draws where there is not - `tools/probe_frames.py` finds those,
  which is not measuring by eye.

  The bar is `ds10_ui_bar_hit`, one table per page, because the slot does not
  fix the role: x 162 is SOLO on the sequencer, CLR on 51_ALL_DRUMS and RECORD
  on the keyboard. Wired from it: the transport, record, solo, CLR, the
  sub-mode cycle, the screen swap, the pad's three assignments and its rate,
  the keyboard's octave and synth selector, and COPY and LOCK on 32_PATTERN.
  61_SYN_EDIT's and 70_PATCH's wide left button is a `synth` selector too, and
  is wired now.

  The two that no probe could read are now settled as far as they can be:

  - the triangle at x 193 is the **transport**. `tools/probe_buttons.py` diffs
    one later frame against a baseline, which a running sequencer can match
    exactly; `tools/probe_play.py` instead asks whether consecutive frames
    differ - whether the screen is *moving* - and it moves after the first tap
    and stops after the second.
  - the **S** changes nothing on screen at all. `tools/probe_ram.py` diffs main
    RAM across a tap, masking whatever churns on its own, and finds it
    toggling bit 1 of one global byte at `0x0208cf70` and nothing else, back
    and forth over four taps. What that byte means is **not proven**. It is
    taken as solo for the track the page is editing, because it is on exactly
    those pages that edit one track and absent from every page that carries a
    mute and solo strip of its own.

  Mute and solo themselves are the sequencer's runtime state, not the song's -
  the original's per-track `+0x22`, 0 to play the pattern and 1 to mute it - so
  they silence the pattern and not the channel, and nothing about them is
  saved.

  Not wired: the CLEAR prompt's own modal dialog, which the browser asks for
  instead. `31_SONG` draws but is not interactive, and nothing on the map opens
  it - its grid's rows and columns have not been pinned down, so the
  arrangement is still edited from the panel. The drum edit pages' wide left
  button and 61_SYN_EDIT's are unidentified.

  What the pages draw for themselves and this does not follow: the piano keys
  down the left of the roll do not move when the roll is scrolled, so their
  labels are only right at the default position.

  The probe cannot yet tell a control that clears the pattern from one that
  redraws it, because a tap on the grid toggles the cell, so re-entering the
  same notes before each probe alternates the baseline. That needs a run which
  enters notes once and never taps the grid again.
- **Where the piano roll's two octaves end.** The scrollbar is wired, and the
  one thing the capture pins is where its handle sits at rest: five twelfths
  down, so the default top row A4 is scroll position five and the range runs
  D5 to D#3. The ends are that inference, not a reading.
- **Which octave the keyboard starts on.** The layout says it starts on an F;
  F3 is an assumption, not a reading.

## 6b. Decompiling the UI, function by function

**See `UIPLAN.md`.** The interface is being restarted as a faithful
decompilation driven like a DS - pen events in, 2D state out - because the
behaviour layer described below was invented rather than read, and it does not
behave like the device. What follows is the findings so far, which the plan
builds on; the plan itself says what survives and what goes.

A different standard from the rest of section 6. The screens there were
reimplemented from decompiled coordinate tables and from tapping the hardware,
and checked by rendering a frame and diffing it against a capture - which
proves the captured states and nothing else. `src/app/widget.c` is for
functions read out of the ARM and run against the original under Unicorn, and
a function without a passing differential test does not go in it.

**The harness is proven.** `tools/emu.py seed` writes 4 MB of a melonDS RAM
dump over the static image, so a function that dereferences objects the linker
never created can be called at all; `tools/snapui.py` captures a page with its
main RAM. Demonstrated end to end: seed from a 61_SYN_EDIT capture, find the
game's own sixteen knob widgets in it, call `0x02055b98` on one at its real
address, and the object updates in place - `make test-uilive` does that for all
sixteen and checks our C reproduces every angle.

**Done:**

- `0x02055b98`, the knob's value-to-angle map. Two things it settled that no
  rendered frame would have: the value is not clamped when the angle is
  computed, and the divide truncates toward zero.
- the shared top bar, around `0x02053900`. Every button on it is registered by
  name - SOLO0/1/2 and AC0/AC1 and DECK all at x 131, PAGE at 42, PLAY, and
  menu/swap0/swap1 at 224 - which
  is why x 131 is in no screen's own constructor, and which confirms both
  hardware probes: the transport found by watching the screen move, and the
  solo inferred from which pages carry one.
- `0x020594d4`, 62_DRUM_SEQ's constructor: the four row pairs that 51_ALL_DRUMS
  shows down the left of its grid, and the scrollbar again - same origin, same
  0..84, same document field 0 as the synth sequencer's.
- `0x0202e01c`, 32_PATTERN's constructor. Its widget table gives the sixteen
  pattern buttons, COPY and LOCK, and the mute and solo strip exactly; the
  probed pitch was one out in both axes.
- `0x02022ba4` with `0x02011fc4` and `0x02011fd0`, the affine matrix every
  rot/scale sprite is drawn with, and the ROM's sine table at `0x0207A3D8`
  that feeds it. This one makes the case for the whole exercise: the `cos()`
  and `sin()` it replaces disagreed with the ROM's table on **16777 of the
  65536 angles**, by one unit of 8.8 - and every captured frame happened to
  land on an angle where they agreed, so `test-page` was and stayed green
  either way.

**Next, in this order:**

- the rest of the widget object. Its constructor `0x0202e750` is read and the
  field layout is in recon; what is left is the update and the OAM emit, the
  layer above `src/app/obj.c`: its constructor, its update, and the path
  from it to OAM. `src/app/obj.c` is the layer below and is already verified.
- the registry - `0x02029db4` insert, `0x0202b198` remove, the lookup and the
  per-class maps.
- 60_SYN_SEQ's scrollbar. `0x02050108` gives its origin (5,31), its 144-pixel
  span and its 0..84 range, and the getter chain is traced to the byte: it is
  **byte 0 of each track's 12-byte per-pattern record**, the block at `+0x25C`
  whose bytes 2 and 3 are already known to be the lane knob ids. So the scroll
  position is per pattern, per track, and saved - the reimplementation keeps it
  in one global rather than per pattern and per track. The 0..84 value itself
  is settled - it counts upward, seven units a row - and the handle geometry
  and the format field are the original's now (`ds10_editor_get_scroll`,
  round-tripped by test-editor). What blocks wiring it to the roll is a
  question the value does not answer: **where the 24-row range sits**. Twelve
  positions of seven units is 24 rows, but the ROM's own songs use notes 34 to
  80, which is 46 semitones - so either the range's base moves with something
  not yet found, or the roll only ever shows part of a song and each pattern's
  own scroll is what makes that workable. Until that is settled the roll keeps
  its wider note-based scroll, which can at least show every note in the ROM.
- the screen constructors. They are table walks, which is the tractable part,
  and success means `data/pages.bin` can be deleted rather than replayed.
  `0x0202e01c` and `0x02049150` are done - it is 32_PATTERN's, not the shared helper recon had
  it down as, and reading it corrected three sets of coordinates that had been
  tapped or measured. The other ten are in the table in section 6.
- the per-frame screen updates. Stateful and allocating; this is where I
  expect to stall, and stalling should be said rather than papered over with
  another screenshot comparison.

Still approximate in `ds10_ui_knob_value`: the pointer sprite's *position*.
Its centre orbits the knob's by (3,-7) turned through the angle, which was
read off thirteen pointers in a captured frame rather than decompiled, and it
is still done in floating point.

90_MAP's bottom bar has real widgets - `subSwing` at (76,176), `subTempo` at
(135,176), `subStep`/`subSave` at (200,176), with their captions at x 85, 147
and 219, all y 177. Checked against `ds10_ui_map_labels`, which draws at x 27,
117 and 176, y 177, and they are **not** the same thing: the ROM's are where
the labels go, and the captured positions this reimplementation uses are where
the *values* are drawn, by the screen's update rather than its constructor.
Only the y agrees, and it agrees exactly. So there is nothing to correct here
until the update is read - which is the third obvious-looking correction in
this session that turned out to be wrong.

Two holes that are engine rather than UI and are on the bit-exactness path:
the document-to-runtime-pattern conversion (`build_rt` is mine, not
decompiled), and where a pen edit reaches the document.

## 7. Longer term

- **Fuzz the vector corpus.** Coverage is only what I sweep, and the inputs are
  chosen by my understanding of the code. Random patches, notes and timings would
  attack that bias. Offered several times, never done.
