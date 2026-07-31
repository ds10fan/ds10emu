# ds10

Reverse engineering KORG DS-10 PLUS (Nintendo DS) with the goal of a native C
reimplementation: the synthesizer engine first, an SDL 1.2 GUI second.

Multiplayer (DS Wireless Play / WiFi) is out of scope.

## Status

Reconnaissance done, implementation not started.

- [`recon.md`](recon.md) — what the ROM actually contains: container layout, the
  ARM9/ARM7 memory map, NitroFS inventory, patch format, where the DSP code and
  tables live.
- [`PLAN.md`](PLAN.md) — phased plan, and the four-tier verification strategy the
  project is built around.

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
