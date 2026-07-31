/* Differential test: our song file parser against the original.
 *
 * Vectors from `python3 tools/oracle.py song-vectors`. For each song .dat in the
 * ROM the generator points the game's own pattern accessors (0x02033F8C for a
 * step, 0x020343B4 for an automation record, 0x02033FE4 for the pattern length)
 * at a byte image of the file and records what they return. So the addressing
 * being checked here - pattern stride, header size, track stride, sign extension
 * - is the original's, not my reading of it.
 *
 * Three things are checked per song:
 *   1. the MD5 header, against our own MD5 over the payload;
 *   2. every step and automation record, and every pattern length, against the
 *      original;
 *   3. parse then serialise reproduces the file byte for byte, which is what
 *      makes the parse total: a field I failed to name still survives.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/md5.h"
#include "../src/core/song.h"

#define MAGIC "SNG1"

static FILE *g;

static void rd(void *p, size_t n)
{
    if (fread(p, 1, n, g) != n) {
        fprintf(stderr, "unexpected end of vector file\n");
        exit(2);
    }
}

static uint32_t rd32(void)
{
    unsigned char b[4];

    rd(b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/song.bin";
    char magic[4];
    uint32_t nsongs, np, nt, ns, i;
    unsigned long total = 0, bad = 0;
    static uint8_t blob[16 + 12252], back[16 + 12252];
    static ds10_song song;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py song-vectors\n", path);
        return 2;
    }
    rd(magic, 4);
    if (memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    nsongs = rd32();
    np = rd32();
    nt = rd32();
    ns = rd32();
    if (np != DS10_SONG_PATTERNS || nt != DS10_SONG_TRACKS ||
        ns != DS10_SONG_STEPS) {
        fprintf(stderr, "vector geometry %u/%u/%u unexpected\n", np, nt, ns);
        return 2;
    }

    for (i = 0; i < nsongs; i++) {
        char name[128];
        uint32_t nlen, blen, counts[DS10_SONG_PATTERNS];
        uint8_t digest[16];
        uint32_t p, t, s, k;
        size_t wrote;
        int rc;

        nlen = rd32();
        if (nlen >= sizeof name) {
            fprintf(stderr, "song name too long\n");
            return 2;
        }
        rd(name, nlen);
        name[nlen] = 0;
        blen = rd32();
        if (blen > sizeof blob) {
            fprintf(stderr, "%s: %u bytes, too big\n", name, blen);
            return 2;
        }
        for (k = 0; k < DS10_SONG_PATTERNS; k++)
            counts[k] = rd32();
        rd(blob, blen);

        /* 1. the hash header */
        total++;
        ds10_md5(blob + 16, blen - 16, digest);
        if (memcmp(digest, blob, 16) != 0) {
            fprintf(stderr, "%s: MD5 header mismatch\n", name);
            bad++;
        }

        rc = ds10_song_parse(&song, blob, blen);
        if (rc != DS10_SONG_OK) {
            fprintf(stderr, "%s: parse failed, status %d\n", name, rc);
            bad++;
            /* Still drain this song's records so the stream stays aligned. */
            for (k = 0; k < np * (nt + DS10_SONG_LANES) * ns; k++)
                rd32();
            continue;
        }

        /* 2. every record, against the original's accessors */
        for (p = 0; p < np; p++) {
            total++;
            if (song.pattern[p].steps != counts[p]) {
                fprintf(stderr, "%s pattern %u: steps %u, want %u\n", name, p,
                        song.pattern[p].steps, counts[p]);
                bad++;
            }
        }
        for (p = 0; p < np; p++)
            for (t = 0; t < nt; t++)
                for (s = 0; s < ns; s++) {
                    unsigned char w[4];
                    const ds10_song_step *st = &song.pattern[p].step[t][s];

                    rd(w, 4);
                    total++;
                    if (st->note != (int8_t)w[0] || st->gate != (int8_t)w[1] ||
                        st->b2 != (int8_t)w[2] || st->b3 != (int8_t)w[3]) {
                        if (bad < 8)
                            fprintf(stderr,
                                    "%s p%u t%u s%u: got %d,%d,%d,%d "
                                    "want %d,%d,%d,%d\n", name, p, t, s,
                                    st->note, st->gate, st->b2, st->b3,
                                    (int8_t)w[0], (int8_t)w[1], (int8_t)w[2],
                                    (int8_t)w[3]);
                        bad++;
                    }
                }
        for (p = 0; p < np; p++)
            for (t = 0; t < DS10_SONG_LANES; t++)
                for (s = 0; s < ns; s++) {
                    unsigned char w[4];
                    const ds10_song_auto *a = &song.pattern[p].lane[t][s];

                    rd(w, 4);
                    total++;
                    if (a->raw[0] != (int8_t)w[0] || a->raw[1] != (int8_t)w[1] ||
                        a->knob[0] != (int8_t)w[2] ||
                        a->knob[1] != (int8_t)w[3]) {
                        if (bad < 8)
                            fprintf(stderr,
                                    "%s p%u lane t%u s%u: got %d,%d,%d,%d "
                                    "want %d,%d,%d,%d\n", name, p, t, s,
                                    a->raw[0], a->raw[1], a->knob[0],
                                    a->knob[1], (int8_t)w[0], (int8_t)w[1],
                                    (int8_t)w[2], (int8_t)w[3]);
                        bad++;
                    }
                }

        /* 3. round trip */
        total++;
        wrote = ds10_song_write(&song, back);
        if (wrote != blen || memcmp(back, blob, blen) != 0) {
            size_t j, first = blen;

            for (j = 0; j < blen && j < wrote; j++)
                if (back[j] != blob[j]) {
                    first = j;
                    break;
                }
            fprintf(stderr, "%s: round trip differs at %#lx (wrote %lu of %u)\n",
                    name, (unsigned long)first, (unsigned long)wrote, blen);
            bad++;
        }
    }
    fclose(g);

    printf("%s: %lu checks over %u songs, %lu mismatches\n",
           bad ? "FAIL" : "PASS", total, nsongs, bad);
    return bad ? 1 : 0;
}
