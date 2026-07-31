# Plan: reimplement KORG DS-10 PLUS as a C / SDL 1.2 application

## 1. Goal

A native C application that reproduces the KORG DS-10 PLUS synthesizer —
primarily its **sound engine**, secondarily its **UI** — with correctness
established by differential testing against the original ARM code rather than by
listening.

- **Primary:** the synth engine, verified bit-exact against the original.
- **Secondary:** the GUI, in SDL 1.2, faithful to the original layout.
- **Out of scope:** multiplayer (DS Wireless Play / WiFi), DSi-exclusive
  features. Roughly 171 of the ARM7's hardware-register touches are WiFi; all of
  that is ignored.
- **Non-negotiable:** verification. Every phase below has a machine-checkable
  exit criterion. No phase is "done" because the output sounds right.

Ground rules: this is a clean reimplementation. No ROM bytes, extracted code, or
derived assets go in the repo — `out/` and `assets/` are gitignored. The user
supplies their own ROM; `tools/` regenerates everything from it, and test vectors
are committed as hashes plus a generator, never as data.

The established facts this plan builds on are in [`recon.md`](recon.md). Read
that first; in particular the memory map in §2, which is the difference between a
usable disassembly and a 99%-wrong one.

## 2. Target

**DS mode, not DSi mode.** Unit code `0x02` means the DS-mode build is complete
and self-contained, and the DSi sections in this dump are encrypted or
mis-dumped (recon §6). Revisit only if a PLUS feature proves DSi-exclusive.

## 3. Verification strategy

This is the spine of the project, so it comes before the phases. Four tiers, in
increasing scope and decreasing strictness.

### Tier 0 — format round-trip (exact)

Parse, re-serialize, compare bytes. Applies to: the ROM container, the 24 factory
patches, the 13 songs, the UI string tables, and every graphics container
(NCGR/NCLR/NSCR/NCER/NANR/NCBR/NFTR).

Pass: byte-identical, 100% of files. A round-trip failure means the format is
misunderstood — no interpretation required.

### Tier 1 — differential DSP testing (exact, and the core of the project)

Run the **original ARM render code** under Unicorn as an oracle. Feed identical
inputs to the oracle and to the C implementation. Compare output buffers
sample-by-sample.

This is achievable *bit-exactly* because neither the ARM946E-S nor the ARM7TDMI
has an FPU — the whole engine is integer fixed point. There is no tolerance
window to argue about: outputs either match or the C code has a bug.

The key move is to **avoid needing to understand the engine's state layout**.
Rather than hand-constructing state in emulated RAM, call the original's own
init and note-on routines first, then call its render routine:

```
oracle:  orig_init(state)        C:  ds10_synth_init(&s)
         orig_load_patch(state, patch_bytes)
                                     ds10_synth_load_patch(&s, patch_bytes)
         orig_note_on(state, note)    ds10_synth_note_on(&s, note)
         orig_render(state, buf, n)   ds10_synth_render(&s, buf, n)
         -> compare buf, bit for bit
```

The state struct can stay opaque for a long time. Once mapped, add field-level
comparison to localise divergence faster.

Corpus, generated not hand-written:

- 24 factory patches x 128 notes x {short, long, released} gate.
- Per-parameter sweeps: for each of the ~31 parameter bytes, sweep its full range
  with the rest held at INIT.
- Random patches, fuzzed. The oracle is cheap, so this scales to millions of
  vectors.
- Deliberate edge cases: maximum resonance (self-oscillation), cutoff at 0,
  retrigger mid-envelope, portamento between extremes, all-voices-on.

Pass: **zero** mismatching samples. `make check` runs a fixed-seed subset for
every commit; a longer randomized run is a separate target.

### Tier 2 — whole-song null test (near-exact)

Render a demo song offline with the C app; capture the same song from a real DS
emulator driven by a recorded input movie for determinism. Align, invert, sum,
and measure the residual.

