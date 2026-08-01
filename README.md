# ds10

Reverse engineering KORG DS-10 PLUS (Nintendo DS) into a C reimplementation:
the synthesizer engine, and the interface, running in a browser.

Multiplayer (DS Wireless Play / WiFi) is out of scope.

## Status

**Audio.** Reimplemented and verified sample-exact against the original ARM9
code running under Unicorn. `out/songplay` renders a `.dat` song to a WAV using
nothing but the reimplementation: the oscillator, filter, envelope, LFO,
portamento and amp; the 31-knob patch format; the drum voice and the one-second
buffer a drum patch is rendered into; the mixer's 25 knobs and the effect's
eight; the sequencer's transport, gating, note emission and event routing; the
song file format; automation lanes.

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

What is *not* done: each page's own top-bar buttons — CLR, play, the track
selector — are not wired, so the browser page drives those from HTML controls
instead; `32_PATTERN` and `31_SONG` draw but are not interactive. Captions
still come from the captured frame rather than being rasterised. See
[`TODO.md`](TODO.md) §6.

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
| `make test` | the audio engine against the original under Unicorn | 17 suites |
| `make test-obj` | the sprite widget's OAM against the game's own setter | 171 checks |
| `make test-2d` | the 2D hardware against melonDS, pixel for pixel | 72 frames |
| `make test-page` | each page rebuilt from assets, against melonDS | 22 screens |
| `make test-editor` | an edited song round-tripped through the file format | hash and all |
| `make test-webedit` | a song entered in the browser build, then rendered by `songplay` | sample-exact |

Read `TODO.md` §1 before trusting the audio number — those suites are strong on
DSP and blind on the wiring around it, which is where all four known bugs were.

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
| `tools/ndsref/` | headless melonDS: dumps the whole 2D state and the framebuffers it produced |
| `tools/snap.py` | reads those dumps; `tools/sweep.py` drives the interface to produce them |
| `tools/nitro.py` | the NITRO asset containers, and a background composed the way the hardware would |
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
