# data/ — ROM-derived inputs

Everything here comes out of `ds10plus.nds`. It is committed so the engine, its
tests and the browser build all work without anyone having a copy of the ROM.

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

The GUI's assets, extracted from NitroFS by `python3 tools/ndsx.py extract`.
Stock Nintendo containers:

| ext  | what |
|------|------|
| NCLR | palette |
| NCGR | character/tile graphics |
| NSCR | tilemap (screen) |
| NCBR | bitmap graphics |
| NCER | OAM cell definitions |
| NANR | cell animation |
| NFTR | font |

- `mainbg/` — one NCGR+NSCR pair per screen; the names are the screen list.
- `mainobj/` — the sprites: buttons, knobs, the fonts, the `3D_TEX` textures.
- `data0/` — the shared backgrounds and the KORG logo.
- `str/` — UI strings, one file per language.
- `demo/`, `demoSP/` — the demo songs and the 24 factory patches.

`dummy/` is not copied here: it is placeholder art left over from development,
2.2 MB of it, and nothing loads it.
