# Plan: reimplement the KORG DS-10 PLUS synthesizer engine in C

## 1. Goal

A C reimplementation of the KORG DS-10 PLUS **sound engine**, with correctness
established by differential testing against the original ARM code rather than by
listening.

- **Scope:** the synth engine, verified bit-exact against the original.
- **Out of scope:** the interface, multiplayer (DS Wireless Play / WiFi), DSi-exclusive
  features. Roughly 171 of the ARM7's hardware-register touches are WiFi; all of
  that is ignored.
- **Non-negotiable:** verification. Every phase below has a machine-checkable
  exit criterion. No phase is "done" because the output sounds right.

Ground rules: this is a clean reimplementation. No ROM bytes, extracted code, or
derived assets go in the repo — `out/` and `assets/` are gitignored. The user
supplies their own ROM; `tools/` regenerates everything from it, and test vectors
are committed as hashes plus a generator, never as data.

The established facts this plan builds on are in [`recon.md`](recon.md). Read
that first; in particular §0 (prior art), §2 (the memory map, which is the
difference between a usable disassembly and a 99%-wrong one) and §10 (the address
and structure reference).

### Which road: translation or reimplementation

Prior art matters here. thement's [`ds10-vst`](https://github.com/thement/ds10-vst)
already produces correct DS-10 audio — by **mechanically translating the original
ARM code to C** and embedding the ROM image in the binary. Two roads exist and
they are not the same project:

| | Road A — translate | Road B — reimplement |
|---|---|---|
| method | ARM→C transpile, `R5 = R2 + R1; goto lab...` | understand each stage, write idiomatic C |
| correctness | by construction | by differential testing |
| time to first sound | short | long |
| readable / extensible | no | yes |
| polyphony, more voices, new features | needs N emulator contexts | natural |
| carries original code | yes (image embedded) | no |

**This plan takes Road B.** It is what "a C application" and "verification is a
must" imply, and Road A is already done by someone else — redoing it adds
nothing. Road B was previously the expensive option; it is now much cheaper,
because thement published the struct layouts, knob table, event encoding and
function addresses (recon §10), so the *discovery* cost is largely paid and what
remains is understanding the arithmetic in each stage.

Road A stays useful as a **fallback and a cross-check**: `ds10-vst` is an
independent implementation to compare against when our output and the oracle
disagree in a way we cannot explain.

## 2. Target

**DS mode, not DSi mode.** Unit code `0x02` means the DS-mode build is complete
and self-contained, and the DSi sections in this dump are encrypted or
mis-dumped (recon §6). Revisit only if a PLUS feature proves DSi-exclusive.

## 3. Verification strategy

This is the spine of the project, so it comes before the phases. Three tiers, in
increasing scope and decreasing strictness.

### Tier 0 — format round-trip (exact)

Parse, re-serialize, compare bytes. Applies to: the ROM container, the 24 factory
patches and the 13 songs.

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

## 4. Deliverable architecture

