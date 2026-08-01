/* ds10view - render a page of the interface to a PPM, with nothing but the
 * reimplementation.
 *
 *     out/ds10view 61_SYN_EDIT 90_MAP out/page
 *
 * writes out/page_bot.ppm and out/page_top.ppm.
 *
 * An optional fourth argument gives the sequencer state to draw over the
 * page, as `cursor/step:note,...` with the notes in MIDI numbers:
 *
 *     out/ds10view 60_SYN_SEQ_PIANO 90_MAP out/page 0/0:60,2:60,3:64
 *
 * That is what lets `make test-page` check the grid: the page itself no
 * longer carries the captured pattern's markers, so reproducing the frame
 * means drawing them from this.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

#define FB (DS10_SCR_W * DS10_SCR_H)

static uint32_t top[FB], bottom[FB];

static int write_ppm(const char *path, const uint32_t *fb)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return -1;
    }
    fprintf(f, "P6\n%d %d\n255\n", DS10_SCR_W, DS10_SCR_H);
    for (i = 0; i < FB; i++) {
        fputc((fb[i] >> 16) & 0xFF, f);
        fputc((fb[i] >> 8) & 0xFF, f);
        fputc(fb[i] & 0xFF, f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *bot = argc > 1 ? argv[1] : "61_SYN_EDIT";
    const char *tp = argc > 2 ? argv[2] : "90_MAP";
    const char *stem = argc > 3 ? argv[3] : "out/page";
    const char *root = getenv("DS10_ASSETS");
    char path[512];
    ds10_ui *u;

    u = ds10_ui_new(root ? root : "data/nitrofs");
    if (!u) return 2;
    if (ds10_ui_page(u, bot, tp) < 0) {
        fprintf(stderr, "%s\n", ds10_ui_error(u));
        return 2;
    }
    if (argc > 4) {
        /* "cursor/a:b,a:b,...". On the piano roll a pair is step:note; on the
         * drum grid it is row:step, and a row may appear more than once. */
        int note[16], i, cursor = -1;
        uint8_t on[4 * 16];
        int drums = strcmp(bot, "51_ALL_DRUMS") == 0;
        const char *p = argv[4];
        int mode;

        /* The page names the sub-mode, so the pairs mean step:row on a value
         * page and step:note on the piano roll. */
        for (mode = ds10_ui_seq_modes() - 1; mode > 0; mode--)
            if (strcmp(bot, ds10_ui_seq_mode_page(mode)) == 0) break;

        for (i = 0; i < 16; i++) note[i] = -1;
        memset(on, 0, sizeof on);
        if (strchr(p, '/')) {
            cursor = atoi(p);
            p = strchr(p, '/') + 1;
        }
        while (*p) {
            const char *colon = strchr(p, ':');
            int a, b;

            if (!colon) break;
            a = atoi(p);
            b = atoi(colon + 1);
            if (drums) {
                if (a >= 0 && a < 4 && b >= 0 && b < 16) on[a * 16 + b] = 1;
            } else if (a >= 0 && a < 16) {
                note[a] = b;
            }
            p = strchr(colon, ',');
            if (!p) break;
            p++;
        }
        if (drums) {
            ds10_ui_drum_grid(u, bot, on, 4, 16, cursor);
        } else if (mode > 0) {
            /* A value page: the pairs were step:row, so turn each row back
             * into the value that row stands for. */
            int value[16];

            for (i = 0; i < 16; i++)
                value[i] = note[i] < 0 ? -1
                                       : ds10_ui_seq_row_value(mode, note[i]);
            ds10_ui_seq_values(u, value, 16, mode, cursor);
        } else {
            ds10_ui_seq_grid(u, bot, note, 16, ds10_ui_seq_top_default(),
                             cursor);
        }
    }
    ds10_ui_frame(u, top, bottom);

    snprintf(path, sizeof path, "%s_bot.ppm", stem);
    if (write_ppm(path, bottom) < 0) return 2;
    snprintf(path, sizeof path, "%s_top.ppm", stem);
    if (write_ppm(path, top) < 0) return 2;
    printf("%s: %s (bottom), %s (top)\n", stem, bot, tp);
    ds10_ui_free(u);
    return 0;
}
