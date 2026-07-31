/* Differential test: our 2D hardware model against melonDS.
 *
 * Each .snap that `out/ndsref` writes holds one frame's whole 2D state - the
 * nine VRAM banks and their mapping, palette, OAM, both engines' registers,
 * the 3D engine's output plane - and the two framebuffers melonDS composited
 * from it. We load the state, render it ourselves, and compare.
 *
 * Two things are checked separately, because they fail for different reasons:
 *
 *   1. flattening the banks per VRAMCNT, against melonDS's own flat buffers;
 *   2. the composited frame, against melonDS's framebuffers.
 *
 *     make test-2d              # every snapshot under out/ref
 *     ./out/test_2d a.snap ...  # named ones
 *
 * A mismatch dumps ours and theirs as .ppm next to the snapshot, because
 * "3162 pixels differ" is not something you can debug and a picture is.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gfx/nds2d.h"

#define FB (DS10_SCR_W * DS10_SCR_H)

static ds10_gpu gpu;
static uint32_t ours_top[FB], ours_bot[FB];
static uint32_t ref_top[FB], ref_bot[FB];
static uint8_t refflat[8][512 * 1024];

static const struct { const char *name; size_t len; } FLAT[8] = {
    {"ABG", 512 * 1024}, {"BBG", 128 * 1024},
    {"AOBJ", 256 * 1024}, {"BOBJ", 128 * 1024},
    {"ABGExtPal", 32 * 1024}, {"BBGExtPal", 32 * 1024},
    {"AOBJExtPal", 8 * 1024}, {"BOBJExtPal", 8 * 1024},
};

static FILE *g;

static void rd(void *p, size_t n)
{
    if (fread(p, 1, n, g) != n) {
        fprintf(stderr, "short read on snapshot\n");
        exit(2);
    }
}

static uint16_t rd16(void)
{
    unsigned char b[2];

    rd(b, 2);
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t rd32(void)
{
    unsigned char b[4];

    rd(b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static void read_engine(ds10_2d *e)
{
    int i;

    memset(e, 0, sizeof *e);
    e->dispcnt = rd32();
    for (i = 0; i < 4; i++) e->bgcnt[i] = rd16();
    for (i = 0; i < 4; i++) e->bgx[i] = rd16();
    for (i = 0; i < 4; i++) e->bgy[i] = rd16();
    for (i = 0; i < 2; i++) e->bgxref[i] = (int32_t)rd32();
    for (i = 0; i < 2; i++) e->bgyref[i] = (int32_t)rd32();
    for (i = 0; i < 2; i++) e->rota[i] = (int16_t)rd16();
    for (i = 0; i < 2; i++) e->rotb[i] = (int16_t)rd16();
    for (i = 0; i < 2; i++) e->rotc[i] = (int16_t)rd16();
    for (i = 0; i < 2; i++) e->rotd[i] = (int16_t)rd16();
    rd(e->win0, 4);
    rd(e->win1, 4);
    rd(e->wincnt, 4);
    rd(e->bgmosaic, 2);
    rd(e->objmosaic, 2);
    e->blendcnt = rd16();
    e->eva = (uint8_t)fgetc(g);
    e->evb = (uint8_t)fgetc(g);
    e->evy = (uint8_t)fgetc(g);
    (void)fgetc(g);
}

static int load(const char *path)
{
    char magic[4];
    uint32_t version;
    int i;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    rd(magic, 4);
    if (memcmp(magic, "NDS2", 4) != 0) {
        fprintf(stderr, "%s: not a snapshot\n", path);
        return -1;
    }
    version = rd32();
    if (version != 3) {
        fprintf(stderr, "%s: snapshot version %u, expected 3\n", path,
                version);
        return -1;
    }
    (void)rd32(); /* frame number */
    gpu.powcnt = rd16();
    gpu.brightness[0] = rd16();
    gpu.brightness[1] = rd16();
    (void)rd16();
    rd(gpu.vram.vramcnt, 9);
    (void)fgetc(g); /* VRAMSTAT */
    (void)rd16();
    read_engine(&gpu.eng[0]);
    read_engine(&gpu.eng[1]);
    rd(gpu.palette, sizeof gpu.palette);
    rd(gpu.oam, sizeof gpu.oam);
    rd(gpu.vram.a, sizeof gpu.vram.a);
    rd(gpu.vram.b, sizeof gpu.vram.b);
    rd(gpu.vram.c, sizeof gpu.vram.c);
    rd(gpu.vram.d, sizeof gpu.vram.d);
    rd(gpu.vram.e, sizeof gpu.vram.e);
    rd(gpu.vram.f, sizeof gpu.vram.f);
    rd(gpu.vram.g, sizeof gpu.vram.g);
    rd(gpu.vram.h, sizeof gpu.vram.h);
    rd(gpu.vram.i, sizeof gpu.vram.i);
    for (i = 0; i < 8; i++) rd(refflat[i], FLAT[i].len);
    rd(gpu.plane3d, sizeof gpu.plane3d);
    rd(ref_top, sizeof ref_top);
    rd(ref_bot, sizeof ref_bot);
    fclose(g);
    return 0;
}