```
src/core/          pure C99, no libc math, NO FLOATS
  fixed.h            Q-format helpers mirroring the original's arithmetic
  tables.c           generated from the ROM at build time (gitignored)
  vco.c vcf.c eg.c lfo.c voice.c drum.c
  fx.c               delay / chorus / flanger  (DRY/WET, L/R RATIO confirmed)
  mixer.c
  patch.c song.c seq.c
  synth.c            top level: ds10_synth_render()
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

Each phase states its exit criterion. Phase 2 can run in parallel with 3.

### P0 — Foundation (largely done)

Done: ROM extraction, BLZ decode, autoload memory map, per-CPU Unicorn images
(both CPUs boot into their autoloaded code), branch-target validation.

Remaining:

- Import the six blobs into Ghidra as **separate memory blocks** at the recon §2
  addresses, `ARM:LE:32:v5t` for ARM9 and `v4t` for ARM7, and **run analysis**.
- Repo skeleton, `Makefile`, `make recon`.

Exit: `make recon` reproduces every recon artifact from the ROM; Ghidra database
with correctly-based blocks, analyzed.

### P1 — Stand up the oracle — **DONE**

`tools/oracle.py` renders real DS-10 audio by executing the original ARM9 code.
Exit criteria met: deterministic across runs, pitch accurate to measurement
resolution, `0x02075410` writes exactly 64 contiguous halfwords per frame. See
recon.md §11 for the object graph, the allocator problem, and the measured VCO
waveform enum.

```sh
python3 tools/oracle.py probe            # construct, render, trace writes
python3 tools/oracle.py wav out/a.wav 0 60 64
python3 tools/oracle.py waves            # characterise vco1 = 0..3
```

The `Oracle` class is the tier-1 API: `load_patch`, `note_on`, `note_off`,
`render(frames)`.

Follow-up left open (not blocking): reproduce the real machine wiring at
`0x02032398` instead of setting `+0x04/+0x10/+0x1C` by hand. Verified that the
hand-set fields taken from thement's dump do not affect output — zeroing them
yields identical samples — so they are outputs, not inputs, at this level.

<details>
<summary>Original P1 plan, kept for reference</summary>

The original blocking question — ARM9 or ARM7 — **is answered: ARM9**, at 32768 Hz
in 64-sample frames, six voices (recon §5). So P1 is no longer discovery; it is
getting the harness to actually drive the published entry points.

1. Import the six blobs into Ghidra at the recon §2 addresses; import thement's
   symbol map from recon §10 as labels so the database is readable immediately.
2. Extend `tools/emu.py` with what the engine needs and the current stub lacks:
   - **`DIVCNT` hardware divider** at `0x04000280` and the square-root
     accelerator. The engine uses both; thement hit this twice and got wrong
     output until it was implemented. This is the single most likely reason a
     first attempt produces garbage.
   - IPC FIFO stubs for `0x00004007` (render frame) and `0x00004047` (sequencer
     tick).
3. Reproduce a note: write the event pair into a synth's event area at `+0x80`
   and bump the count at `+0x08`, then call the render handler and read the
   64-sample buffer out of ITCM.

Exit: `tools/emu.py` renders a frame for a named factory patch and a given note,
deterministically, and the output is non-silent and stable across runs. **That
artifact is the oracle every later phase depends on.**

</details>

### P2 — Data formats

The patch parameter block is **already done** — bytes `0x20..0x3E` are knobs 0..30,
verified against all 24 factory patches (recon §4). Remaining:

- Patch bytes `0x00..0x0F`: determine whether it is a checksum. Needed only to
  write patches the original will accept, not for playback — so this is
  low-priority, not blocking.
- Song, 12044 bytes: diff `ds10init.dat` against the 13 demo songs to expose
  pattern/step/track layout. Not covered by prior art.
- The `ds10backup_080425.bin` save image. thement located patches at about
  `0x38000` with the same `+0x20` parameter offset; the rest is unmapped, and
  their own open question was "what is stored in patch saves other than the synth
  configuration?"

Exit: Tier 0 green on all 24 patches and all 13 songs.

### P3 — Audio engine — **DONE**

Every audio component is reimplemented in float-free C and verified bit-exact
against the original ARM9 code. All under `make test`, which regenerates its
vectors from the user's ROM first:

| component | source | vectors |
|---|---|---|
| whole synth voice, end to end | `voice.c` | 33280 samples / 520 frames |
| patch loading, 31 knobs | `patch.c` | 2720 patches x 45 fields |
| event handling, 9 opcodes | `event.c` | 78 dispatch calls x 19 fields |
| effect parameter mappings | `fx.c` | 489 sets x 17 fields |
| mixer + effect DSP | `mixer.c` `fx.c` | 41472 samples / 35 cases |
| drum voice | `drum.c` | 8960 samples / 21 cases |
| VCF per-sample | `vcf.c` | 19968 samples / 312 frames |
| VCF cutoff mapping | `vcf.c` | 468 frame transitions |
| VCO x2 + xmod + sync + mix | `vco.c` | 15616 samples / 244 frames |
| ENV | `env.c` | 2976 frame transitions |
| LFO | `lfo.c` | 1136 frame transitions |
| NOTECTRL + PITCHCTRL | `pitch.c` | 1180 frame transitions |
| AMP + drive | `amp.c` | 15104 samples / 236 frames |

Tempo sync is covered in all three places it appears: the effect's
tempo-synced delay time through the hardware divider, the synth LFO's
BPM-synced rate across five tempos, and the bar-start event that re-phases a
BPM-synced LFO but leaves a free-running one alone.

Every component landed bit-exact on its first or second attempt. The failures
were: two harness sampling-order bugs, one selector array too small, one mapping
applied to all three effect types instead of one, and one case of dividing by the
sync flag instead of the tempo. None was a misread of the arithmetic.

What is deliberately NOT covered: the sequencer that generates events. It is
outside "audio code".

Recon §4/§10 reshape this phase considerably:

- The topology is no longer hypothetical. Knob names give the actual module set:
  2 VCOs with 4 waveforms and sync, VCF with **three types** (LP/BP/HP, not just
  low-pass), ADSR, LFO with optional BPM sync, portamento, a **drive/distortion
  stage** in the AMP, and a 5-input modulation matrix with 7 sources.
- The drum path is **resampled playback**, not a live voice: a drum patch is
  rendered once into a `0x8000`-sample buffer and then pitched by play-speed with
  linear interpolation (`0x02072354`). So `drum.c` is a resampler plus the shared
  voice renderer, not a second DSP path.
- **Envelopes update once per frame, not per sample.** Getting this wrong is a
  silent, plausible-sounding bug that Tier 1 would catch but that is much cheaper
  to just implement correctly.
- Known arithmetic traps, from thement's own translator bugs: ASR sign extension,
  `SMULL` sign extension on the second operand, and post- vs pre-increment on
  `STRH Rn, [Rm], #2`.

