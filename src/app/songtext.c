/* songtext - a song file as text and back, on the command line.
 *
 *   out/songtext dump [--clean] song.ds10song [out.ds10song.txt]
 *   out/songtext load song.ds10song.txt [out.ds10song]
 *   out/songtext check song.ds10song...
 *
 * `check` is the round trip: dump, read back, compare with the original byte for
 * byte, in both exact and clean mode. The format is docs/songtext.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/md5.h"
#include "../extras/songtext.h"

#define MAXFILE (16 + 12252)
#define MAXTEXT (1 << 20)

static uint8_t file[MAXFILE], back[MAXFILE];
static char text[MAXTEXT];

static size_t slurp(const char *path, void *buf, size_t cap)
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

static void spit(const char *path, const void *buf, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fwrite(buf, 1, n, f);
    fclose(f);
}

static const char *base(const char *path)
{
    const char *s = strrchr(path, '/');

    return s ? s + 1 : path;
}

/* Is this payload offset the gate, velocity or pan of a step with no note? Those
 * three bytes are what clean mode is allowed to lose, and nothing else is. */
static int dead_byte(const uint8_t *p, size_t len, size_t off)
{
    int i;

    for (i = 0; i < 16; i++) {
        size_t base = 0xEC + (size_t)i * 676, rel, step;

        if (base + 676 > len) break;
        if (off < base + 0x042 || off >= base + 0x1C2) continue;
        rel = off - base - 0x042;
        if ((rel & 3) == 1) return 0;          /* the note byte itself */
        step = rel & ~(size_t)3;
        return p[base + 0x042 + step + 1] == 0xFF;
    }
    return 0;
}

/* A song file, checked the way the hardware checks it. */
static size_t read_song(const char *path)
{
    size_t n = slurp(path, file, sizeof file);
    uint8_t sum[16];

    if (n < 16) {
        fprintf(stderr, "%s: %lu bytes is not a song file\n", path,
                (unsigned long)n);
        exit(1);
    }
    ds10_md5(file + 16, n - 16, sum);
    if (memcmp(sum, file, 16)) {
        fprintf(stderr, "%s: MD5 header does not match\n", path);
        exit(1);
    }
    return n - 16;
}

static size_t dump(const char *path, int clean)
{
    size_t len = read_song(path);
    size_t n = ds10_songtext_write(file + 16, len, base(path), clean, text,
                                   sizeof text);

    if (n >= sizeof text) {
        fprintf(stderr, "%s: text needs %lu bytes\n", path, (unsigned long)n);
        exit(1);
    }
    return n;
}

static int cmd_dump(int argc, char **argv)
{
    int clean = 0, i, n = 0;
    const char *arg[2] = {NULL, NULL};

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--clean")) clean = 1;
        else if (n < 2) arg[n++] = argv[i];
    }
    if (!n) {
        fprintf(stderr, "usage: songtext dump [--clean] <song> [out.txt]\n");
        return 1;
    }
    dump(arg[0], clean);
    if (arg[1]) spit(arg[1], text, strlen(text));
    else fputs(text, stdout);
    return 0;
}

static int cmd_load(int argc, char **argv)
{
    size_t len = 0, n;
    int line = 0, status;
    char out[512];

    if (!argc) {
        fprintf(stderr, "usage: songtext load <song.txt> [out]\n");
        return 1;
    }
    n = slurp(argv[0], text, sizeof text - 1);
    text[n] = 0;
    status = ds10_songtext_read(text, file + 16, sizeof file - 16, &len, &line);
    if (status != DS10_SONGTEXT_OK) {
        fprintf(stderr, "%s: %s\n", argv[0],
                status == DS10_SONGTEXT_SIZE ? "no usable `payload` line"
                : "cannot parse");
        if (line) fprintf(stderr, "  line %d\n", line);
        return 1;
    }
    ds10_md5(file + 16, len, file);
    if (argc > 1) {
        snprintf(out, sizeof out, "%s", argv[1]);
    } else {
        const char *dot = strrchr(argv[0], '.');

        snprintf(out, sizeof out, "%.*s",
                 (int)(dot ? dot - argv[0] : (long)strlen(argv[0])), argv[0]);
    }
    spit(out, file, 16 + len);
    printf("%s: %lu bytes\n", out, (unsigned long)(16 + len));
    return 0;
}

static int cmd_check(int argc, char **argv)
{
    int i, clean, bad = 0;

    for (i = 0; i < argc; i++) {
        size_t len = read_song(argv[i]);

        memcpy(back, file, 16 + len);
        for (clean = 0; clean < 2; clean++) {
            size_t got = 0, j, diff = 0, first = 0;
            int status;

            ds10_songtext_write(back + 16, len, NULL, clean, text, sizeof text);
            status = ds10_songtext_read(text, file + 16, sizeof file - 16, &got,
                                       NULL);
            if (status != DS10_SONGTEXT_OK || got != len) {
                printf("FAIL %-24s %s: cannot read back (%d)\n",
                       base(argv[i]), clean ? "clean" : "exact", status);
                bad++;
                continue;
            }
            for (j = 0; j < len; j++)
                if (file[16 + j] != back[16 + j]) {
                    if (!diff) first = j;
                    diff++;
                    /* Clean mode may only lose the gate, velocity and pan of a
                     * step with no note. Anything else is a bug. */
                    if (clean && !dead_byte(back + 16, len, j)) {
                        printf("FAIL %-24s clean: live byte %#lx changed\n",
                               base(argv[i]), (unsigned long)j);
                        bad++;
                        break;
                    }
                }
            if (diff && !clean) {
                printf("FAIL %-24s exact: %lu bytes differ, first at %#lx\n",
                       base(argv[i]), (unsigned long)diff,
                       (unsigned long)first);
                bad++;
            }
        }
        memcpy(file, back, 16 + len);
    }
    printf("%d of %d songs round-tripped\n", argc - bad, argc);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: songtext dump|load|check ...\n");
        return 1;
    }
    if (!strcmp(argv[1], "dump")) return cmd_dump(argc - 2, argv + 2);
    if (!strcmp(argv[1], "load")) return cmd_load(argc - 2, argv + 2);
    if (!strcmp(argv[1], "check")) return cmd_check(argc - 2, argv + 2);
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 1;
}
