/* songtext - see songtext.h, and docs/songtext.md for the format.
 *
 * The encoder is exact by construction rather than by my having named every
 * field: it writes the lines it knows, parses its own output back, and appends a
 * `raw <offset> <hex>` line for every byte that did not survive. So a field
 * nobody has named still round-trips, and that is the same trick
 * tools/songtext.py uses - the two have to produce identical text.
 */
#include "songtext.h"

#include "../core/md5.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ layout */

#define PAT_OFF 0xEC
#define PAT_SIZE 676
#define NO_NOTE 0xFF
#define NO_PATTERN 0xFFFF

#define P_STEPS 0x000
#define P_KNOBS 0x004
#define P_STEP 0x042
#define P_LANE 0x1C2
#define P_MIX 0x242
#define P_PARAMS 0x25B

#define T_DRUM_KNOBS 0x2B2C
#define T_DRUM_EXTRA 0x2BA8
#define T_NAME_NOW 0x2BCC
#define T_NAME_PAT 0x2BFC
#define NAME_SIZE 8

#define NKNOBS 31
#define NMIX 25
#define NDRUMS 4
#define DRUM_EXTRA_N 9
#define NTRACKS 6
#define NSTEPS 16
#define NLANES 2
#define NPATTERNS 16
#define NCHAIN 100

#define GATE_TIE 5
#define VEL_DEFAULT 127
#define PAN_DEFAULT 63
#define DRUM_ROOT 60

/* What ds10init.dat leaves on a step with no note. */
static const uint8_t REST_FILLER[3] = {3, 127, 64};

/* 25% on a synth track, retrig on a drum track. */
static const uint8_t GATE_DEFAULT[2] = {1, 6};

static const size_t PAYLOADS[4] = {11212, 11260, 12028, 12252};

/* ------------------------------------------------------------------- tables */

struct enumdef {
    const char *const *name;
    int n;
};

static const char *const WAVES[] = {"tri", "saw", "square", "noise"};
static const char *const FILTERS[] = {"lp", "bp", "hp"};
static const char *const ONOFF[] = {"off", "on"};
static const char *const SOURCES[] = {"none", "sine", "saw", "square", "sh",
                                      "eg", "vco2"};
static const struct enumdef E_WAVE = {WAVES, 4};
static const struct enumdef E_FILTER = {FILTERS, 3};
static const struct enumdef E_ONOFF = {ONOFF, 2};
static const struct enumdef E_SOURCE = {SOURCES, 7};

static const char *const KNOB_NAME[NKNOBS] = {
    "octave", "pitcheg", "porta", "pitchint", "vco1int", "vco2int", "wave1",
    "wave2", "vco2pitch", "balance", "sync", "cutoff", "peak", "filter",
    "cutoffeg", "cutoffint", "vcaeg", "vcaint", "drive", "level", "attack",
    "decay", "sustain", "release", "mgfreq", "mgsync", "pitchsrc", "vco1src",
    "vco2src", "cutoffsrc", "vcasrc",
};

/* knob_min < 0 in the synth's own descriptors. */
static const uint8_t KNOB_SIGNED[NKNOBS] = {
    1, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
};

static const struct enumdef *const KNOB_ENUM[NKNOBS] = {
    NULL, NULL, NULL, NULL, NULL, NULL, &E_WAVE, &E_WAVE, NULL, NULL, &E_ONOFF,
    NULL, NULL, &E_FILTER, NULL, NULL, &E_ONOFF, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, &E_ONOFF, &E_SOURCE, &E_SOURCE, &E_SOURCE, &E_SOURCE,
    &E_SOURCE,
};

/* A modulation slot: destination, its intensity knob, its source knob. */
struct moddef {
    const char *dest;
    int kint;
    int ksrc;
};

static const struct moddef MODS[5] = {
    {"pitch", 3, 26}, {"vco1p", 4, 27}, {"vco2p", 5, 28},
    {"cutoff", 15, 29}, {"vca", 17, 30},
};

/* The ROM's INIT patch, ds10synth00.dat bytes 0x20..0x3E. */
static const uint8_t INIT_PATCH[NKNOBS] = {
    0, 0, 0, 0, 0, 0, 1, 1, 3, 0, 0, 127, 0, 0, 0, 0, 0, 0, 0, 105, 0, 66, 127,
    0, 19, 0, 0, 0, 0, 0, 0,
};

/* ds10init.dat pattern 0 at +0x242. */
static const uint8_t INIT_MIX[NMIX] = {
    0, 0, 62, 0, 0, 0, 1, 0, 0, 65, 62, 61, 63, 65, 73, 0, 0, 127, 127, 127,
    127, 127, 127, 0, 0,
};

static const char *const ROUTES[] = {"syn1", "syn2", "syn1+syn2", "drums",
                                     "all", "off"};
static const char *const FX_TYPES[] = {"delay", "chorus", "flanger"};

struct fxdef {
    int knob;
    const char *name;
    int is_signed;
};

static const struct fxdef FX[9] = {
    {2, "time", 0}, {3, "ratio", 1}, {4, "rate", 0}, {5, "depth", 1},
    {6, "sync", 0}, {7, "fb", 1}, {8, "wet", 0}, {15, "k15", 0},
    {16, "k16", 0},
};

#define MIX_PAN 9
#define MIX_VOL 17

struct gatedef {
    int code;
    const char *name;
};

static const struct gatedef GATES[8] = {
    {0xFF, "hold"}, {6, "retrig"}, {5, "tie"}, {4, "100%"}, {3, "75%"},
    {2, "50%"}, {1, "25%"}, {0, "off"},
};

