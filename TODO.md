# TODO

## 1. What the whole-song differential does not cover yet

`make test-songwalk` walks an arrangement and compares the ROM's own mix bus with
`out/songplay`, sample for sample: `new_00` (eight patterns, no repeats, a cutoff
lane in every one) and `fresh10_02` (a per-bar mute lane over both synths and three
drums) are bit-identical, 381248 and 393216 samples. That closes what this section
used to be: four bugs found by ear that no test could fail, because the harness
installed one pattern and looped it.

What it still does not reach:

- **The transport object.** `songwalk` walks the chain and reads the mute masks from
  Python; the original does both from its transport, via `0x020330AC`, so the mask
  reading and the bar counter are ours rather than its. Modelling that object needs a
  document object too.
- **Every routed event consumed.** Assert that nothing is posted which no machine
  handles. Three of the four bugs found by ear would have shown up that way.
- **`tests/test_songrender.c` rebuilds the engine inside the test**, so a bug in how
  `songplay` wires things up cannot fail it. `songwalk` supersedes it for wiring, and
  the duplication is a trap worth removing.
- **Harness constants standing in for song data.** Three of the five bugs found while
  building `songwalk` were in the *reference*: drum buffers from an inference at half
  amplitude, a tempo written by hand that no voice ever heard, and the mixer-knob push
  placed after the tick instead of inside the install. `_rt_pattern` and the capture
  setups deserve the same audit - each constant standing in for song data is a path
  nothing can see.

## 2. Verify against real hardware

`tools/songhw.py` plays a song on the ROM under melonDS and captures the SPU, which
is enough to listen to and settled that the per-bar mute really is applied there. It
cannot be sample-exact - melonDS resamples through blip_buf and the ROM's playback
channel runs at a different rate - so it is an ear, not a test.

For the things an ear cannot judge, `tools/mksong.py` writes valid song files, so a
targeted probe is easy and much easier to judge than a busy track:

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
- **The transport.** The chain walk is mine. The original's bar advance
  (`0x0206D1E4`, reached through the `"stpt"` message) and its per-bar mute update
  (`0x020330AC`) are read, but the transport object that drives them - and the
  document object it reads through - are not modelled, so `songwalk` supplies both
  from Python.

## 4. Not implemented

- **Automation modes other than playback.** The processor branches on track state
  `+0x20` (1 = HLD, 2 = SMT) and `+0x21` (0..3). Both `+0x20` paths are done with
  `+0x21` of 0, which is what a recorded song plays back; `+0x21` of 1..3 consume a
  pending value from live knob input, which is editing rather than playback, and no
  ROM demo asks for one.
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

## 5b. Unnamed bytes

Everything round-trips, so nothing is lost - these are just not understood:

- bytes 8, 10 and 11 of the per-track editor record at pattern `+0x25C`: no
  accessor call site touches them and they are zero in all 10464 records here;
- the second 25-knob mixer space the ROM's own id table implies - volume
  `42 + channel`, pan `34 + channel` - which nothing here models.

## 5e. `trs`, the per-bar transpose, is stored and not played

The song page's mute lane is played now - `ds10_song_mute` reads it, `songplay`
applies it the way `0x020330AC` and `0x02079D58` do, and `make test-songwalk` holds
that to the original sample for sample on `fresh10_02`. (The claim that used to be
here, that nothing reads the masks, was wrong: the read is in the transport, not in
the song player whose copy at `+0x8C` really is a dead store.)

What is still stored and not played is `trs`, the signed per-bar transpose at
payload `+0x2FA8`. Nothing here reads it, and the original's own path for it is not
traced - `0x0203C6FC` draws it and `0x0203C038` edits it, but what applies it to a
playing note has not been found. Worth doing with a song that transposes, and none
in `data/saves` does: their only non-zero `trs` bytes are the two the record buffer
leaves behind, which the original does not load either (docs/songtext.md).

Deck B's mask is also unplayed, and cannot be otherwise: its knobs are 42..47, a
second 25-knob mixer space this build has no mixer for (recon.md §12).

## 7. Longer term

- **Fuzz the vector corpus.** Coverage is only what I sweep, and the inputs are
  chosen by my understanding of the code. Random patches, notes and timings would
  attack that bias. Offered several times, never done.
