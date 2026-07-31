#!/usr/bin/env python3
"""widgets - recover the interface's sprite widgets out of a captured frame.

Every moving thing on a DS-10 screen is one 64-byte object (`ds10_obj`,
decompiled from 0x0202a820) holding a run of sprites and a pointer to the OAM
entries it built. That makes them findable: walk main RAM, read 16 words, and
rebuild the OAM the way the game's setter does. If the bytes at the object's
own pointer match what we rebuilt, it is a widget and every field is known.

    python3 tools/widgets.py out/sweep/f01234.snap

Nothing is guessed. A candidate is only accepted if our builder reproduces the
game's own OAM byte for byte, so a false positive would have to be a 64-byte
region that happens to describe the memory it points at.
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import snap

RAM_BASE = 0x02000000
MAX_COUNT = 32

# Two field orders exist. 0x0202a820 is the standalone setter; the version
# inlined into the screen constructors (0x02050108 and friends) carries one
# extra word at index 6 and shifts everything after it along. Both objects are
# 64 bytes. A candidate is tried against both and accepted on whichever
# reproduces the OAM it points at.
LAYOUTS = {
    "set": ["x", "y", "priority", "tile", "palette", "affine", "mode",
            "mosaic", "rot", "shape", "big", "oamptr", "count", "res",
            "xstep", "bank"],
    "inline": ["x", "y", "priority", "tile", "palette", "affine", "_pad",
               "mode", "mosaic", "rot", "shape", "big", "oamptr", "count",
               "xstep", "_tail"],
}


def build(w):
    """The OAM run this object describes. Mirrors src/app/obj.c exactly."""
    out = bytearray()
    for i in range(w["count"]):
        x = (w["xstep"] * i + w["x"]) & 0xFFFFFFFF
        y = w["y"] & 0xFFFFFFFF
        rot = w["rot"] & 0xFFFFFFFF
        if rot in (0x100, 0x300):
            if w["mode"] == 3:
                v = (y & 0xFF) | ((w["affine"] << 25) & 0xFFFFFFFF)
                v |= w["shape"] | (w["mode"] << 10) | (w["mosaic"] << 12) \
                    | ((x & 0x1FF) << 16)
            else:
                v = w["shape"] | (y & 0xFF) | (w["big"] << 13) \
                    | ((w["affine"] << 25) & 0xFFFFFFFF) | (w["mode"] << 10) \
                    | (w["mosaic"] << 12) | ((x & 0x1FF) << 16)
        else:
            a0 = y & 0xFF
            if w["mode"] != 3:
                a0 |= w["big"] << 13
            v = w["shape"] | a0 | (w["mode"] << 10) | (w["mosaic"] << 12) \
                | ((x & 0x1FF) << 16)
        out += struct.pack("<IHH", (rot | v) & 0xFFFFFFFF,
                           (w["tile"] | (w["priority"] << 10)
                            | (w["palette"] << 12)) & 0xFFFF, 0)
    return bytes(out)


def scan(ram):
    """Every widget object in main RAM, as dicts."""
    found = []
    n = len(ram)
    for off in range(0, n - 64, 4):
        vals = struct.unpack_from("<16i", ram, off)
        for kind, fields in LAYOUTS.items():
            w = dict(zip(fields, vals))
            count = w["count"]
            if count < 1 or count > MAX_COUNT:
                continue
            ptr = w["oamptr"] & 0xFFFFFFFF
            po = ptr - RAM_BASE
            if po < 0 or po + count * 8 > n:
                continue
            w["oamptr"] = ptr
            # Bounds the setter's own arithmetic implies; anything outside
            # them would not have produced a sensible sprite.
            if not (0 <= w["priority"] <= 3 and 0 <= w["palette"] <= 15
                    and 0 <= w["mode"] <= 3 and 0 <= w["tile"] <= 0x3FF
                    and w["rot"] in (0, 0x100, 0x300)):
                continue
            want = build(w)
            if ram[po:po + len(want)] != want:
                continue
            # An all-transparent run says nothing; skip it.
            if want == b"\0" * len(want):
                continue
            w["addr"] = RAM_BASE + off
            w["kind"] = kind
            found.append(w)
            break
    return found


def same_run(run, oam, slot):
    """Is this run at `slot` of an OAM half?

    The allocator's copy (0x020689bc) moves three halfwords per entry, not
    four: the fourth is the rotation parameter, which belongs to whoever owns
    the affine slot, not to the sprite. So only the first six bytes of each
    entry are the widget's.
    """
    n = len(run) // 8
    if (slot + n) * 8 > len(oam):
        return False
    for i in range(n):
        if run[i * 8:i * 8 + 6] != oam[(slot + i) * 8:(slot + i) * 8 + 6]:
            return False
    return True


def live(widgets, oam):
    """Those whose entries are actually in OAM, and which engine's."""
    out = []
    for w in widgets:
        run = build(w)
        for engine, half in ((0, oam[:1024]), (1, oam[1024:])):
            for slot in range(128 - len(run) // 8 + 1):
                if same_run(run, half, slot):
                    out.append((engine, slot, w))
                    break
            else:
                continue
            break
    return out


def main():
    path = sys.argv[1]
    s = snap.load(path)
    if not s.ram:
        print("%s: no main RAM in this capture; re-run ndsref with -m" % path)
        return 1
    ws = scan(s.ram)
    lv = live(ws, s.oam)
    print("%s: %d widget objects in RAM, %d of them live in OAM"
          % (path, len(ws), len(lv)))
    for engine, slot, w in sorted(lv, key=lambda t: (t[0], t[1])):
        print("  %s #%-3d @%08x %-6s x=%-4d y=%-3d n=%-2d step=%-3d "
              "tile=%#05x pal=%-2d prio=%d mode=%d rot=%#05x shape=%#06x "
              "big=%d"
              % ("AB"[engine], slot, w["addr"], w["kind"], w["x"], w["y"],
                 w["count"], w["xstep"], w["tile"], w["palette"],
                 w["priority"], w["mode"], w["rot"], w["shape"] & 0xFFFF,
                 w["big"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
