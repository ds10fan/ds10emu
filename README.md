# ds10

Reverse engineering KORG DS-10 PLUS (Nintendo DS) into a C reimplementation
of its synthesizer engine.

The interface and multiplayer (DS Wireless Play / WiFi) are out of scope.

## Status

**Audio.** Reimplemented and verified sample-exact against the original ARM9
code running under Unicorn. `out/songplay` renders a `.dat` song to a WAV using
nothing but the reimplementation: the oscillator, filter, envelope, LFO,
portamento and amp; the 31-knob patch format; the drum voice and the one-second
buffer a drum patch is rendered into; the mixer's 25 knobs and the effect's
eight; the sequencer's transport, gating, note emission and event routing; the
song file format; both automation paths the KAOSS pad offers, `HLD` and `SMT`;
and the song page's per-bar track mute.

Verified over **whole arrangements**, not just one looping pattern: `make
test-songwalk` walks a song's chain in the original's own code and compares its
mix bus with `out/songplay` sample for sample. `new_00` (eight patterns, no
repeats, a cutoff lane in every one) and `fresh10_02` (a mute lane over both
synths and three drums) are bit-identical, 381248 and 393216 samples.

**Formats.** The three the game uses have names and suffixes here: `.ds10song`,
`.ds10patch` and `.ds10save`. `tools/sav.py` is the one tool for a save — list,
extract, pack a slot, or unpack the whole 512 KiB to text and back. `songtext` is
that text: a song as `note`/`gate`/`vel`/`pan` rows, named patch and mixer knobs,
automation lanes on the pad's two axes, the song page's `mute` and `trs` lanes, and
a `[raw]` block for whatever is still unnamed, so a dump reloads byte for byte —
109 of 109 songs and all six saves do. [`docs/songtext.md`](docs/songtext.md) is the
reference.

- [`recon.md`](recon.md) — what the ROM actually contains: container layout, the
  ARM9/ARM7 memory map, NitroFS inventory, the song and patch formats, the
  engine's addresses and structures.
- [`PLAN.md`](PLAN.md) — phased plan, and the tiered verification strategy the
  project is built around.
- [`TODO.md`](TODO.md) — what is left, starting with the gap in the audio test
  suite that four separate bugs have already slipped through.

### Verification

Every claim above is a test, not an impression.

| target | what it compares | scale |
|---|---|---|
| `make test` | the audio engine against the original under Unicorn | 20 suites |
| `make test-drumbuf` | a drum buffer against one a running DS rendered | 4 drums, 12288 samples |
| `make test-songwalk` | a whole arrangement, the ROM's own mix bus against `songplay` | 6 channels, 381248 samples |
| `make test-songtext` | every song through the text format and back | 109 songs |

`make test` is strong on DSP and was blind on the wiring around it, which is
where every bug found by ear had been; `test-songwalk` is what closed that, and
found five more on the way — three of them in the test harness rather than in the
engine. `TODO.md` §1 has what it still does not reach.

### Prior art

There is significant earlier work by **thement** on the byte-identical ROM —
a [project log](http://ibawizard.net/~thement/ds10rev.html) (plain HTTP only) and
[`ds10-vst`](https://github.com/thement/ds10-vst), a working VST. That VST is a
mechanical ARM-to-C translation with the ROM image embedded, not a
reimplementation; this project takes the reimplementation road instead, and uses
theirs as a cross-check. See `recon.md` §0 and `PLAN.md` §1.

## Requirements

`data/` holds the ROM-derived inputs the engine and its tests need, so
neither requires a ROM. Regenerating the test vectors does; supply your own
dump:

```
ds10plus.nds     sha1 85064992fe671f6e34ffeb01570c9cce56a6a245
```

Tooling: `python3` with `unicorn` and `pillow`, `arm-none-eabi-objdump`, and
Ghidra for the disassembly work. `make test-songhw` and the drum buffer capture
need a melonDS core built without the Qt frontend; `make ndsref` prints the exact
commands if it is missing.

## Usage

```sh
make test                   # the audio engine, against the original
make songplay               # out/songplay song.dat -o out.wav
make test-songwalk          # a whole arrangement, the ROM's own render against ours
python3 tools/songtext.py   # dump|load|check, the text format
python3 tools/songhw.py     # play a song on the ROM in melonDS and capture the audio

make recon                  # extract everything from the ROM into out/
```

`DS10_ROM` overrides the ROM path (default `ds10plus.nds`).

## Tools

| file | what |
|------|------|
| `tools/ndsx.py` | NDS header, NitroFS walk, BLZ decode, extraction, autoload memory map, patch dump, register classification |
| `tools/emu.py` | per-CPU Unicorn image and function-call harness — the oracle the audio verification depends on |
| `tools/oracle.py` | renders real DS-10 audio by executing the original ARM9 code, and writes the test vectors |
| `tools/ndsref/` | headless melonDS: per-frame machine snapshots, with `-m` main RAM and with `-a` the SPU's output |
| `tools/snap.py` | reads those dumps |
| `tools/sav.py` | a `.ds10save`: list, extract or pack a song slot, unpack the whole thing to text and back |
| `tools/songtext.py` | a song as editable text and back, byte for byte — [`docs/songtext.md`](docs/songtext.md) |
| `tools/songwalk.py` | a whole arrangement rendered twice, by the ROM's own code and by `out/songplay`, compared sample for sample |
| `tools/songhw.py` | plays a song on the ROM under melonDS and captures it, for listening rather than diffing |
| `tools/probe.py` | branch-target validation, lookup-table detection, xrefs, multiply density |

## Layout

```
src/core/  the audio engine
src/app/   songplay, the song-to-WAV renderer
data/      ROM-derived inputs, committed
out/       everything extracted or built (gitignored)
```
