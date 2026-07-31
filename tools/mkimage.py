#!/usr/bin/env python3
"""Build a single sparse ARM9 image for Ghidra.

Spans ITCM 0x01FF8000 through the end of the static module at 0x02088820, so
that code in ITCM and code in main RAM land in one program at their real
addresses and cross-references between them resolve. Import as ARM:LE:32:v5t
with auto-analysis off, set the image base to 0x01FF8000, then analyse.

Not included, because they are separate regions and not needed yet:
  DTCM     0x02FE0000 (0xA0 bytes)
  overlay0 0x02160AC0 (0x1980 bytes, sits in bss)
"""
import os

BASE = 0x01FF8000
END = 0x02088820
OUT = "out"

PARTS = [(0x01FF8000, "arm9_01ff8000.bin"), (0x02004000, "arm9.bin")]


def main():
    img = bytearray(END - BASE)
    for addr, fn in PARTS:
        with open(os.path.join(OUT, fn), "rb") as f:
            data = f.read()
        off = addr - BASE
        if off < 0 or off + len(data) > len(img):
            raise SystemExit(f"{fn} does not fit at {addr:#x}")
        img[off:off + len(data)] = data
        print(f"  {fn:24} @ {addr:#010x}  {len(data)} bytes")
    dst = os.path.join(OUT, "arm9_ghidra.bin")
    with open(dst, "wb") as f:
        f.write(img)
    print(f"{dst}: {len(img)} bytes, base {BASE:#x}..{END:#x}")


if __name__ == "__main__":
    main()
