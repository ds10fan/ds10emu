/* DS-10 song file (`ds10*.dat`, and the save-RAM slots, which are the same
 * thing).
 *
 * A slot is a 16-byte MD5 of everything after it, followed by a byte image of
 * the in-memory song record. The image is written by 0x0205D320 as a handful of
 * unrolled copies whose source and destination offsets are equal, so the file
 * layout IS the struct layout - there is no marshalling, and any byte the parser
 * cannot name still round-trips.
 *
 * Where the file and the live editor model disagree is the header size: the file
 * has 0xEC bytes before the pattern array, the live model 0xB4. Everything from
 * the pattern base onwards is identical, which is why the original's own pattern
 * accessors can be pointed straight at a file image; tests/test_song.c does
 * exactly that.
 *
 * Four payload lengths exist. The format only ever grew by appending, so an
 * older file is a prefix of a newer one:
 *
 *   11212  v0, the earliest demos      ends after the 0x2B2C block
 *   11260  + 48 bytes
 *   12028  v1, DS-10                   + 768 bytes
 *   12252  v2, DS-10 PLUS              + 224 bytes
 */
#ifndef DS10_SONG_H
#define DS10_SONG_H

#include <stddef.h>
#include <stdint.h>

#define DS10_SONG_PATTERNS 16
#define DS10_SONG_CHAIN 100   /* bars; 0xFFFF ends the arrangement */
#define DS10_SONG_TRACKS 6    /* 2 synth + 4 drum */
#define DS10_SONG_STEPS 16
#define DS10_SONG_LANES 2     /* automation lanes, synth tracks only */

#define DS10_SONG_PATTERN_SIZE 676
#define DS10_SONG_PATTERN_OFF 0xEC
#define DS10_SONG_TAIL_OFF 0x2B2C
#define DS10_SONG_TAIL_MAX 1200
#define DS10_SONG_NO_PATTERN 0xFFFF

/* One step of one track. Verified field for field against the original's
 * accessor at 0x02033F8C, which copies exactly these four signed bytes. */
typedef struct {
    int8_t note;   /* +0x00  negative = no note on this step */
    int8_t gate;   /* +0x01  gate length */
    int8_t b2;     /* +0x02  the low half of the 16-bit per-step value */
    int8_t b3;     /* +0x03 */
} ds10_song_step;

/* One step of the two automation lanes, from 0x020343B4. `raw` is what
 * ds10_lane_map consumes; -1 means "no automation here, use the default". */
typedef struct {
    int8_t raw[DS10_SONG_LANES];
    int8_t knob[DS10_SONG_LANES];
} ds10_song_auto;

#define DS10_SONG_KNOBS 31   /* what the synth machine's own knob count reports */

typedef struct {
    uint32_t steps;         /* +0x00  steps in this pattern; 6..16 in the demos */
    /* +0x04  one 31-knob patch per synth track. 0x0206DFCC pushes exactly this
     * array into the machine on a pattern change, one knob at a time, and the
     * synth reports 31 knobs. Every one of the 544 patches in the ROM's songs
     * falls inside the per-knob range patch.c already knows. */
    int8_t knobs[2][DS10_SONG_KNOBS];
    ds10_song_step step[DS10_SONG_TRACKS][DS10_SONG_STEPS];   /* +0x42 */
    ds10_song_auto lane[DS10_SONG_LANES][DS10_SONG_STEPS];    /* +0x1C2 */
    uint8_t params[0x62];   /* +0x242  drum and mixer settings, not yet named */
} ds10_song_pattern;

typedef struct {
    uint16_t version;       /* +0x00  1 = DS-10, 2 = PLUS, 0/0xFFFF = pre-release */
    uint16_t revision;      /* +0x02 */
    uint32_t tempo;         /* +0x04  BPM */
    uint16_t f08;           /* +0x08  12 from v1 on, 16 before it */
    uint16_t swing;         /* +0x0A  percent; 50 is straight */
    uint8_t reserved[8];    /* +0x0C  zero in every file in the ROM */
    char name[16];          /* +0x14  6 shown, NUL padded */
    uint16_t chain[DS10_SONG_CHAIN];                          /* +0x24 */
    ds10_song_pattern pattern[DS10_SONG_PATTERNS];            /* +0xEC */

    /* 0x2B2C to the end of the payload: three more blocks the serializer copies
     * (160, 48 and 768 bytes) plus the PLUS-only 224. Round-tripped verbatim. */
    uint8_t tail[DS10_SONG_TAIL_MAX];
    uint32_t payload_len;   /* file length minus the 16-byte hash */
} ds10_song;

enum ds10_song_status {
    DS10_SONG_OK = 0,
    DS10_SONG_BAD_SIZE,     /* not one of the four known payload lengths */
    DS10_SONG_BAD_HASH,     /* MD5 header does not match the payload */
};

/* Parse a whole file, hash header included. */
int ds10_song_parse(ds10_song *s, const uint8_t *file, size_t len);

/* Serialise back to a file, hash included. Returns bytes written
 * (s->payload_len + 16); `out` needs that much room. */
size_t ds10_song_write(const ds10_song *s, uint8_t *out);

/* Bars in the arrangement, i.e. chain entries before the first 0xFFFF. */
int ds10_song_bars(const ds10_song *s);

/* The four drum patches, and a second nine-entry parameter set per drum whose
 * meaning is not established. 0x0206B9DC reads them at song record +0x42AF4 and
 * +0x42B70, which are these payload offsets, and pushes the 31 into a machine one
 * knob at a time - the same 31-knob space as a synth patch. */
#define DS10_SONG_DRUMS 4
#define DS10_SONG_DRUM_OFF (0x2B2C - DS10_SONG_TAIL_OFF)
#define DS10_SONG_DRUM_EXTRA_OFF (0x2BA8 - DS10_SONG_TAIL_OFF)
#define DS10_SONG_DRUM_EXTRA 9

const int8_t *ds10_song_drum_knobs(const ds10_song *s, int drum);
const int8_t *ds10_song_drum_extra(const ds10_song *s, int drum);

#endif /* DS10_SONG_H */
