#!/usr/bin/env python3
"""songwalk - a whole arrangement, the ROM's own render against ours, sample exact.

    python3 tools/songwalk.py check data/saves/new_00.ds10song [bars]
    python3 tools/songwalk.py sweep [bars]        # every song in the corpus

The ROM's side is `tools/oracle.py`, which runs the original's sequencer, voices,
mixer and router out of the extracted image and reads the mixer's own stereo pair -
the mix bus, not a speaker. Given the song's chain it walks the arrangement the way
the transport does: the sequencer asks its player for a pattern once a bar and the
original's own installer takes it from there.

Our side is `out/songplay` itself, run as it ships and its WAV read back. That is
deliberate: tests/test_songrender.c rebuilds the engine inside the test, which means
a bug in how songplay wires things up cannot fail it. Here the thing under test is
the program.

What this covers that test-songrender does not: the bar boundary, the pattern
change, the chain walk, and whatever the original does per bar that we have not
thought of. That is where every bug this project has shipped has been.
"""
import os
import struct
import subprocess
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import songtext as st                                          # noqa: E402

RATE = 32768
FRAME = 64
OUT = "out/songwalk"


def song_chain(payload):
    chain = list(struct.unpack_from("<100H", payload, 0x24))
    if 0xFFFF in chain:
        chain = chain[:chain.index(0xFFFF)]
    return chain


def our_drumbufs(song):
    """The four drum buffers songplay renders, for the ROM side to use as well.
    Their content is checked against a running DS by `make test-drumbuf`; holding
    them constant here keeps this test on the wiring, and stops it comparing
    against the oracle's own weaker inference of them."""
    prefix = os.path.join(OUT, os.path.splitext(os.path.basename(song))[0])
    subprocess.run(["./out/songplay", "-b", "1", "-D", prefix,
                    "-o", prefix + ".probe.wav", song],
                   check=True, stdout=subprocess.DEVNULL)
    return [open("%s.drum%d.raw" % (prefix, d), "rb").read() for d in range(4)]


def capture(song, bars):
    """The ROM's mix bus for `bars` bars, cached because it takes a few seconds."""
    import oracle
    blob = open(song, "rb").read()
    payload = blob[16:]
    chain = song_chain(payload)
    tempo = struct.unpack_from("<I", payload, 4)[0]
    bars = min(bars, len(chain))
    frames = int(bars * 4 * 60.0 / tempo * RATE / FRAME)
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "%s.%dbar.bin"
                        % (os.path.splitext(os.path.basename(song))[0], bars))
    if os.path.exists(path):
        return path, chain, tempo, bars
    print("capturing %d frames (%d bars of %s) from the ROM..."
          % (frames, bars, os.path.basename(song)))
    masks = [st.mute_get(payload, st.MUTE_A - 0, b) if len(payload) > st.MUTE_A + 84
             else 0 for b in range(len(chain))]
    r = oracle.capture_songrender(blob, chain[0], frames, chain=chain,
                                  drumbufs=our_drumbufs(song),
                                  mute=masks if any(masks) else None)
    out = r[-1]
    with open(path, "wb") as f:
        # All eight buffers: the six machine outputs the ROM's own voices wrote,
        # then the mixer's stereo pair. Per-channel is what localises a
        # divergence to a track instead of leaving it in the mix.
        f.write(b"SWLK" + struct.pack("<II", len(out), FRAME))
        for frame in out:
            for ch in range(8):
                f.write(struct.pack("<%dh" % FRAME, *frame[ch]))
    return path, chain, tempo, bars


def read_vector(path):
    """The eight buffers, each as one flat list of samples."""
    d = open(path, "rb").read()
    if d[:4] != b"SWLK":
        raise SystemExit("%s: bad vector - delete it and re-capture" % path)
    n, frame = struct.unpack_from("<II", d, 4)
    chans = [[] for _ in range(8)]
    off = 12
    for _ in range(n):
        for ch in range(8):
            chans[ch] += struct.unpack_from("<%dh" % frame, d, off)
            off += frame * 2
    return chans


