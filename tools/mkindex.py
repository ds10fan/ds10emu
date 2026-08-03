#!/usr/bin/env python3
"""mkindex - build the listening page for out/audio.

Reads every .mp3 there, pulls the song's own header out of the matching .dat
(next to the audio, or back in the extracted NitroFS) and writes index.html.
"""
import glob
import html
import os
import struct
import wave

DIR = "out/audio"


def source(name):
    for p in ("%s/%s.ds10song" % (DIR, name),
              "out/nitrofs/data/demoSP/%s.dat" % name[:-3],
              "out/nitrofs/data/demo/%s.dat" % name):
        if os.path.exists(p):
            return p
    return None


def main():
    rows = []
    for mp3 in sorted(glob.glob("%s/*.mp3" % DIR)):
        name = os.path.basename(mp3)[:-4]
        wav = "%s/%s.wav" % (DIR, name)
        w = wave.open(wav)
        secs = w.getnframes() / w.getframerate()
        w.close()
        src = source(name)
        if not src:
            continue
        p = open(src, "rb").read()[16:]
        title = p[0x14:0x1A].decode("latin1").rstrip("\0")
        ver = struct.unpack_from("<H", p, 0)[0]
        tempo = struct.unpack_from("<I", p, 4)[0]
        swing = struct.unpack_from("<H", p, 0x0A)[0]
        chain = struct.unpack_from("<100H", p, 0x24)
        bars = next((i for i, v in enumerate(chain) if v == 0xFFFF), 100)
        rows.append((name, title, ver, tempo, swing, bars, secs))
    # The synthetic test tones first, then the ROM's demos.
    rows.sort(key=lambda r: (r[0].startswith("ds10"), r[0]))

    body = "\n".join(
        "<tr><td class=n>%s</td><td class=t>%s</td><td>v%d</td><td>%d</td>"
        "<td>%d%%</td><td>%d</td><td>%d:%02d</td>"
        "<td><audio controls preload=none src=\"%s.mp3\"></audio></td>"
        "<td class=l><a href=\"%s.mp3\">mp3</a> <a href=\"%s.wav\">wav</a>"
        "%s</td></tr>"
        % (html.escape(n), html.escape(t), v, bpm, sw, ba,
           int(s) // 60, int(s) % 60, n, n, n,
           " <a href=\"%s.dat\">dat</a>" % n
           if os.path.exists("%s/%s.ds10song" % (DIR, n)) else "")
        for n, t, v, bpm, sw, ba, s in rows)

    open("%s/index.html" % DIR, "w").write("""<!doctype html>
<meta charset=utf-8><title>KORG DS-10 PLUS - decompiled render</title>
<style>
body{font:14px/1.5 system-ui,sans-serif;margin:2rem auto;max-width:62rem;padding:0 1rem;
     background:#14161a;color:#e6e6e6}
h1{font-size:1.3rem;margin:0 0 .2rem} p{color:#9aa4b2;margin:.2rem 0 1.2rem}
table{border-collapse:collapse;width:100%} th,td{padding:.45rem .6rem;text-align:left}
th{font-size:.75rem;text-transform:uppercase;letter-spacing:.05em;color:#7d8794;
   border-bottom:1px solid #2a2f37}
tr:nth-child(even) td{background:#191c21} td{border-bottom:1px solid #21252b}
.n{font-family:ui-monospace,monospace;font-size:.85rem}
.t{font-weight:600} .l a{color:#6ea8fe;text-decoration:none;margin-right:.4rem}
audio{height:32px;width:13rem;vertical-align:middle}
code{background:#21252b;padding:.1rem .35rem;border-radius:3px}
</style>
<h1>KORG DS-10 PLUS &mdash; rendered by the decompiled engine</h1>
<p>Every sample produced by <code>out/songplay</code>, a C reimplementation of the
ARM9 audio engine. 32768&nbsp;Hz stereo; MP3 resampled to 44.1&nbsp;kHz at
192&nbsp;kbit/s. <code>scale</code> is a synthetic song written by
<code>tools/mksong.py</code> &mdash; one sawtooth, filter open, a major scale up and
down &mdash; as a check that notes come out at the right pitches.</p>
<table>
<tr><th>file<th>name<th>fmt<th>bpm<th>swing<th>bars<th>length<th>listen<th>files</tr>
""" + body + "\n</table>\n")
    print("%d entries" % len(rows))


main()
