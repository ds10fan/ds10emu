#include "song.h"

#include <string.h>

#include "md5.h"

#define HASH 16

/* The four payload lengths that exist. Anything else is not a song. */
static const uint32_t SIZES[] = {11212, 11260, 12028, 12252};

static int known_size(uint32_t n)
{
    size_t i;

    for (i = 0; i < sizeof SIZES / sizeof *SIZES; i++)
        if (SIZES[i] == n)
            return 1;
    return 0;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void parse_pattern(ds10_song_pattern *q, const uint8_t *p)
{
    int t, s, l;

    q->steps = rd32(p);
    memcpy(q->knobs, p + 0x04, sizeof q->knobs);
    for (t = 0; t < DS10_SONG_TRACKS; t++)
        for (s = 0; s < DS10_SONG_STEPS; s++) {
            const uint8_t *r = p + 0x42 + t * 64 + s * 4;

            q->step[t][s].note = (int8_t)r[0];
            q->step[t][s].gate = (int8_t)r[1];
            q->step[t][s].b2 = (int8_t)r[2];
            q->step[t][s].b3 = (int8_t)r[3];
        }
    for (t = 0; t < DS10_SONG_LANES; t++)
        for (s = 0; s < DS10_SONG_STEPS; s++) {
            const uint8_t *r = p + 0x1C2 + t * 64 + s * 4;

            for (l = 0; l < DS10_SONG_LANES; l++) {
                q->lane[t][s].raw[l] = (int8_t)r[l];
                q->lane[t][s].knob[l] = (int8_t)r[2 + l];
            }
        }
    memcpy(q->params, p + 0x242, sizeof q->params);
}

static void write_pattern(const ds10_song_pattern *q, uint8_t *p)
{
    int t, s, l;

    wr32(p, q->steps);
    memcpy(p + 0x04, q->knobs, sizeof q->knobs);
    for (t = 0; t < DS10_SONG_TRACKS; t++)
        for (s = 0; s < DS10_SONG_STEPS; s++) {
            uint8_t *r = p + 0x42 + t * 64 + s * 4;

            r[0] = (uint8_t)q->step[t][s].note;
            r[1] = (uint8_t)q->step[t][s].gate;
            r[2] = (uint8_t)q->step[t][s].b2;
            r[3] = (uint8_t)q->step[t][s].b3;
        }
    for (t = 0; t < DS10_SONG_LANES; t++)
        for (s = 0; s < DS10_SONG_STEPS; s++) {
            uint8_t *r = p + 0x1C2 + t * 64 + s * 4;

            for (l = 0; l < DS10_SONG_LANES; l++) {
                r[l] = (uint8_t)q->lane[t][s].raw[l];
                r[2 + l] = (uint8_t)q->lane[t][s].knob[l];
            }
        }
    memcpy(p + 0x242, q->params, sizeof q->params);
}

int ds10_song_parse(ds10_song *s, const uint8_t *file, size_t len)
{
    const uint8_t *p = file + HASH;
    uint8_t want[HASH];
    int i;

    if (len < HASH || !known_size((uint32_t)(len - HASH)))
        return DS10_SONG_BAD_SIZE;
    ds10_md5(p, len - HASH, want);
    if (memcmp(want, file, HASH) != 0)
        return DS10_SONG_BAD_HASH;

    memset(s, 0, sizeof *s);
    s->payload_len = (uint32_t)(len - HASH);
    s->version = rd16(p + 0x00);
    s->revision = rd16(p + 0x02);
    s->tempo = rd32(p + 0x04);
    s->f08 = rd16(p + 0x08);
    s->swing = rd16(p + 0x0A);
    memcpy(s->reserved, p + 0x0C, sizeof s->reserved);
    memcpy(s->name, p + 0x14, sizeof s->name);
    for (i = 0; i < DS10_SONG_CHAIN; i++)
        s->chain[i] = rd16(p + 0x24 + 2 * i);
    for (i = 0; i < DS10_SONG_PATTERNS; i++)
        parse_pattern(&s->pattern[i],
                      p + DS10_SONG_PATTERN_OFF + i * DS10_SONG_PATTERN_SIZE);
    memcpy(s->tail, p + DS10_SONG_TAIL_OFF, s->payload_len - DS10_SONG_TAIL_OFF);
    return DS10_SONG_OK;
}

size_t ds10_song_write(const ds10_song *s, uint8_t *out)
{
    uint8_t *p = out + HASH;
    int i;

    memset(p, 0, s->payload_len);
    wr16(p + 0x00, s->version);
    wr16(p + 0x02, s->revision);
    wr32(p + 0x04, s->tempo);
    wr16(p + 0x08, s->f08);
    wr16(p + 0x0A, s->swing);
    memcpy(p + 0x0C, s->reserved, sizeof s->reserved);
    memcpy(p + 0x14, s->name, sizeof s->name);
    for (i = 0; i < DS10_SONG_CHAIN; i++)
        wr16(p + 0x24 + 2 * i, s->chain[i]);
    for (i = 0; i < DS10_SONG_PATTERNS; i++)
        write_pattern(&s->pattern[i],
                      p + DS10_SONG_PATTERN_OFF + i * DS10_SONG_PATTERN_SIZE);
    memcpy(p + DS10_SONG_TAIL_OFF, s->tail,
           s->payload_len - DS10_SONG_TAIL_OFF);
    ds10_md5(p, s->payload_len, out);
    return s->payload_len + HASH;
}

int ds10_song_bars(const ds10_song *s)
{
    int i;

    for (i = 0; i < DS10_SONG_CHAIN; i++)
        if (s->chain[i] == DS10_SONG_NO_PATTERN)
            return i;
    return DS10_SONG_CHAIN;
}
