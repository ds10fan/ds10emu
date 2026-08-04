#!/usr/bin/env python3
"""songhw - play a whole song on the ROM under melonDS and diff it against ours.

    python3 tools/songhw.py capture data/saves/xfer2_10.ds10song
    python3 tools/songhw.py compare data/saves/xfer2_10.ds10song

`capture` packs the song into slot 0 of a save, boots the ROM, walks the interface
to the SONG page and presses play, and drains the SPU to a 32768 Hz wav.

Getting there: `L` swaps the map onto the touch screen, (229,128) is the map's SONG
box - recon.md said nothing on the map opened 31_SONG, which was wrong - and
(205,8) is the transport's play button on that page. Pressing play anywhere else
loops the current pattern instead of walking the chain, which is how the first
attempt at this went wrong and only listening caught it.

`compare` renders the same song with out/songplay, aligns the two by their energy
envelopes, and reports per-bar level and shape. What it does NOT do is claim
sample-exactness: melonDS resamples the DS's 32728.5 Hz mix through blip_buf, and
the ROM's own playback channel runs at 32768, so the capture is the speaker rather
than the mix bus. The bar envelope is what this can check, and it is enough to see
a muted track that sounds, a pattern that never arrives, or a tempo that drifts.
"""
import os
import struct
import subprocess
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sav                                                     # noqa: E402
import songtext as st                                          # noqa: E402

ROM = os.environ.get("DS10_ROM", "ds10plus.nds")
BASE_SAVE = "songs/DS10PLUS.ds10save"
OUT = "out/songhw"
RATE = 32768
FPS = 59.8261

# The taps: boot past the title, L to put the map on the touch screen, the map's
# SONG box, then play on the song page.
SCRIPT = """1180 touch 128 55
1195 release
2260 touch 128 58
2300 release
2360 touch 78 186
2400 release
2800 key L
2820 up L
2940 touch 229 128
2960 release
3100 touch 205 8
3120 release
"""
PLAY_FRAME = 3120


def capture(song, seconds=30):
    """Boot, play the arrangement, and write what the SPU produced."""
    os.makedirs(OUT, exist_ok=True)
    tag = os.path.splitext(os.path.basename(song))[0]
    save = os.path.join(OUT, tag + ".ds10save")
    if sav.cmd_pack([BASE_SAVE, "0", song, save]) != 0:
        raise SystemExit("could not pack %s" % song)
    script = os.path.join(OUT, tag + ".script")
    with open(script, "w") as f:
        f.write(SCRIPT)
    frames = int(PLAY_FRAME + (seconds + 1) * FPS)
    raw = os.path.join(OUT, tag + ".raw")
    subprocess.run(["./out/ndsref", ROM, "-f", str(frames), "-o", OUT,
                    "-s", script, "-w", save, "-a", raw], check=True,
                   stdout=subprocess.DEVNULL)
    # Keep from the play tap on, as a wav for listening as well as diffing.
    skip = int(PLAY_FRAME / FPS * RATE) * 4
    data = open(raw, "rb").read()[skip:]
    out = os.path.join(OUT, tag + ".hw.wav")
    w = wave.open(out, "wb")
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(data)
    w.close()
    print("%s: %.1f s from the ROM" % (out, len(data) / 4.0 / RATE))
    return out


def read_wav(path):
    w = wave.open(path)
    n = w.getnframes()
    return struct.unpack("<%dh" % (n * w.getnchannels()), w.readframes(n))


def envelope(s, step):
    """Mono RMS every `step` samples - what the alignment and the report use."""
    out = []
    for i in range(0, len(s) - step * 2, step * 2):
        seg = s[i:i + step * 2]
        out.append((sum(v * v for v in seg) / len(seg)) ** 0.5)
    return out


def onset(e):
    """First envelope step that is clearly sounding. A song starts on a beat, so
    this is a far more reliable landmark than correlating a repetitive envelope -
    the first attempt at that locked onto a bar 16 arrangements later."""
    loud = sorted(e)[len(e) // 2]
    for i, v in enumerate(e):
        if v > loud * 0.25:
            return i
    return 0


def align(a, b, window):
    """Line the two up on their onsets, then refine within `window` steps."""
    base = onset(a) - onset(b)
    n = min(len(b), 64)
    mb = sum(b[:n]) / n
    best, at = -2.0, max(base, 0)
    for off in range(max(base - window, 0), base + window + 1):
        if off < 0 or off + n > len(a):
            continue
        seg = a[off:off + n]
        ma = sum(seg) / n
        num = sum((seg[i] - ma) * (b[i] - mb) for i in range(n))
        den = (sum((v - ma) ** 2 for v in seg) *
               sum((v - mb) ** 2 for v in b[:n])) ** 0.5
        c = num / den if den else 0
        if c > best:
            best, at = c, off
    return at, best


def compare(song, seconds=30):
    tag = os.path.splitext(os.path.basename(song))[0]
    hw = os.path.join(OUT, tag + ".hw.wav")
    if not os.path.exists(hw):
        hw = capture(song, seconds)
    ours = os.path.join(OUT, tag + ".ours.wav")
    payload = st.read_song(song)
    tempo = struct.unpack_from("<I", bytes(payload), 4)[0]
    bars = min(int(seconds / (4 * 60.0 / tempo)), 32)
    subprocess.run(["./out/songplay", "-b", str(bars), "-o", ours, song],
                   check=True, stdout=subprocess.DEVNULL)

    bar = int(RATE * 4 * 60.0 / tempo)
    step = bar // 8                      # eight envelope points per bar
    h = envelope(read_wav(hw), step)
    o = envelope(read_wav(ours), step)
    off, c = align(h, o, 8)          # refine within one bar
    print("%s: %d BPM, %d bars, aligned at %+.3f s (envelope correlation %.3f)"
          % (tag, tempo, bars, off * step / float(RATE), c))

    print("\nbar  hardware      ours    ratio")
    hs, os_ = [], []
    for k in range(bars):
        a = h[off + k * 8:off + (k + 1) * 8]
        b = o[k * 8:(k + 1) * 8]
        if len(a) < 8 or len(b) < 8:
            break
        ha = sum(a) / 8
        ob = sum(b) / 8
        hs.append(ha)
        os_.append(ob)
        print("  %2d  %8.0f  %8.0f   %.3f" % (k, ha, ob, ha / ob if ob else 0))
    mh, mo = sum(hs) / len(hs), sum(os_) / len(os_)
    shape = (sum((hs[i] - mh) * (os_[i] - mo) for i in range(len(hs))) /
             ((sum((v - mh) ** 2 for v in hs) *
               sum((v - mo) ** 2 for v in os_)) ** 0.5))
    print("\nmean level ratio %.3f, bar-shape correlation %.3f"
          % (mh / mo, shape))
    return shape


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cmd, song = sys.argv[1], sys.argv[2]
    secs = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    if cmd == "capture":
        capture(song, secs)
    elif cmd == "compare":
        compare(song, secs)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