static void write_ppm(const char *path, const uint32_t *fb)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", DS10_SCR_W, DS10_SCR_H);
    for (i = 0; i < FB; i++) {
        fputc((fb[i] >> 16) & 0xFF, f);
        fputc((fb[i] >> 8) & 0xFF, f);
        fputc(fb[i] & 0xFF, f);
    }
    fclose(f);
}

static unsigned long diff(const uint32_t *a, const uint32_t *b)
{
    unsigned long n = 0;
    int i;

    for (i = 0; i < FB; i++)
        if (a[i] != b[i]) n++;
    return n;
}

int main(int argc, char **argv)
{
    unsigned long badflat = 0, badpix = 0;
    int files = 0, i, k;

    if (argc < 2) {
        fprintf(stderr, "usage: test_2d <snapshot>...\n"
                "  generate with: ./out/ndsref <rom> -o out/ref -e 60\n");
        return 2;
    }

    for (i = 1; i < argc; i++) {
        const uint8_t *mine[8];
        unsigned long dt, db;

        if (load(argv[i]) < 0) return 2;
        files++;

        ds10_vram_flatten(&gpu.vram);
        mine[0] = gpu.vram.abg;
        mine[1] = gpu.vram.bbg;
        mine[2] = gpu.vram.aobj;
        mine[3] = gpu.vram.bobj;
        mine[4] = gpu.vram.abgextpal;
        mine[5] = gpu.vram.bbgextpal;
        mine[6] = gpu.vram.aobjextpal;
        mine[7] = gpu.vram.bobjextpal;
        for (k = 0; k < 8; k++) {
            size_t j, n = 0, first = 0;

            for (j = 0; j < FLAT[k].len; j++)
                if (mine[k][j] != refflat[k][j]) {
                    if (!n) first = j;
                    n++;
                }
            if (n) {
                fprintf(stderr, "%s: %s differs in %lu of %lu bytes, "
                        "first at %#lx (ours %02x, melonDS %02x)\n",
                        argv[i], FLAT[k].name, (unsigned long)n,
                        (unsigned long)FLAT[k].len, (unsigned long)first,
                        mine[k][first], refflat[k][first]);
                badflat++;
            }
        }

        ds10_gpu_frame(&gpu, ours_top, ours_bot);
        dt = diff(ours_top, ref_top);
        db = diff(ours_bot, ref_bot);
        if (dt || db) {
            char p[512];
            size_t base = strlen(argv[i]);

            if (base > 5 && !strcmp(argv[i] + base - 5, ".snap")) base -= 5;
            fprintf(stderr, "%s: top %lu/%d pixels differ, bottom %lu/%d\n",
                    argv[i], dt, FB, db, FB);
            snprintf(p, sizeof p, "%.*s_ours_top.ppm", (int)base, argv[i]);
            write_ppm(p, ours_top);
            snprintf(p, sizeof p, "%.*s_ours_bot.ppm", (int)base, argv[i]);
            write_ppm(p, ours_bot);
            snprintf(p, sizeof p, "%.*s_ref_top.ppm", (int)base, argv[i]);
            write_ppm(p, ref_top);
            snprintf(p, sizeof p, "%.*s_ref_bot.ppm", (int)base, argv[i]);
            write_ppm(p, ref_bot);
            badpix++;
        }
    }

    if (ds10_2d_unsupported)
        fprintf(stderr, "note: hit an unimplemented path: %s\n",
                ds10_2d_unsupported);
    printf("%s: %d frames, %lu VRAM mismatches, %lu frame mismatches\n",
           (badflat || badpix) ? "FAIL" : "PASS", files, badflat, badpix);
    return (badflat || badpix) ? 1 : 0;
}
