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
#define DS10_SONG_MIX_KNOBS 25

#define DS10_SONG_PATTERN_SIZE 676
#define DS10_SONG_PATTERN_OFF 0xEC
#define DS10_SONG_TAIL_OFF 0x2B2C
#define DS10_SONG_TAIL_MAX 1200
#define DS10_SONG_NO_PATTERN 0xFFFF

/* One step of one track. The four bytes and their address are verified against
 * the original's accessor at 0x02033F8C, but that only copies them - it does not
 * say which is which. The order below is from the data, and it is NOT the order
 * the sequencer's runtime pattern uses:
 *
 *   byte 0 is only ever 0..6 across all 26112 steps in the ROM, which are the
 *   gate codes (4 full, 5 over, 6 retrigger);
 *   byte 1 ranges 24..95 peaking hard at 60, plus 0xFF on 16910 steps, which is
 *   the sequencer's "no note" marker;
 *   byte 2 peaks at 127 and byte 3 at 64 - full velocity, centre pan.
 *
 * The runtime pattern has note first and gate second, so whatever builds it
 * swaps them. Getting this backwards turns every rest into a tie and takes every
 * pitch from the gate byte, which sounds like noise. */
typedef struct {
    int8_t gate;   /* +0x00  4 = full step, 5 = one over, 6 = retrigger */
    int8_t note;   /* +0x01  0xFF = no note on this step */
    int8_t vel;    /* +0x02  scales this channel's mixer volume */
    int8_t pan;    /* +0x03  offsets this channel's mixer pan */
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
    /* +0x242  the mixer's 25 knobs, pushed in on a pattern change by 0x0206EC10.
     * Signed, because that pusher reads them with ldrsb. */
    int8_t mixer[DS10_SONG_MIX_KNOBS];
    /* +0x25B: a zero byte, then six 12-byte per-track editor records at +0x25C.
     * The accessor at 0x0206B0C0 takes a field 0..9 and reads record byte
     * `field`:
     *
     *   0 piano-roll scroll (0..84, see ds10_editor_get_scroll)   1 keyboard
     *   octave   2, 3 a copy of the two lanes' knob ids   4 KAOSS pad scale
     *   5 pad key   6 automation path, 1 = HLD and 2 = SMT   7 pad slot
     *   9 the pattern page's LOCK
     *
     * Byte 6 is the only one playback wants: it is ds10_seq_auto.enable, and
     * `automation()` in seq.c runs a lane on either of its two paths, so both
     * values are live. src/app/songplay.c installs it from here. SMT is much the
     * commoner ask - 2942 of the 3104 pattern-track pairs in data/saves against
     * 162 for HLD, and no pair asks for neither. Bytes 8, 10 and 11 have no caller
     * and are zero in every pattern in data/saves; docs/songtext.md has the
     * measurements. */
    uint8_t params[0x62 - DS10_SONG_MIX_KNOBS];
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

    /* 0x2B2C to the end of the payload. The 160 is the four drum patches and
     * their nine extra bytes each (both read by 0x0206B9DC); the 48 and the 768
     * are name fields, six per row at an 8-byte stride, one row for the running
     * machines and then one row per pattern. The PLUS-only 224 at 0x2EFC is the
     * song page's two per-bar lanes - the mute masks below, and a per-bar
     * transpose nothing here reads yet. Round-tripped verbatim either way. */
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

/* Which tracks a bar mutes, six bits packed five bars to a word: bit 0 is the
 * first synth and bit 5 the last drum. One array per deck, PLUS payloads only.
 *
 * The transport reads both of them every bar (0x020330AC) and hands them to
 * 0x02079D58, which for each channel whose state changed pushes the channel's
 * volume knob - 17 + channel for deck A, 42 + channel for deck B - to 0 when the
 * track is muted and back to the pattern's own value when it is not
 * (0x0206E084, landing in the live knob array at seq +0x60). So a muted track
 * still plays its notes into a channel whose volume is zero.
 *
 * Returns 0 for a bar or deck the payload has no room for. */
#define DS10_SONG_MUTE_DECKS 2
#define DS10_SONG_MUTE_BARS 100
int ds10_song_mute(const ds10_song *s, int deck, int bar);

#endif /* DS10_SONG_H */
