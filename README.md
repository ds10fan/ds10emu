# ds10

Reverse engineering KORG DS-10 PLUS (Nintendo DS) into a C reimplementation:
the synthesizer engine, and the interface, running in a browser.

Multiplayer (DS Wireless Play / WiFi) is out of scope.

**The interface is being rewritten.** See `UIPLAN.md`: the drawing is a
verified model of the DS 2D hardware, but the behaviour was invented rather
than decompiled, and is being replaced by a function-by-function decompilation
driven like a DS - pen events in, 2D state out.

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

**Graphics.** The DS 2D hardware is modelled in C and is pixel-exact against
melonDS over 29 screens. The interface's pages are drawn from the game's own
asset files; nine of them reproduce the real thing exactly.

**Browser.** `make serve` builds the whole thing to WebAssembly and serves it,
and it is an editor rather than a viewer. A new song starts from the ROM's own
blank template. Notes go in on the piano roll and the drum grid, appear where
the hardware puts them, sound as they are entered, and play back through the
decompiled engine with the cursor walking the grid. Patches, the mixer, the
effect and the KAOSS pad respond, the map navigates, and **save** writes a
`.dat` the hardware would load.

The sequencer's own sub-mode button steps NOTE, GATE, VOLUME, PAN and the two
automation lanes, and each of those pages is drawn from the pattern and
editable; the pattern page selects patterns; the map's per-synth boxes pick
the track.

What is *not* done: CLR and the transport buttons on each page's top bar are
not wired, so the browser page drives those from HTML controls instead, and
`31_SONG` draws but is not interactive. (On the hardware it *is* reachable, which
this project had recorded the wrong way round: `L` puts the map on the touch
screen and its `SONG` box opens the page.) Captions still come from the captured
frame rather than being rasterised. See [`TODO.md`](TODO.md) §6.

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
  engine's addresses and structures, and (§16) the graphics path.
- [`PLAN.md`](PLAN.md) — phased plan, and the four-tier verification strategy the
  project is built around.
- [`TODO.md`](TODO.md) — what is left, starting with the gap in the audio test
  suite that four separate bugs have already slipped through.

### Verification

Every claim above is a test, not an impression.

| target | what it compares | scale |
|---|---|---|
| `make test` | the audio engine against the original under Unicorn | 25 suites |
| `make test-obj` | the sprite widget's OAM against the game's own setter | 171 checks |
| `make test-2d` | the 2D hardware against melonDS, pixel for pixel | 72 frames |
| `make test-page` | each page rebuilt from assets, against melonDS | 22 screens |
| `make test-editor` | an edited song round-tripped through the file format | hash and all |
| `make test-drumbuf` | a drum buffer against one a running DS rendered | 4 drums, 12288 samples |
| `make test-songwalk` | a whole arrangement, the ROM's own mix bus against `songplay` | 6 channels, 381248 samples |
| `make test-songtext` | every song through the text format and back | 109 songs |
| `make test-webedit` | a song entered in the browser build, then rendered by `songplay` | sample-exact |

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

`data/` holds the ROM-derived inputs the engine and the browser build need, so
neither requires a ROM. Regenerating the test vectors does; supply your own
dump:

```
ds10plus.nds     sha1 85064992fe671f6e34ffeb01570c9cce56a6a245
```

Tooling: `python3` with `unicorn` and `pillow`, `arm-none-eabi-objdump`, and
Ghidra for the disassembly work. The browser build needs the Emscripten SDK.
`make test-2d` needs a melonDS core built without the Qt frontend; `make ndsref`
prints the exact commands if it is missing.

## Usage

```sh
make serve                  # build for the browser and serve it on :8537
make test                   # the audio engine, against the original
make test-2d                # the 2D hardware, against melonDS
make songplay               # out/songplay song.dat -o out.wav
make test-songwalk          # a whole arrangement, the ROM's own render against ours
python3 tools/songtext.py   # dump|load|check, the text format
python3 tools/songhw.py     # play a song on the ROM in melonDS and capture the audio

make recon                  # extract everything from the ROM into out/
make sweep                  # walk the whole interface, snapshotting each screen
```

`DS10_ROM` overrides the ROM path (default `ds10plus.nds`).

## Tools

| file | what |
|------|------|
| `tools/ndsx.py` | NDS header, NitroFS walk, BLZ decode, extraction, autoload memory map, patch dump, register classification |
| `tools/emu.py` | per-CPU Unicorn image and function-call harness — the oracle the audio verification depends on |
| `tools/oracle.py` | renders real DS-10 audio by executing the original ARM9 code, and writes the test vectors |
| `tools/ndsref/` | headless melonDS: dumps the whole 2D state and the framebuffers it produced, and with `-a` the SPU's output |
| `tools/snap.py` | reads those dumps; `tools/sweep.py` drives the interface to produce them |
| `tools/nitro.py` | the NITRO asset containers, and a background composed the way the hardware would |
| `tools/sav.py` | a `.ds10save`: list, extract or pack a song slot, unpack the whole thing to text and back |
| `tools/songtext.py` | a song as editable text and back, byte for byte — [`docs/songtext.md`](docs/songtext.md) |
| `tools/songwalk.py` | a whole arrangement rendered twice, by the ROM's own code and by `out/songplay`, compared sample for sample |
| `tools/songhw.py` | plays a song on the ROM under melonDS and captures it, for listening rather than diffing |
| `tools/uibyte.py` | what one byte of a song does: change it, boot twice, diff the framebuffers |
| `tools/widgets.py` | recovers sprite widgets from a captured frame by rebuilding their OAM |
| `tools/probe.py` | branch-target validation, lookup-table detection, xrefs, multiply density |

## Layout

```
src/core/  the audio engine
src/gfx/   the DS 2D hardware, and the asset containers
src/app/   the interface, and the native tools built on it
src/web/   the browser build's entry points
web/       the page that hosts it
data/      ROM-derived inputs, committed
out/       everything extracted or built (gitignored)
```