static const char *const NOTES[12] = {"C", "C#", "D", "D#", "E", "F", "F#",
                                      "G", "G#", "A", "A#", "B"};

static const char *const TRACK_NAME[NTRACKS] = {"syn1", "syn2", "drum1",
                                                "drum2", "drum3", "drum4"};

static const char *const GRID_KIND[4] = {"note", "gate", "vel", "pan"};

/* --------------------------------------------------------------- small bits */

static int track_by_name(const char *s)
{
    int i;

    for (i = 0; i < NTRACKS; i++)
        if (!strcmp(s, TRACK_NAME[i])) return i;
    return -1;
}

static int knob_by_name(const char *s)
{
    int i;

    for (i = 0; i < NKNOBS; i++)
        if (!strcmp(s, KNOB_NAME[i])) return i;
    return -1;
}

static const char *gate_name(int code)
{
    size_t i;

    for (i = 0; i < sizeof GATES / sizeof *GATES; i++)
        if (GATES[i].code == code) return GATES[i].name;
    return NULL;
}

static int gate_value(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof GATES / sizeof *GATES; i++)
        if (!strcmp(name, GATES[i].name)) return GATES[i].code;
    return -1;
}

static int mod_by_int(int knob)
{
    int i;

    for (i = 0; i < 5; i++)
        if (MODS[i].kint == knob) return i;
    return -1;
}

static int mod_by_src(int knob)
{
    int i;

    for (i = 0; i < 5; i++)
        if (MODS[i].ksrc == knob) return i;
    return -1;
}

static int is_mod_src(int knob)
{
    return mod_by_src(knob) >= 0;
}

static int isdigit_(int c)
{
    return c >= '0' && c <= '9';
}

static void note_name(int v, char *out)
{
    sprintf(out, "%s%d", NOTES[v % 12], v / 12 - 1);
}

static int note_value(const char *s)
{
    int i, best = -1, blen = 0;
    size_t n;

    for (i = 0; i < 12; i++) {
        n = strlen(NOTES[i]);
        if (strncmp(s, NOTES[i], n)) continue;
        if ((int)n > blen) { best = i; blen = (int)n; }
    }
    if (best < 0) return -1;
    if (!s[blen] || (!isdigit_(s[blen]) && s[blen] != '-')) return -1;
    return (atoi(s + blen) + 1) * 12 + best;
}

/* ------------------------------------------------------------------ the sink */

struct sink {
    char *buf;
    size_t cap, len;
};

static void emit(struct sink *k, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    if (k->len < k->cap)
        n = vsnprintf(k->buf + k->len, k->cap - k->len, fmt, ap);
    else
        n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) k->len += (size_t)n;
}

/* --------------------------------------------------------------------- names */

/* A name field as a quoted string. Six raw bytes, not a C string: the game pads
 * with 0x08 as often as with NUL, so only trailing NULs come off. */
