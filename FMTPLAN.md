# FMTPLAN — the file-format worklist

Temporary. Delete this file when every item is done.

Status: `todo`, `doing`, `done`, or `dropped`.

| # | item | decision | status |
|---|---|---|---|
| 1 | Suffixes | `.ds10song`, `.ds10patch`, `.ds10save`, and `.ds10song.txt` for the text | done |
| 2 | Gate default `25%` vs `tie` | show a real user song both ways, then decide | todo |
| 3 | Lane retarget syntax | a lane never retargets — find out what those 31 slots really are, then drop the syntax | todo |
| 4 | `bank.txt` flat vs sections | show both, then decide | todo |
| 5 | `tempo 127` not `tempo = 127` | keep as is | done |
| 6 | `"XLEAD\b"` | keep. 0x08 is probably a glyph, not padding — render it from the game's own font to settle it | todo |
| 7 | `tools/mksaveindex.py` | delete — done in b74a275, confirm nothing refers to it | todo |
| 8 | Pattern `+0x25C`, bytes 1/4/5/6/9 | do they vary, and are they the octave/view state? Measure, then name what is nameable | todo |
| 9 | Stale docs | fix `recon.md` §14's pattern table and the tail-block description, and `src/core/song.h` | todo |
| 10 | payload `+0x2EFC`, 168 bytes | show what is in it | todo |
| 11 | payload `+0x2FA4`, 56 bytes | is it read by the original's code? | todo |
| 12 | `drumN extra`, 9 bytes per drum | is it read by the original's code? | todo |
| 13 | Mixer knobs 15, 16 | treat as nonexistent; keep storing them, say so in the docs | todo |
| 14 | Gate `hold` (0xFF), synth `retrig` | find where a 0xFF gate can come from at all; confirm synth `retrig` cannot | todo |

## Notes as they are settled

### 1 — suffixes

| format | suffix | example |
|---|---|---|
| song record | `.ds10song` | `data/saves/ds10plus_02.ds10song` |
| song as text | `.ds10song.txt` | `slot02.ds10song.txt` |
| instrument record | `.ds10patch` | (nothing writes one yet) |
| instrument bank as text | `.ds10patch.txt` | `bank.ds10patch.txt` |
| cartridge save | `.ds10save` | `songs/DS10PLUS.ds10save` |

The text suffix keeps both facts - which format, and that it is text - so an
editor opens it and `songtext load x.ds10song.txt` drops the `.txt` to name its
output. Names inside the ROM stay `.dat`: they are the ROM's, not ours.

Renamed the 6 saves and the 97 extracted songs, and updated sav.py, songtext.py,
songtext.c, mksong.py, mkindex.py, the four probes, sweep.py, the Makefile and the
docs. Verified afterwards: 109 songs round-trip in C, 108 in Python, and all six
saves still unpack to text and repack byte-identically.