def ours_channels(song, bars):
    """songplay's own six machine buffers, via -C."""
    prefix = os.path.join(OUT, os.path.splitext(os.path.basename(song))[0])
    subprocess.run(["./out/songplay", "-b", str(bars), "-C", prefix,
                    "-o", prefix + ".ours.wav", song],
                   check=True, stdout=subprocess.DEVNULL)
    out = []
    for ch in range(6):
        d = open("%s.ch%d.raw" % (prefix, ch), "rb").read()
        out.append(list(struct.unpack("<%dh" % (len(d) // 2), d)))
    return out


def ours(song, bars):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "%s.ours.wav"
                        % os.path.splitext(os.path.basename(song))[0])
    subprocess.run(["./out/songplay", "-b", str(bars), "-o", path, song],
                   check=True, stdout=subprocess.DEVNULL)
    w = wave.open(path)
    n = w.getnframes()
    s = struct.unpack("<%dh" % (n * 2), w.readframes(n))
    return list(s[0::2]), list(s[1::2])


TRACK = ("syn1", "syn2", "drum1", "drum2", "drum3", "drum4", "mixL", "mixR")


def first_diff(a, b):
    """Index of the first difference, or -1. Bulk-compares as bytes and only
    walks the chunk that disagrees - a sweep over the corpus is otherwise all
    Python loop."""
    n = min(len(a), len(b))
    if n == 0:
        return -1
    STEP = 4096
    for base in range(0, n, STEP):
        end = min(base + STEP, n)
        if (struct.pack("<%dh" % (end - base), *a[base:end]) ==
                struct.pack("<%dh" % (end - base), *b[base:end])):
            continue
        for i in range(base, end):
            if a[i] != b[i]:
                return i
    return -1


def check(song, bars=8):
    path, chain, tempo, bars = capture(song, bars)
    rom = read_vector(path)
    hl, hr = rom[6], rom[7]
    ours_ch = ours_channels(song, bars)
    ol, orr = ours(song, bars)
    n = min(len(hl), len(ol))
    print("%s: %d BPM, chain %s" % (os.path.basename(song), tempo,
                                    " ".join(str(c) for c in chain[:bars])))
    print("  ROM %d samples, ours %d, comparing %d (%.2f s)"
          % (len(hl), len(ol), n, n / float(RATE)))
    dl, dr = first_diff(hl[:n], ol[:n]), first_diff(hr[:n], orr[:n])
    first = min(x for x in (dl, dr) if x >= 0) if (dl >= 0 or dr >= 0) else -1
    bad = worst = 0
    if first >= 0:
        for i in range(first, n):
            d = abs(hl[i] - ol[i]) + abs(hr[i] - orr[i])
            if d:
                bad += 1
                if d > worst:
                    worst = d
    # Per channel, so a divergence lands on a track rather than in the mix.
    print("\n  channel   ROM rms   ours rms   first differing sample")
    for ch in range(6):
        a, b = rom[ch], ours_ch[ch]
        m = min(len(a), len(b))
        at = next((i for i in range(m) if a[i] != b[i]), -1)
        ra = (sum(v * v for v in a[:m]) / m) ** 0.5
        rb = (sum(v * v for v in b[:m]) / m) ** 0.5
        print("  %-8s %9.0f %10.0f   %s"
              % (TRACK[ch], ra, rb, "identical" if at < 0 else str(at)))
    if not bad:
        print("  PASS: identical")
        return 0
    print("  FAIL: %d of %d samples differ (%.1f%%), first at %d (frame %d,"
          " bar %.2f), worst %d"
          % (bad, n, 100.0 * bad / n, first, first // FRAME,
             first / (4 * 60.0 / tempo * RATE), worst))
    for i in range(first, min(first + 6, n)):
        print("    %6d  ROM %7d %7d   ours %7d %7d" % (i, hl[i], hr[i],
                                                       ol[i], orr[i]))
    return 1


def corpus():
    """Everything committed that is a song and has an arrangement to walk."""
    import glob
    out = []
    for f in (sorted(glob.glob("data/saves/*.ds10song")) +
              [x for x in sorted(glob.glob("data/nitrofs/demo/*.dat"))
               if "synth" not in x]):
        payload = st.read_song(f)
        if song_chain(payload):
            out.append(f)
    return out


def one(args):
    """One song, in a worker: returns a line and whether it matched."""
    song, bars = args
    try:
        path, chain, tempo, bars = capture(song, bars)
        rom = read_vector(path)
        ours_ch = ours_channels(song, bars)
        ol, orr = ours(song, bars)
        n = min(len(rom[6]), len(ol))
        at = min(x for x in (first_diff(rom[6][:n], ol[:n]),
                             first_diff(rom[7][:n], orr[:n]), 1 << 60)
                 if x >= 0)
        if at == 1 << 60:
            return song, tempo, bars, n, None, ""
        # Which channel went first says where to look.
        who = [TRACK[c] for c in range(6)
               if first_diff(rom[c], ours_ch[c]) >= 0]
        return (song, tempo, bars, n, at, ",".join(who) or "mix only")
    except Exception as exc:                                  # noqa: BLE001
        return song, 0, bars, 0, -1, "error: %s" % exc


def sweep(bars=6, jobs=None):
    """Every song in the corpus, in parallel. Prints a line each and a summary."""
    import multiprocessing
    songs = corpus()
    jobs = jobs or max(1, (os.cpu_count() or 2) - 2)
    print("sweeping %d songs, %d bars each, %d at a time\n" % (len(songs), bars,
                                                               jobs))
    bad = []
    with multiprocessing.Pool(jobs) as pool:
        for song, tempo, nbars, n, at, who in pool.imap(
                one, [(s, bars) for s in songs]):
            name = os.path.basename(song)
            if at is None:
                print("  %-26s %3d BPM %2d bars %8d samples  identical"
                      % (name, tempo, nbars, n))
            else:
                bad.append((name, at, who))
                print("  %-26s %3d BPM %2d bars %8d samples  DIFFERS at %d (%s)"
                      % (name, tempo, nbars, n, at, who))
    print("\n%d of %d identical" % (len(songs) - len(bad), len(songs)))
    for name, at, who in bad:
        print("  %-26s first difference at sample %s in %s" % (name, at, who))
    return 1 if bad else 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    if sys.argv[1] == "sweep":
        return sweep(int(sys.argv[2]) if len(sys.argv) > 2 else 6)
    if sys.argv[1] != "check" or len(sys.argv) < 3:
        print(__doc__)
        return 1
    bars = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    return check(sys.argv[2], bars)


if __name__ == "__main__":
    sys.exit(main())