Pass: sample-exact once Tier 1 is green and sequencer timing matches. If not
exact, residual peak below -90 dBFS **with a written explanation** of the
remaining difference — an unexplained residual is a failure, not a rounding
detail.

Note the DS output stage (10-bit DAC, `SOUNDBIAS`, hardware mixing) sits between
the engine and the speaker. Model it as a toggleable stage. Tier 1 compares the
engine's buffer *before* this stage, so it is unaffected by that decision.

### Tier 3 — GUI comparison (partial)

Render each screen in the C app; compare against emulator screenshots at
equivalent state.

Pass: tile/BG layers pixel-exact. Screens composited with the DS 3D engine
(recon §5 — parts of the UI are textured 3D quads) will not match pixel-for-pixel
in SDL 1.2; those are compared visually and the deviation is documented. Being
explicit about this beats a vague "looks close".

## 4. Deliverable architecture

```
src/core/          pure C99, no SDL, no libc math, NO FLOATS
  fixed.h            Q-format helpers mirroring the original's arithmetic
  tables.c           generated from the ROM at build time (gitignored)
  vco.c vcf.c eg.c lfo.c voice.c drum.c
  fx.c               delay / chorus / flanger  (DRY/WET, L/R RATIO confirmed)
  mixer.c
  patch.c song.c seq.c
  synth.c            top level: ds10_synth_render()
src/platform/
  audio_sdl.c        SDL_OpenAudio + ring buffer
  video_sdl.c        two 256x192 surfaces, integer scaler
  input_sdl.c        mouse -> stylus, keyboard -> buttons
  assets.c           NitroFS reader + Nintendo container decoders
  main.c
src/gui/
  widget.c knob.c slider.c stepgrid.c keyboard.c kaoss.c
  screen_*.c         one per original screen
tests/
  test_synth.c       Tier 1 runner
  test_assets.c      Tier 0 round-trip
  nulltest.py        Tier 2
tools/               extraction, oracle, table codegen  (already present)
```

The hard constraint is that `src/core/` is float-free and dependency-free. That
is what makes Tier 1 possible; it also means the engine is trivially callable
from another language later if a different front end is ever wanted.

## 5. Phases

Each phase states its exit criterion. Phases 2 and 5 can run in parallel with 3.

### P0 — Foundation (largely done)

Done: ROM extraction, BLZ decode, autoload memory map, per-CPU Unicorn images
(both CPUs boot into their autoloaded code), branch-target validation.

Remaining:

- Import the six blobs into Ghidra as **separate memory blocks** at the recon §2
  addresses, `ARM:LE:32:v5t` for ARM9 and `v4t` for ARM7, and **run analysis**.
- Repo skeleton, `Makefile`, `make recon`.

Exit: `make recon` reproduces every recon artifact from the ROM; Ghidra database
with correctly-based blocks, analyzed.

### P1 — Locate the audio path (blocking; do this first)

The open question from recon §5: **is the render loop on the ARM9 or the ARM7?**
The table cluster and multiply density point at ARM9, but the sound hardware is
ARM7-only, so a buffer handoff exists either way. Nothing else can start until
this is settled.

Four independent approaches, cheapest first:

1. Ghidra: xref the function at `0x02075454`/`0x02075458` that references both
   resolutions of the `0x02080400` curve table.
2. Unicorn: hook all memory writes and find the routine that writes ≥256
   sequential 16-bit values — that is the render target buffer, whichever CPU
   it is on.
3. ARM7: trace the IPC FIFO handler and the `SOUNDCNT`/`SNDCAP` setup back to
   whoever supplies its samples.
4. Xref the remaining curve tables and see which are read in tight loops
   (waveform / phase-accumulator indexed) versus once per note (parameter
   curves). This also answers recon open question 2.

Exit: a named render entry point plus a Unicorn harness that calls it with a
factory patch and a note, and produces plausible non-silent audio. Also
recorded: sample rate, block size, buffer format, voice count.

### P2 — Data formats

