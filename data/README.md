# data/ — ROM-derived inputs

`tables.bin` and `nitrofs/` come out of `ds10plus.nds`, `saves/` out of
`songs/*.ds10save`. All of it is committed so the engine and its tests work
without anyone having a copy of the ROM.

## tables.bin

The 16 lookup tables the DSP reads, plus the state the constructors leave a
voice, the mixer and the effect in — which is never zero, and which anything
bit-exact has to start from. Also the 31 knob descriptors' min and range, which
is what an automation lane's offset and scale are derived from.

Regenerate and check it against the ROM:

    make tables

That re-extracts into `out/tables.bin` and `cmp`s the two. Written by
`tools/oracle.py tables`; the layout is read by `src/core/tables.c`.

## nitrofs/

Songs and patches extracted from NitroFS by `python3 tools/ndsx.py extract`:

- `demo/`, `demoSP/` — the demo songs and the 24 factory patches.

## saves/

The songs in `songs/*.ds10save`, one file per written slot, named
`<save>_<slot>.ds10song`. A slot is the same record a `ds10*.dat` is, so these load
like any other song file.

    make data/saves        # rm -rf and re-extract, 97 slots from 6 saves

Written by `tools/sav.py extract`. The same tool lists a save's slots and packs
a song file back into one:

    python3 tools/sav.py list songs/*.ds10save
    python3 tools/sav.py pack songs/NEW.ds10save 3 out/scale.ds10song out/NEW.ds10save

`pack` replaces the slot's 12268 bytes and nothing else — the save's own header
hash covers only its 28 constant bytes, and nothing outside a slot refers to
one. It takes PLUS records only: a slot's payload is a fixed 12252 bytes with
the hash over all of it, and a shorter DS-10 or pre-release song file would have
to have 224 bytes invented for it.
