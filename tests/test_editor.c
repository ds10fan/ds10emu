/* The editor model, checked against the ROM's own file format.
 *
 * The point of this test is that an edit is not just a change to some
 * in-memory struct of ours: it has to survive being written out as a DS-10
 * song and read back, hash and all. If it does, the hardware would load it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/app/editor.h"

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

static size_t slurp(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

int main(void)
{
    static uint8_t blob[16 + 12252], out[16 + 12252], out2[16 + 12252];
    static ds10_editor e;
    static ds10_song back;
    size_t n, wrote, wrote2;
    int i, t, s;

    n = slurp("data/nitrofs/demo/ds10init.dat", blob, sizeof blob);
    check(ds10_editor_init(&e, blob, n) == DS10_SONG_OK, "init parses");

    /* The blank template is blank: no notes anywhere, no arrangement, and
     * every step at the default gate. */
    {
        int notes = 0, offgate = 0;

        for (i = 0; i < DS10_SONG_PATTERNS; i++)
            for (t = 0; t < DS10_SONG_TRACKS; t++)
                for (s = 0; s < DS10_SONG_STEPS; s++) {
                    if ((uint8_t)e.song.pattern[i].step[t][s].note !=
                        DS10_EDIT_REST)
                        notes++;
                    if (e.song.pattern[i].step[t][s].gate !=
                        DS10_EDIT_GATE_DEFAULT)
                        offgate++;
                }
        check(notes == 0, "blank song has no notes");
        check(offgate == 0, "blank song is all default gate");
        check(ds10_editor_bars(&e) == 0, "blank song has no bars");
    }

    /* Enter something: a synth line, a kick on every fourth step, and a
     * couple of the other three bytes moved off their defaults. */
    for (i = 0; i < 16; i += 2) ds10_editor_note(&e, 0, i, 60 + (i % 12));
    for (i = 0; i < 16; i += 4) ds10_editor_note(&e, 2, i, 60);
    ds10_editor_gate(&e, 0, 0, 6);
    ds10_editor_vel(&e, 0, 2, 100);
    ds10_editor_pan(&e, 0, 4, 20);
    ds10_editor_steps(&e, 12);
    ds10_editor_knob(&e, 0, 11, 90);
    ds10_editor_mixer(&e, 17, 110);
    ds10_editor_tempo(&e, 140);
    ds10_editor_swing(&e, 62);
    ds10_editor_name(&e, "TEST");

    check(ds10_editor_get_note(&e, 0, 0) == 60, "note read back");
    check(ds10_editor_get_note(&e, 0, 1) == -1, "rest reads as -1");
    check(ds10_editor_get_gate(&e, 0, 0) == 6, "gate read back");
    check(ds10_editor_get_gate(&e, 0, 2) == DS10_EDIT_GATE_DEFAULT,
          "untouched gate keeps its default");
    check(ds10_editor_get_steps(&e) == 12, "pattern length");

    /* Clearing a note must not disturb the gate. */
    ds10_editor_note(&e, 0, 0, -1);
    check(ds10_editor_get_note(&e, 0, 0) == -1, "note cleared");
    check(ds10_editor_get_gate(&e, 0, 0) == 6, "clearing a note keeps gate");
    ds10_editor_note(&e, 0, 0, 60);

    /* The arrangement. */
    ds10_editor_chain_set(&e, 0, 0);
    ds10_editor_chain_set(&e, 1, 1);
    ds10_editor_chain_set(&e, 2, 0);
    check(ds10_editor_bars(&e) == 3, "three bars");
    ds10_editor_chain_insert(&e, 1, 5);
    check(ds10_editor_bars(&e) == 4, "insert extends");
    check(ds10_editor_chain_get(&e, 1) == 5, "insert lands");
    check(ds10_editor_chain_get(&e, 2) == 1, "insert shifts the rest");
    ds10_editor_chain_remove(&e, 1);
    check(ds10_editor_bars(&e) == 3, "remove shortens");
    check(ds10_editor_chain_get(&e, 1) == 1, "remove closes the gap");
    /* Setting far past the end appends rather than leaving a hole. */
    ds10_editor_chain_set(&e, 50, 2);
    check(ds10_editor_bars(&e) == 4, "a set past the end appends");

    /* The drums' own patches live in the song's tail, which the parser
     * round-trips verbatim rather than naming - so an edit there is the one
     * most likely to be quietly dropped. */
    ds10_editor_drum_knob(&e, 0, 11, 77);
    ds10_editor_drum_knob(&e, 3, 23, 44);
    check(ds10_editor_get_drum_knob(&e, 0, 11) == 77, "drum knob reads back");
    check(ds10_editor_get_drum_knob(&e, 3, 23) == 44, "and the fourth drum");
    check(ds10_editor_get_drum_knob(&e, 1, 11) != 77 ||
          ds10_editor_get_drum_knob(&e, 0, 11) == 77,
          "the drums have separate patches");

    /* Round trip: what we wrote has to parse, hash and all, and come back
     * with every edit intact. */
    wrote = ds10_editor_export(&e, out);
    check(wrote == e.song.payload_len + 16, "wrote a whole file");
    check(ds10_song_parse(&back, out, wrote) == DS10_SONG_OK,
          "the exported file parses and its hash checks");

    check(back.tempo == 140, "tempo survives");
    check(back.swing == 62, "swing survives");
    check(memcmp(back.name, "TEST  ", 6) == 0, "name survives");
    check(ds10_song_bars(&back) == 4, "arrangement survives");
    check(back.pattern[0].steps == 12, "pattern length survives");
    check(back.pattern[0].knobs[0][11] == 90, "patch knob survives");
    check(back.pattern[0].mixer[17] == 110, "mixer knob survives");
    for (i = 0; i < 16; i += 2)
        check((uint8_t)back.pattern[0].step[0][i].note == 60 + (i % 12),
              "synth note survives");
    for (i = 0; i < 16; i += 4)
        check((uint8_t)back.pattern[0].step[2][i].note == 60,
              "drum note survives");
    check(back.pattern[0].step[0][0].gate == 6, "gate survives");
    check(back.pattern[0].step[0][2].vel == 100, "velocity survives");
    check(back.pattern[0].step[0][4].pan == 20, "pan survives");
    check(ds10_song_drum_knobs(&back, 0)[11] == 77, "a drum knob survives");
    check(ds10_song_drum_knobs(&back, 3)[23] == 44, "and the fourth drum's");

    /* Writing the same song twice gives the same bytes - no uninitialised
     * padding leaking into the hash. */
    wrote2 = ds10_editor_export(&e, out2);
    check(wrote == wrote2 && memcmp(out, out2, wrote) == 0,
          "export is deterministic");

    /* Adopting a demo and exporting it unchanged reproduces the file, which
     * is what says the editor's own copy is lossless. */
    {
        static ds10_song demo;
        static uint8_t dblob[16 + 12252], dout[16 + 12252];
        size_t dn = slurp("data/nitrofs/demo/ds10pdemo00.dat", dblob,
                          sizeof dblob);
        size_t dw;

        check(ds10_song_parse(&demo, dblob, dn) == DS10_SONG_OK,
              "demo parses");
        ds10_editor_adopt(&e, &demo);
        dw = ds10_editor_export(&e, dout);
        check(dw == dn && memcmp(dout, dblob, dn) == 0,
              "adopt then export reproduces the demo byte for byte");
    }

    /* Whole-pattern operations. */
    ds10_editor_init(&e, blob, n);
    ds10_editor_note(&e, 0, 3, 72);
    ds10_editor_pattern_copy(&e, 0, 4);
    check((uint8_t)e.song.pattern[4].step[0][3].note == 72, "copy");
    ds10_editor_transpose(&e, 4, 0, -12);
    check((uint8_t)e.song.pattern[4].step[0][3].note == 60, "transpose");
    check((uint8_t)e.song.pattern[4].step[0][2].note == DS10_EDIT_REST,
          "transpose leaves rests alone");
    ds10_editor_track_clear(&e, 4, 0);
    check((uint8_t)e.song.pattern[4].step[0][3].note == DS10_EDIT_REST,
          "track clear");

    if (fails) {
        fprintf(stderr, "FAIL: %d checks\n", fails);
        return 1;
    }
    printf("PASS: editor round trips through the song format\n");
    return 0;
}