- Patch, 63 bytes: complete the field map from recon §4. Resolve whether
  `0x00..0x0F` is a checksum, and if so which algorithm — needed only to write
  patches the original will accept, not for playback.
- Song, 12044 bytes: diff `ds10init.dat` against the 13 demo songs to expose
  pattern/step/track layout.
- UI strings, and the `ds10backup_080425.bin` save image layout.

Exit: Tier 0 green on all 24 patches and all 13 songs.

### P3 — Synth engine (the bulk of the work)

Reverse and reimplement one stage at a time, differential-testing each stage as
it lands rather than at the end. Order follows the signal path:

oscillators → envelope → filter → amp/pan → voice → drums → FX → mixer.

Where the original decomposes into per-stage functions, give each its own oracle
entry point. Where it does not, test at block level and bisect mismatches by
sweeping one parameter at a time.

Expected topology, based on the MS-10 lineage and the confirmed FX parameters —
treat as **hypothesis to confirm, not as a specification**: 2 VCOs, 24 dB
low-pass VCF with cutoff and peak, ADSR envelope, LFO, portamento, 4 drum parts,
and a delay/chorus/flanger send.

Recon §4 removes work here: the factory drum patches (`KICK1`, `SNARE1`, `HIHAT`,
…) use the **same 63-byte patch format** as the tonal ones, so the drum parts are
the same voice with different parameters. There is one engine to reverse, not
two, and `drum.c` is parameter plumbing rather than a second DSP path.

Exit: Tier 1 green — zero mismatches across the full corpus.

### P4 — Sequencer and song playback

Step sequencing, tempo, swing, pattern chaining, song mode, and the per-step
lanes the ROM confirms exist (gate, pan, volume, piano-roll, KAOSS — the six
`60_SYN_SEQ_*` layouts).

Exit: Tier 2 null test passes on every demo song.

### P5 — GUI (SDL 1.2)

Asset pipeline first (NitroFS + container decoders, already needed by Tier 0),
then screens in dependency order:

`61_SYN_EDIT` → `60_SYN_SEQ_*` → `32_PATTERN` → `63_DRUM_EDIT_PATCH` →
`30_MIXER` → `52_FX` → `71_KAOSS` → `31_SONG` → `81_KEY` → `01_MEIN_MENU` →
`15_OPTION`.

The full 65-layout inventory is in recon §3, so this scope is known up front
rather than discovered.

Note the installed SDL 1.2.68 is **sdl12-compat over SDL2**, not original SDL
1.2. The 1.2 API is what we code against; it just happens to get modern
backends, which is an advantage.

Exit: all functional screens navigable and driving the engine; Tier 3 pixel-diff
green on tile layers, 3D-composited screens documented.

### P6 — Integration

Save/load in the original's format, WAV export, CLI (headless render for Tier 2).

Exit: load a demo song, play it, edit a patch, save, reload, and get the same
audio back.

## 6. Risks

| risk | mitigation |
|---|---|
| Engine not cleanly decomposable into per-stage functions | Block-level oracle works regardless; bisect by parameter sweep |
| DSP split across both CPUs with tight IPC coupling | `emu.py` already models CPUs separately; add a FIFO bridge between two instances |
| Original is C++ with virtual dispatch, hard to follow statically | Lean on the oracle: probe behaviour empirically instead of reading every branch |
| The 16-byte patch header is a hash we cannot reproduce | Only blocks *writing* patches the original accepts; playback and Tier 1 unaffected |
| 3D-composited UI cannot be pixel-matched in SDL 1.2 | Accept and document; do not pretend Tier 3 is exact everywhere |
| Bit-exactness lost in the DS hardware output stage | Tier 1 compares pre-hardware buffers; model the output stage separately as a toggle |

## 7. Rough sizing

P1 days · P2 days · **P3 weeks — the bulk** · P4 about a week · P5 two to three
weeks · P6 about a week.

P1 is the gate. If it lands cleanly, P3 becomes a grind against a fast, exact
oracle rather than an open-ended research problem, and that is the whole point of
front-loading the emulation harness.
