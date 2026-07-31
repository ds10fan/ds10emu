# ds10

Reverse engineering KORG DS-10 PLUS (Nintendo DS) with the goal of a native C
reimplementation: the synthesizer engine first, an SDL 1.2 GUI second.

Multiplayer (DS Wireless Play / WiFi) is out of scope.

## Status

The audio engine is reimplemented in C and renders songs. `out/songplay` reads a
`.dat` song and writes a WAV using nothing but the reimplementation.

- [`recon.md`](recon.md) — what the ROM actually contains: container layout, the
  ARM9/ARM7 memory map, NitroFS inventory, the song and patch formats, and the
  engine's addresses and structures.
- [`PLAN.md`](PLAN.md) — phased plan, and the four-tier verification strategy the
  project is built around.
- [`TODO.md`](TODO.md) — what is left, starting with the gap in the test suite
  that four separate bugs have already slipped through.

The engine runs on the **ARM9** at **32768 Hz** in **64-sample frames**: six
voices (2 synth + 4 drum) plus mixer and effect. Decompiled and verified
sample-exact against the original: the oscillator, filter, envelope, LFO,
portamento and amp; the 31-knob patch format; the drum voice and the one-second
buffer a drum patch is rendered into; the mixer's 25 knobs and the effect's eight;
the sequencer's transport, gating, note emission and event routing; the song file
format; and automation lanes.

`make test` runs seventeen differential suites, every one of them comparing
against the original ARM9 code executing under Unicorn. Read `TODO.md` §1 before
trusting that number — the suites are strong on DSP and blind on the wiring
around it.

### Prior art

There is significant earlier work by **thement** on the byte-identical ROM —
a [project log](http://ibawizard.net/~thement/ds10rev.html) (plain HTTP only) and
[`ds10-vst`](https://github.com/thement/ds10-vst), a working VST. That VST is a
mechanical ARM-to-C translation with the ROM image embedded, not a
reimplementation; this project takes the reimplementation road instead, and uses
theirs as a cross-check. See `recon.md` §0 and `PLAN.md` §1.

## Requirements

The ROM is **not** included and nothing derived from it is committed. Supply your
own dump:

```
ds10plus.nds     sha1 85064992fe671f6e34ffeb01570c9cce56a6a245
```

Tooling: `python3` with `unicorn`, `arm-none-eabi-objdump`, and
`libsdl1.2-dev` for the eventual application. Ghidra is used for the
disassembly work.

## Usage

```sh
make recon                  # extract everything from the ROM into out/
python3 tools/ndsx.py fs     # list the NitroFS contents
python3 tools/ndsx.py autoload
python3 tools/emu.py smoke   # verify the memory map by booting both CPUs
python3 tools/probe.py tables
```

`DS10_ROM` overrides the ROM path (default `ds10plus.nds`).

## Tools

| file | what |
|------|------|
| `tools/ndsx.py` | NDS header, NitroFS walk, BLZ decode, extraction, autoload memory map, patch dump, register classification |
| `tools/emu.py` | per-CPU Unicorn image and function-call harness — the oracle the verification strategy depends on |
| `tools/probe.py` | branch-target validation, lookup-table detection, xrefs, multiply density |

## Layout

```
tools/    analysis and extraction scripts
out/      everything extracted from the ROM (gitignored)
```
