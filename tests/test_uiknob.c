/* Differential test: our knob angle map against the original's, 0x02055b98.
 *
 * The first UI function verified this way rather than by comparing a rendered
 * frame. The vector carries what the original wrote into the widget for each
 * case - the angle, the clamped value, and the four derived fields - and this
 * reproduces them from the inputs alone.
 *
 *     python3 tools/oracle.py ui-vectors
 *     make test-uiknob
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/app/widget.h"

#define MAGIC "KNB1"

static FILE *g;
static int fails;

static int32_t rd32(void)
{
    unsigned char b[4];

    if (fread(b, 1, 4, g) != 4) exit(2);
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

static int16_t rd16(void)
{
    unsigned char b[2];

    if (fread(b, 1, 2, g) != 2) exit(2);
    return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static void bad(const char *what, int lo, int hi, int v, int sweep, int rev,
                long want, long got)
{
    if (fails++ < 12)
        fprintf(stderr, "FAIL: %s for min %d max %d value %d sweep %d%s: "
                        "the original has %ld, we have %ld\n",
                what, lo, hi, v, sweep, rev ? " reversed" : "", want, got);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/uiknob.bin";
    char magic[4];
    int32_t ncases, i;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py ui-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, g) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32();
    for (i = 0; i < ncases; i++) {
        int lo = rd32(), hi = rd32(), v = rd32(), sweep = rd32(), rev = rd32();
        int32_t clamp = rd32();
        int16_t angle = rd16();
        int32_t start, half, span, range, revflag;

        (void)rd16();                       /* padding */
        start = rd32();
        half = rd32();
        span = rd32();
        range = rd32();
        revflag = rd32();
        (void)revflag;

        if (ds10_widget_knob_angle(lo, hi, v, sweep, rev) != angle)
            bad("angle", lo, hi, v, sweep, rev, angle,
                ds10_widget_knob_angle(lo, hi, v, sweep, rev));
        if (ds10_widget_clamp(lo, hi, v) != clamp)
            bad("clamp", lo, hi, v, sweep, rev, clamp,
                ds10_widget_clamp(lo, hi, v));
        if (ds10_widget_knob_half(sweep) != half)
            bad("half sweep", lo, hi, v, sweep, rev, half,
                ds10_widget_knob_half(sweep));
        if (0x10000 - ds10_widget_knob_half(sweep) != start)
            bad("start angle", lo, hi, v, sweep, rev, start,
                0x10000 - ds10_widget_knob_half(sweep));
        if (2 * ds10_widget_knob_half(sweep) != span)
            bad("full sweep", lo, hi, v, sweep, rev, span,
                2 * ds10_widget_knob_half(sweep));
        if (hi - lo != range)
            bad("range", lo, hi, v, sweep, rev, range, hi - lo);
    }
    fclose(g);
    if (fails) {
        fprintf(stderr, "FAIL: %d mismatches over %d cases\n", fails, ncases);
        return 1;
    }
    printf("PASS: %d knob settings x 6 fields, 0 mismatches\n", ncases);
    return 0;
}