static void quote(const uint8_t *raw, size_t n, char *out)
{
    size_t i;
    char *p = out;

    while (n && raw[n - 1] == 0) n--;
    *p++ = '"';
    for (i = 0; i < n; i++) {
        uint8_t c = raw[i];

        if (c == '"' || c == '\\') { *p++ = '\\'; *p++ = (char)c; }
        else if (c >= 0x20 && c < 0x7F) *p++ = (char)c;
        else if (c == 0) { *p++ = '\\'; *p++ = '0'; }
        else if (c == 8) { *p++ = '\\'; *p++ = 'b'; }
        else { p += sprintf(p, "\\x%02x", c); }
    }
    *p++ = '"';
    *p = 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The quoted string starting at `s` (which points at the opening quote) into
 * `out`, at most `cap` bytes. Returns how many bytes it decoded. */
static size_t unquote(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;

    if (*s != '"') return 0;
    s++;
    while (*s && *s != '"' && n < cap) {
        if (*s == '\\') {
            s++;
            if (*s == '0') out[n++] = 0;
            else if (*s == 'b') out[n++] = 8;
            else if (*s == 'x' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
                out[n++] = (uint8_t)(hexval(s[1]) * 16 + hexval(s[2]));
                s += 2;
            } else out[n++] = (uint8_t)*s;
            s++;
        } else {
            out[n++] = (uint8_t)*s++;
        }
    }
    return n;
}

/* --------------------------------------------------------------- knob tokens */

static void knob_text(int i, int v, char *out)
{
    const struct enumdef *e = KNOB_ENUM[i];

    if (e && v >= 0 && v < e->n) {
        sprintf(out, "%s=%s", KNOB_NAME[i], e->name[v]);
        return;
    }
    if (KNOB_SIGNED[i] && v > 127) v -= 256;
    sprintf(out, "%s=%d", KNOB_NAME[i], v);
}

static int knob_value(int i, const char *s)
{
    const struct enumdef *e = KNOB_ENUM[i];
    int k;

    if (e)
        for (k = 0; k < e->n; k++)
            if (!strcmp(s, e->name[k])) return k;
    return atoi(s) & 0xFF;
}

/* The `patch` line's tokens, and the `mod` group. A modulation slot that has no
 * source is not written at all; one that has takes its intensity with it. */
static void patch_text(const uint8_t *knobs, char *body, char *mods)
{
    int i;
    char tok[48];

    *body = *mods = 0;
    for (i = 0; i < NKNOBS; i++) {
        int m = mod_by_int(i);

        if (is_mod_src(i)) continue;
        if (m >= 0 && knobs[MODS[m].ksrc] != 0) continue;
        if (knobs[i] == INIT_PATCH[i]) continue;
        knob_text(i, knobs[i], tok);
        if (*body) strcat(body, " ");
        strcat(body, tok);
    }
    for (i = 0; i < 5; i++) {
        int src = knobs[MODS[i].ksrc], amount = knobs[MODS[i].kint];

        if (src == 0) continue;
        if (src < (int)(sizeof SOURCES / sizeof *SOURCES))
            sprintf(tok, "%s=%s", MODS[i].dest, SOURCES[src]);
        else
            sprintf(tok, "%s=%d", MODS[i].dest, src);
        if (amount != 0)
            sprintf(tok + strlen(tok), ":%+d",
                    amount > 127 ? amount - 256 : amount);
        if (*mods) strcat(mods, " ");
        strcat(mods, tok);
    }
}

static void parse_patch(char **tok, int n, uint8_t *knobs)
{
    int i, k;
    char *eq;

    for (i = 0; i < n; i++) {
        eq = strchr(tok[i], '=');
        if (!eq) continue;
        *eq = 0;
        k = knob_by_name(tok[i]);
        if (k >= 0) knobs[k] = (uint8_t)knob_value(k, eq + 1);
        *eq = '=';
    }
}

static void parse_mods(char **tok, int n, uint8_t *knobs)
{
    int i, j, src;
    char *eq, *colon;

    for (i = 0; i < n; i++) {
        eq = strchr(tok[i], '=');
        if (!eq) continue;
        *eq = 0;
        for (j = 0; j < 5; j++)
            if (!strcmp(tok[i], MODS[j].dest)) break;
        *eq = '=';
        if (j == 5) continue;
        colon = strchr(eq + 1, ':');
        if (colon) *colon = 0;
        for (src = 0; src < (int)(sizeof SOURCES / sizeof *SOURCES); src++)
            if (!strcmp(eq + 1, SOURCES[src])) break;
        knobs[MODS[j].ksrc] =
            (uint8_t)(src < (int)(sizeof SOURCES / sizeof *SOURCES)
                      ? src : atoi(eq + 1));
        knobs[MODS[j].kint] = (uint8_t)(colon ? atoi(colon + 1) & 0xFF : 0);
        if (colon) *colon = ':';
    }
}

/* ------------------------------------------------------------ the step rows */

/* One track's four rows. `cell[kind][step]`, and `print[kind]` says which rows
 * carry anything. */
struct grid {
    char cell[4][NSTEPS][12];
    int print[4];
};

static void grid_build(const uint8_t *pat, int track, struct grid *g)
{
    int drum = track >= 2, s, k;
    int dflt = GATE_DEFAULT[drum ? 1 : 0];

    memset(g, 0, sizeof *g);
    for (s = 0; s < NSTEPS; s++) {
        const uint8_t *r = pat + P_STEP + track * 64 + s * 4;
        int gate = r[0], note = r[1], vel = r[2], pan = r[3];
        const char *gn;

        if (note == NO_NOTE) {
            for (k = 0; k < 4; k++) strcpy(g->cell[k][s], ".");
            continue;
        }
        if (s && note == r[-3] && r[-4] == GATE_TIE) {
            /* The note carries on, so the step before it has to be a tie, and
             * saying so twice would be noise: `-` is the tie. */
            strcpy(g->cell[0][s], "-");
            strcpy(g->cell[1][s - 1], ".");
        } else if (drum && note == DRUM_ROOT) {
            strcpy(g->cell[0][s], "x");
        } else {
            note_name(note, g->cell[0][s]);
        }
        gn = gate_name(gate);
        if (gate == dflt) strcpy(g->cell[1][s], ".");
        else if (gn) strcpy(g->cell[1][s], gn);
        else sprintf(g->cell[1][s], "%d", gate);
        if (vel == VEL_DEFAULT) strcpy(g->cell[2][s], ".");
        else sprintf(g->cell[2][s], "%d", vel);
        if (pan == PAN_DEFAULT) strcpy(g->cell[3][s], ".");
        else sprintf(g->cell[3][s], "%d", pan);
    }
    for (s = 0; s < NSTEPS; s++)
        if (strcmp(g->cell[0][s], ".")) g->print[0] = 1;
    if (!g->print[0]) return;
    for (k = 1; k < 4; k++)
        for (s = 0; s < NSTEPS; s++)
            if (strcmp(g->cell[k][s], ".") && strcmp(g->cell[k][s], "-"))
                g->print[k] = 1;
}

/* Bar marks every four steps, one column width for the whole pattern, and no
 * trailing blanks. */
static void grid_row(struct sink *k, const char *label, const char *kind,
                     char cell[NSTEPS][12], int w)
{
    char line[NSTEPS * 16], *p = line;
    int s;

    for (s = 0; s < NSTEPS; s++) {
        if (s && s % 4 == 0) p += sprintf(p, "| ");
        p += sprintf(p, "%-*s ", w, cell[s]);
    }
    while (p > line && p[-1] == ' ') p--;
    *p = 0;
    emit(k, "%-5s %-4s %s\n", label, kind, line);
}

static void parse_grid(int kind, char **tok, int n, uint8_t *pat, int track)
{
    int drum = track >= 2, s = 0, i, g;

    for (i = 0; i < n && s < NSTEPS; i++) {
        uint8_t *r = pat + P_STEP + track * 64 + s * 4;
        const char *t = tok[i];

        if (!strcmp(t, "|")) continue;
        if (kind == 0) {
            if (!strcmp(t, ".") || (!strcmp(t, "-") && s == 0)) {
                r[0] = REST_FILLER[0];
                r[1] = NO_NOTE;
                r[2] = REST_FILLER[1];
                r[3] = REST_FILLER[2];
            } else if (!strcmp(t, "-")) {
                /* Carry the note on, and tie the step before it so it does. */
                r[-4] = GATE_TIE;
                r[0] = GATE_DEFAULT[drum ? 1 : 0];
                r[1] = r[-3];
                r[2] = VEL_DEFAULT;
                r[3] = PAN_DEFAULT;
            } else {
                int v = !strcmp(t, "x") ? DRUM_ROOT : note_value(t);

                if (v < 0) return;
                r[0] = GATE_DEFAULT[drum ? 1 : 0];
                r[1] = (uint8_t)v;
                r[2] = VEL_DEFAULT;
                r[3] = PAN_DEFAULT;
            }
        } else if (strcmp(t, ".") && strcmp(t, "-")) {
            if (kind == 1) {
                g = gate_value(t);
                r[0] = (uint8_t)(g >= 0 ? g : atoi(t) & 0xFF);
            } else if (kind == 2) {
                r[2] = (uint8_t)(atoi(t) & 0xFF);
            } else {
                r[3] = (uint8_t)(atoi(t) & 0xFF);
            }
        }
        s++;
    }
}

/* ------------------------------------------------------------------- lanes */

/* One automation lane: its knob, then only the steps that have a value. -1 is
 * "no automation here", which is three quarters of them. */
static int lane_text(const uint8_t *pat, int track, int lane, char *out)
{
    const uint8_t *base = pat + P_LANE + track * 64;
    int s, any = 0, knob = base[2 + lane];
    char tok[32];

    for (s = 0; s < NSTEPS; s++)
        if (base[s * 4 + lane] != NO_NOTE) any = 1;
    if (!any && knob == 11 + lane) return 0;
    if (knob < NKNOBS)
        sprintf(out, "%s lane%d knob=%s", TRACK_NAME[track], lane + 1,
                KNOB_NAME[knob]);
    else
        sprintf(out, "%s lane%d knob=%d", TRACK_NAME[track], lane + 1, knob);
    for (s = 0; s < NSTEPS; s++) {
        if (base[s * 4 + lane] != NO_NOTE) {
            sprintf(tok, " %d=%d", s, base[s * 4 + lane]);
            strcat(out, tok);
        }
        if (base[s * 4 + 2 + lane] != knob) {
            int k = base[s * 4 + 2 + lane];

            if (k < NKNOBS) sprintf(tok, " %d/%s", s, KNOB_NAME[k]);
            else sprintf(tok, " %d/%d", s, k);
            strcat(out, tok);
        }
    }
    return 1;
}

static void parse_lane(char **tok, int n, uint8_t *pat, int track, int lane)
{
    uint8_t *base = pat + P_LANE + track * 64;
    int i, s, knob = 11 + lane;
    char *eq, *slash;

    for (i = 0; i < n; i++)
        if (!strncmp(tok[i], "knob=", 5)) {
            int k = knob_by_name(tok[i] + 5);

            knob = k >= 0 ? k : atoi(tok[i] + 5);
        }
    for (s = 0; s < NSTEPS; s++) base[s * 4 + 2 + lane] = (uint8_t)knob;
    for (i = 0; i < n; i++) {
        if ((slash = strchr(tok[i], '/')) != NULL && isdigit_(tok[i][0])) {
            int k;

            *slash = 0;
            s = atoi(tok[i]);
            k = knob_by_name(slash + 1);
            if (s >= 0 && s < NSTEPS)
                base[s * 4 + 2 + lane] = (uint8_t)(k >= 0 ? k
                                                   : atoi(slash + 1));
            *slash = '/';
        } else if ((eq = strchr(tok[i], '=')) != NULL && isdigit_(tok[i][0])) {
            *eq = 0;
            s = atoi(tok[i]);
            if (s >= 0 && s < NSTEPS)
                base[s * 4 + lane] = (uint8_t)(atoi(eq + 1) & 0xFF);
            *eq = '=';
        }
    }
}

/* ------------------------------------------------------------- mixer and fx */

static void mix_text(const uint8_t *mix, char *mixline, char *fxline)
{
    int i, ch;
    char tok[64];

    *mixline = *fxline = 0;
    if (mix[0] != INIT_MIX[0]) {
        if (mix[0] < (int)(sizeof ROUTES / sizeof *ROUTES))
            sprintf(mixline, "route=%s", ROUTES[mix[0]]);
        else
            sprintf(mixline, "route=%d", mix[0]);
    }
    for (i = 0; i < 2; i++) {
        int base = i ? MIX_PAN : MIX_VOL;

        if (!memcmp(mix + base, INIT_MIX + base, 6)) continue;
        sprintf(tok, "%s=", i ? "pan" : "vol");
        for (ch = 0; ch < 6; ch++)
            sprintf(tok + strlen(tok), "%s%d", ch ? "," : "", mix[base + ch]);
        if (*mixline) strcat(mixline, " ");
        strcat(mixline, tok);
    }
    {
        char params[256] = "";

        for (i = 0; i < (int)(sizeof FX / sizeof *FX); i++) {
            int v = mix[FX[i].knob];

            if (v == INIT_MIX[FX[i].knob]) continue;
            if (FX[i].is_signed && v > 127) v -= 256;
            sprintf(tok, "%s=%d", FX[i].name, v);
            if (*params) strcat(params, " ");
            strcat(params, tok);
        }
        if (*params || mix[1] != INIT_MIX[1]) {
            if (mix[1] < (int)(sizeof FX_TYPES / sizeof *FX_TYPES))
                sprintf(fxline, "%s", FX_TYPES[mix[1]]);
            else
                sprintf(fxline, "%d", mix[1]);
            if (*params) sprintf(fxline + strlen(fxline), " %s", params);
        }
    }
}

static void parse_mix(char **tok, int n, uint8_t *mix)
{
    int i, ch;
    char *eq, *p;

    for (i = 0; i < n; i++) {
        eq = strchr(tok[i], '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(tok[i], "route")) {
            for (ch = 0; ch < (int)(sizeof ROUTES / sizeof *ROUTES); ch++)
                if (!strcmp(eq + 1, ROUTES[ch])) break;
            mix[0] = (uint8_t)(ch < (int)(sizeof ROUTES / sizeof *ROUTES)
                               ? ch : atoi(eq + 1));
        } else if (!strcmp(tok[i], "vol") || !strcmp(tok[i], "pan")) {
            int base = tok[i][0] == 'v' ? MIX_VOL : MIX_PAN;

            p = eq + 1;
            for (ch = 0; ch < 6 && *p; ch++) {
                mix[base + ch] = (uint8_t)(atoi(p) & 0xFF);
                p = strchr(p, ',');
                if (!p) break;
                p++;
            }
        }
        *eq = '=';
    }
}

static void parse_fx(char **tok, int n, uint8_t *mix)
{
    int i, j;
    char *eq;

    if (n < 1) return;
    for (j = 0; j < (int)(sizeof FX_TYPES / sizeof *FX_TYPES); j++)
        if (!strcmp(tok[0], FX_TYPES[j])) break;
    mix[1] = (uint8_t)(j < (int)(sizeof FX_TYPES / sizeof *FX_TYPES)
                       ? j : atoi(tok[0]));
    for (i = 1; i < n; i++) {
        eq = strchr(tok[i], '=');
        if (!eq) continue;
        *eq = 0;
        for (j = 0; j < (int)(sizeof FX / sizeof *FX); j++)
            if (!strcmp(tok[i], FX[j].name))
                mix[FX[j].knob] = (uint8_t)(atoi(eq + 1) & 0xFF);
        *eq = '=';
    }
}

/* ---------------------------------------------------------------- the blank */

/* The skeleton a text is read into: everything this format calls a default.
 * Whatever a song does differently is then a named line or a `raw` one. */
static void blank(uint8_t *p, size_t len)
{
    int i, t, s;

    memset(p, 0, len);
    p[0x08] = 12;
    p[0x0A] = 50;
    for (i = 0; i < NCHAIN; i++) {
        p[0x24 + i * 2] = 0xFF;
        p[0x25 + i * 2] = 0xFF;
    }
    for (i = 0; i < NPATTERNS; i++) {
        uint8_t *pat = p + PAT_OFF + i * PAT_SIZE;

        if ((size_t)(PAT_OFF + (i + 1) * PAT_SIZE) > len) break;
        pat[P_STEPS] = NSTEPS;
        for (t = 0; t < 2; t++)
            memcpy(pat + P_KNOBS + t * NKNOBS, INIT_PATCH, NKNOBS);
        for (t = 0; t < NTRACKS; t++)
            for (s = 0; s < NSTEPS; s++) {
                uint8_t *r = pat + P_STEP + t * 64 + s * 4;

                r[0] = REST_FILLER[0];
                r[1] = NO_NOTE;
                r[2] = REST_FILLER[1];
                r[3] = REST_FILLER[2];
            }
        for (t = 0; t < NLANES; t++)
            for (s = 0; s < NSTEPS; s++) {
                uint8_t *r = pat + P_LANE + t * 64 + s * 4;

                r[0] = NO_NOTE;
                r[1] = NO_NOTE;
                r[2] = 11;
                r[3] = 12;
            }
        memcpy(pat + P_MIX, INIT_MIX, NMIX);
    }
    if (len >= T_DRUM_KNOBS + NDRUMS * NKNOBS)
        for (i = 0; i < NDRUMS; i++)
            memcpy(p + T_DRUM_KNOBS + i * NKNOBS, INIT_PATCH, NKNOBS);
}

/* ----------------------------------------------------------------- the read */

/* Strip a comment. `#` starts one only at the start of a line or after
 * whitespace, so `C#2` is a note. */
static void strip_comment(char *s)
{
    char *p;

    for (p = s; *p; p++)
        if (*p == '#' && (p == s || p[-1] == ' ' || p[-1] == '\t')) {
            *p = 0;
            return;
        }
}

#define MAXTOK 160

static int split(char *line, char **tok)
{
    int n = 0;
    char *p = line;

    while (*p && n < MAXTOK) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

static size_t payload_of(const char *text)
{
    const char *p = text;

    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "payload ", 8)) {
            size_t v = (size_t)atol(p + 8), i;

            for (i = 0; i < sizeof PAYLOADS / sizeof *PAYLOADS; i++)
                if (PAYLOADS[i] == v) return v;
            return 0;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

int ds10_songtext_read(const char *text, uint8_t *out, size_t cap, size_t *len,
                       int *errline)
{
    char line[4096], full[4096], *tok[MAXTOK];
    const char *cur = text;
    size_t plen = payload_of(text);
    int lineno = 0, pat = -1, track = -1, n;

    if (errline) *errline = 0;
    if (!plen) return DS10_SONGTEXT_SIZE;
    if (cap < plen) return DS10_SONGTEXT_TRUNC;
    if (len) *len = plen;
    blank(out, plen);

    while (*cur) {
        const char *nl = strchr(cur, '\n');
        size_t nchar = nl ? (size_t)(nl - cur) : strlen(cur);

        lineno++;
        if (nchar >= sizeof line) nchar = sizeof line - 1;
        memcpy(line, cur, nchar);
        line[nchar] = 0;
        cur = nl ? nl + 1 : cur + strlen(cur);
        strip_comment(line);
        /* split() writes NULs into the line, and a quoted name can hold a space
         * - `TMP\0 6` is a real one - so keep the line whole for those. */
        memcpy(full, line, strlen(line) + 1);
        n = split(line, tok);
        if (!n) continue;

        if (tok[0][0] == '[') {
            pat = -1;
            track = -1;
            if (!strncmp(tok[0], "[pattern", 8) && n > 1) pat = atoi(tok[1]);
            continue;
        }
        if (!strcmp(tok[0], "payload")) continue;

        /* [song] */
        if (!strcmp(tok[0], "version")) { out[0] = (uint8_t)atoi(tok[1]);
            out[1] = (uint8_t)(atoi(tok[1]) >> 8); continue; }
        if (!strcmp(tok[0], "revision")) { out[2] = (uint8_t)atoi(tok[1]);
            out[3] = (uint8_t)(atoi(tok[1]) >> 8); continue; }
        if (!strcmp(tok[0], "tempo")) {
            long v = atol(tok[1]);
            int i;

            for (i = 0; i < 4; i++) out[4 + i] = (uint8_t)(v >> (8 * i));
            continue;
        }
        if (!strcmp(tok[0], "f08")) { out[8] = (uint8_t)atoi(tok[1]);
            out[9] = (uint8_t)(atoi(tok[1]) >> 8); continue; }
        if (!strcmp(tok[0], "swing")) { out[0x0A] = (uint8_t)atoi(tok[1]);
            out[0x0B] = (uint8_t)(atoi(tok[1]) >> 8); continue; }
        if (!strcmp(tok[0], "name") && pat < 0) {
            uint8_t nm[16];
            const char *q = strchr(full, '"');
            size_t got = q ? unquote(q, nm, sizeof nm) : 0;

            memset(out + 0x14, 0, 16);
            memcpy(out + 0x14, nm, got);
            continue;
        }
        if (!strcmp(tok[0], "chain")) {
            int i;

            for (i = 1; i < n && i <= NCHAIN; i++) {
                int v = atoi(tok[i]);

                out[0x24 + (i - 1) * 2] = (uint8_t)v;
                out[0x25 + (i - 1) * 2] = (uint8_t)(v >> 8);
            }
            continue;
        }
        if (!strcmp(tok[0], "raw")) {
            size_t off = (size_t)strtoul(tok[1], NULL, 16);
            int i;

            for (i = 2; i < n; i++) {
                int hi = hexval(tok[i][0]), lo = hexval(tok[i][1]);

                if (hi < 0 || lo < 0) continue;
                if (off < plen) out[off] = (uint8_t)(hi * 16 + lo);
                off++;
            }
            continue;
        }
        if (pat < 0 && !strncmp(tok[0], "drum", 4) && n > 1) {
            int d = tok[0][4] - '1';

            if (d < 0 || d >= NDRUMS) return DS10_SONGTEXT_SYNTAX;
            if (plen < T_DRUM_KNOBS + NDRUMS * NKNOBS) continue;
            if (!strcmp(tok[1], "patch"))
                parse_patch(tok + 2, n - 2, out + T_DRUM_KNOBS + d * NKNOBS);
            else if (!strcmp(tok[1], "mod"))
                parse_mods(tok + 2, n - 2, out + T_DRUM_KNOBS + d * NKNOBS);
            else if (!strcmp(tok[1], "extra")) {
                int i;

                for (i = 2; i < n && i - 2 < DRUM_EXTRA_N; i++)
                    out[T_DRUM_EXTRA + d * DRUM_EXTRA_N + i - 2] =
                        (uint8_t)(atoi(tok[i]) & 0xFF);
            }
            continue;
        }
        if (pat < 0 || pat >= NPATTERNS) continue;

        /* [pattern N] */
        {
            uint8_t *p = out + PAT_OFF + pat * PAT_SIZE;
            int kind, t;

            if (!strcmp(tok[0], "steps")) {
                long v = atol(tok[1]);
                int i;

                for (i = 0; i < 4; i++) p[P_STEPS + i] = (uint8_t)(v >> (8 * i));
                continue;
            }
            if (!strcmp(tok[0], "scroll")) {
                int i;

                for (i = 1; i < n && i <= NTRACKS; i++)
                    p[P_PARAMS + 1 + (i - 1) * 12] = (uint8_t)(atoi(tok[i]) & 0xFF);
                continue;
            }
            if (!strcmp(tok[0], "mix")) { parse_mix(tok + 1, n - 1, p + P_MIX);
                continue; }
            if (!strcmp(tok[0], "fx")) { parse_fx(tok + 1, n - 1, p + P_MIX);
                continue; }

            for (kind = 0; kind < 4; kind++)
                if (!strcmp(tok[0], GRID_KIND[kind])) break;
            if (kind < 4) {          /* a continuation row under the last note */
                if (track >= 0) parse_grid(kind, tok + 1, n - 1, p, track);
                continue;
            }
            t = track_by_name(tok[0]);
            if (t < 0 || n < 2) continue;
            for (kind = 0; kind < 4; kind++)
                if (!strcmp(tok[1], GRID_KIND[kind])) break;
            if (kind < 4) {
                parse_grid(kind, tok + 2, n - 2, p, t);
                track = t;
                continue;
            }
            if (!strcmp(tok[1], "patch"))
                parse_patch(tok + 2, n - 2, p + P_KNOBS + t * NKNOBS);
            else if (!strcmp(tok[1], "mod"))
                parse_mods(tok + 2, n - 2, p + P_KNOBS + t * NKNOBS);
            else if (!strcmp(tok[1], "name")) {
                uint8_t nm[8];
                const char *q = strchr(full, '"');
                size_t got = q ? unquote(q, nm, 6) : 0;
                size_t off = T_NAME_PAT + (size_t)pat * NTRACKS * NAME_SIZE
                             + (size_t)t * NAME_SIZE;

                if (off + 6 <= plen) {
                    memset(out + off, 0, 6);
                    memcpy(out + off, nm, got);
                }
                if (pat == 0 && (size_t)(T_NAME_NOW + t * NAME_SIZE + 6) <= plen) {
                    off = T_NAME_NOW + (size_t)t * NAME_SIZE;
                    memset(out + off, 0, 6);
                    memcpy(out + off, nm, got);
                }
            } else if (!strncmp(tok[1], "lane", 4)) {
                parse_lane(tok + 2, n - 2, p, t, tok[1][4] - '1');
            }
        }
    }
    return DS10_SONGTEXT_OK;
}

/* ---------------------------------------------------------------- the write */

/* Is this offset one of the three bytes of a step with no note? The sequencer
 * never reads them and the editor leaves whatever it likes there. */
static int is_dead(const uint8_t *p, size_t len, size_t off)
{
    size_t rel, step;
    int i;

    for (i = 0; i < NPATTERNS; i++) {
        size_t base = PAT_OFF + (size_t)i * PAT_SIZE;

        if (base + PAT_SIZE > len) break;
        if (off < base + P_STEP || off >= base + P_LANE) continue;
        rel = off - base - P_STEP;
        step = rel & ~(size_t)3;
        if ((rel & 3) == 1) return 0;
        return p[base + P_STEP + step + 1] == NO_NOTE;
    }
    return 0;
}

/* The named lines, without the `raw` section. */
static void write_named(struct sink *k, const uint8_t *p, size_t len,
                        const char *source)
{
    int i, t, l, s;
    char body[1024], mods[512], tmp[1024];

    if (source) emit(k, "# %s\n", source);
    emit(k, "[song]\n");
    emit(k, "payload %u\n", (unsigned)len);
    emit(k, "version %u\n", (unsigned)(p[0] | p[1] << 8));
    if (p[2] || p[3]) emit(k, "revision %u\n", (unsigned)(p[2] | p[3] << 8));
    emit(k, "tempo %lu\n", (unsigned long)(p[4] | (unsigned long)p[5] << 8 |
                                           (unsigned long)p[6] << 16 |
                                           (unsigned long)p[7] << 24));
    if ((p[0x0A] | p[0x0B] << 8) != 50)
        emit(k, "swing %u\n", (unsigned)(p[0x0A] | p[0x0B] << 8));
    if ((p[8] | p[9] << 8) != 12)
        emit(k, "f08 %u\n", (unsigned)(p[8] | p[9] << 8));
    {
        size_t nn = 16;

        while (nn && p[0x14 + nn - 1] == 0) nn--;
        if (nn) {
            quote(p + 0x14, nn, tmp);
            emit(k, "name %s\n", tmp);
        }
    }
    {
        int bars = 0;

        while (bars < NCHAIN &&
               (p[0x24 + bars * 2] | p[0x25 + bars * 2] << 8) != NO_PATTERN)
            bars++;
        if (bars) {
            emit(k, "chain");
            for (i = 0; i < bars; i++)
                emit(k, " %u", (unsigned)(p[0x24 + i * 2] |
                                          p[0x25 + i * 2] << 8));
            emit(k, "\n");
        }
    }
    if (len >= T_DRUM_KNOBS + NDRUMS * NKNOBS)
        for (i = 0; i < NDRUMS; i++) {
            const uint8_t *kn = p + T_DRUM_KNOBS + i * NKNOBS;

            patch_text(kn, body, mods);
            if (*body) emit(k, "drum%d patch %s\n", i + 1, body);
            if (*mods) emit(k, "drum%d mod %s\n", i + 1, mods);
            if (len >= T_DRUM_EXTRA + NDRUMS * DRUM_EXTRA_N) {
                const uint8_t *x = p + T_DRUM_EXTRA + i * DRUM_EXTRA_N;
                int any = 0;

                for (s = 0; s < DRUM_EXTRA_N; s++) any |= x[s];
                if (any) {
                    emit(k, "drum%d extra", i + 1);
                    for (s = 0; s < DRUM_EXTRA_N; s++) emit(k, " %u", x[s]);
                    emit(k, "\n");
                }
            }
        }

    for (i = 0; i < NPATTERNS; i++) {
        const uint8_t *pat = p + PAT_OFF + (size_t)i * PAT_SIZE;
        struct grid g[NTRACKS];
        struct sink sub;
        char buf[16384];
        int w = 1, any = 0;
        unsigned steps;

        if ((size_t)(PAT_OFF + (i + 1) * PAT_SIZE) > len) break;
        sub.buf = buf;
        sub.cap = sizeof buf;
        sub.len = 0;
        buf[0] = 0;

        steps = (unsigned)(pat[P_STEPS] | pat[P_STEPS + 1] << 8);
        if (steps != NSTEPS) emit(&sub, "steps %u\n", steps);
        if (len >= T_NAME_PAT + ((size_t)i + 1) * NTRACKS * NAME_SIZE)
            for (t = 0; t < NTRACKS; t++) {
                const uint8_t *nm = p + T_NAME_PAT +
                                    (size_t)i * NTRACKS * NAME_SIZE +
                                    (size_t)t * NAME_SIZE;
                size_t nn = 6;

                while (nn && nm[nn - 1] == 0) nn--;
                if (!nn) continue;
                quote(nm, nn, tmp);
                emit(&sub, "%s name %s\n", TRACK_NAME[t], tmp);
            }
        for (t = 0; t < 2; t++) {
            patch_text(pat + P_KNOBS + t * NKNOBS, body, mods);
            if (*body) emit(&sub, "%s patch %s\n", TRACK_NAME[t], body);
            if (*mods) emit(&sub, "%s mod %s\n", TRACK_NAME[t], mods);
        }
        for (t = 0; t < NTRACKS; t++) {
            int kind;

            grid_build(pat, t, &g[t]);
            for (kind = 0; kind < 4; kind++) {
                if (!g[t].print[kind]) continue;
                for (s = 0; s < NSTEPS; s++) {
                    int cl = (int)strlen(g[t].cell[kind][s]);

                    if (cl > w) w = cl;
                }
            }
        }
        for (t = 0; t < NTRACKS; t++) {
            int kind, first = 1;

            for (kind = 0; kind < 4; kind++) {
                if (!g[t].print[kind]) continue;
                grid_row(&sub, first ? TRACK_NAME[t] : "", GRID_KIND[kind],
                         g[t].cell[kind], w);
                first = 0;
            }
        }
        for (t = 0; t < NLANES; t++)
            for (l = 0; l < NLANES; l++)
                if (lane_text(pat, t, l, tmp)) emit(&sub, "%s\n", tmp);
        if (memcmp(pat + P_MIX, INIT_MIX, NMIX)) {
            char mixline[256], fxline[256];

            mix_text(pat + P_MIX, mixline, fxline);
            if (*mixline) emit(&sub, "mix %s\n", mixline);
            if (*fxline) emit(&sub, "fx %s\n", fxline);
        }
        {
            int scroll[NTRACKS], sany = 0;

            for (t = 0; t < NTRACKS; t++) {
                scroll[t] = pat[P_PARAMS + 1 + t * 12];
                sany |= scroll[t];
            }
            if (sany) {
                emit(&sub, "scroll");
                for (t = 0; t < NTRACKS; t++) emit(&sub, " %d", scroll[t]);
                emit(&sub, "\n");
            }
        }
        any = sub.len > 0;
        if (any) emit(k, "\n[pattern %d]\n%s", i, buf);
    }
}

size_t ds10_songtext_write(const uint8_t *payload, size_t len,
                           const char *source, int clean,
                           char *out, size_t cap)
{
    static uint8_t back[12252];
    struct sink k;
    char *named;
    size_t nlen;
    int status;

    k.buf = out;
    k.cap = cap;
    k.len = 0;
    if (cap) out[0] = 0;
    write_named(&k, payload, len, source);

    /* Parse what we just wrote and say, in `raw` lines, whatever did not
     * survive it. Needs the whole named text, so if the caller's buffer was too
     * small we can only report the length it should have been. */
    if (k.len >= cap) return k.len;
    nlen = k.len;
    named = (char *)malloc(nlen + 1);
    if (!named) return k.len;
    memcpy(named, out, nlen + 1);
    status = ds10_songtext_read(named, back, sizeof back, NULL, NULL);
    free(named);
    if (status != DS10_SONGTEXT_OK) return k.len;

    {
        size_t at = 0;
        int opened = 0;

        while (at < len) {
            size_t j, run, end;

            if (payload[at] == back[at] ||
                (clean && is_dead(payload, len, at))) {
                at++;
                continue;
            }
            run = 0;
            end = at;
            for (j = at; j < len && run <= 8; j++) {
                if (payload[j] != back[j] &&
                    !(clean && is_dead(payload, len, j))) {
                    run = 0;
                    end = j + 1;
                } else {
                    run++;
                }
            }
            if (!opened) {
                emit(&k, "\n[raw]\n");
                emit(&k, "# bytes this format does not name, payload-relative\n");
                opened = 1;
            }
            while (at < end) {
                size_t chunk = end - at > 24 ? 24 : end - at;

                emit(&k, "raw %04x", (unsigned)at);
                for (j = 0; j < chunk; j++) emit(&k, " %02x", payload[at + j]);
                emit(&k, "\n");
                at += chunk;
            }
        }
    }
    return k.len;
}

/* ------------------------------------------------------------ ds10_song ends */

size_t ds10_songtext_dump(const ds10_song *s, const char *source, int clean,
                          char *out, size_t cap)
{
    static uint8_t file[16 + 12252];
    size_t n = ds10_song_write(s, file);

    if (n < 16) return 0;
    return ds10_songtext_write(file + 16, n - 16, source, clean, out, cap);
}

int ds10_songtext_parse(ds10_song *s, const char *text, int *errline)
{
    static uint8_t file[16 + 12252];
    size_t len = 0;
    int status = ds10_songtext_read(text, file + 16, sizeof file - 16, &len,
                                    errline);

    if (status != DS10_SONGTEXT_OK) return status;
    ds10_md5(file + 16, len, file);
    if (ds10_song_parse(s, file, 16 + len) != DS10_SONG_OK) {
        /* The hash is ours to compute, so the only way this fails is a length
         * the song code does not know. */
        return DS10_SONGTEXT_SIZE;
    }
    return DS10_SONGTEXT_OK;
}