The genuinely unreversed parts — and therefore the real risk in this phase — are
the **effects block and the mixer** (recon §9). Prior art has their struct offsets
but not their algorithms, and the pan law contains an unexplained `0x4444` scaling
factor. Budget accordingly: the synth voice is well-mapped, the FX are not.

Exit: Tier 1 green — zero mismatches across the full corpus.

### P4 — Sequencer and song playback

Step sequencing, tempo, swing, pattern chaining, song mode, and the per-step
lanes the ROM confirms exist (gate, pan, volume, piano-roll, KAOSS — the six
`60_SYN_SEQ_*` layouts).

Exit: Tier 2 null test passes on every demo song.

### P6 — Integration

Save/load in the original's format, WAV export, CLI (headless render for Tier 2).

Exit: load a demo song, play it, edit a patch, save, reload, and get the same
audio back.

## 6. Risks

| risk | mitigation |
|---|---|
| ~~Effects and mixer are unreversed even in prior art~~ | **Structure now decoded** - recon.md 12. The effect is one delay DSP with three parameter-mapping strategies behind a vtable, which is why reading the setters linearly failed. 14 small mapping functions remain |
| Engine not cleanly decomposable into per-stage functions | Block-level oracle works regardless; bisect by parameter sweep |
| Harness produces garbage because the hardware divider is missing | Implement `DIVCNT` and the sqrt accelerator in P1, before trusting any output |
| Original is C++ with virtual dispatch, hard to follow statically | Lean on the oracle: probe behaviour empirically instead of reading every branch |
| The 16-byte patch header is a hash we cannot reproduce | Only blocks *writing* patches the original accepts; playback and Tier 1 unaffected |
| Bit-exactness lost in the DS hardware output stage | Tier 1 compares pre-hardware buffers; model the output stage separately as a toggle |

## 7. Rough sizing

P1 days · P2 days · **P3 weeks — the bulk, and now concentrated in the FX/mixer
rather than the voice** · P4 about a week · P6 about a week.

P1 is still the gate, but it is now a plumbing task rather than a research one:
the render entry points are published, so the work is implementing the hardware
divider and driving the event queue. Once `emu.py` renders one frame
deterministically, P3 becomes a grind against a fast exact oracle instead of an
open-ended research problem — which is the whole reason for front-loading the
harness.

The one part where prior art does not help is the effects and mixer. Expect that
to dominate P3's tail.
