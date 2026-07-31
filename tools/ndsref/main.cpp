/* ndsref - run ds10plus.nds in melonDS and dump what the 2D hardware sees.
 *
 * Two things come out of this, and the GUI work needs both:
 *
 *   - the composited framebuffers, as pixel ground truth;
 *   - the whole 2D state that produced them - VRAM, palette, OAM, the display
 *     and background registers, the bank mapping.
 *
 * Feed a snapshot to our own renderer and the framebuffer it produces should
 * match melonDS's byte for byte. That is the differential test the Unicorn
 * harness cannot do, because Unicorn runs the CPU and stops at the hardware.
 *
 *   ndsref <rom> [-f frames] [-o outdir] [-s script] [-e every] [-b begin]
 *                [-w save.sav]
 *
 * The script is one command per line, "<frame> <command> [args]":
 *
 *   120 touch 128 96      pen down at (x, y), held until released
 *   140 release
 *   200 key A             press, held until "up"
 *   210 up A
 *   300 snap              dump this frame regardless of -e
 *
 * Frames are counted from reset. Everything is deterministic, so a script that
 * reaches a screen once reaches it every time.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "NDS.h"
#include "NDSCart.h"
#include "GPU.h"
#include "SPU.h"
#include "Platform.h"
#include "Args.h"
#include "GPU_Soft.h"

using namespace melonDS;

namespace melonDS::Platform {
void SaveWritten(const u8 *, u32) {}
}

/* The 3D engine's output is an input to the 2D compositor - DS-10 runs BG0 as
 * the 3D layer - so a snapshot has to carry it. melonDS keeps the 3D renderer
 * protected inside Renderer; a subclass is the sanctioned way to reach it, and
 * avoids patching melonDS. */
class RefRenderer : public SoftRenderer {
public:
    using SoftRenderer::SoftRenderer;
    u32 *Line3D(int y) { return Rend3D->GetLine(y); }
};

#define SNAP_MAGIC "NDS2"
#define SNAP_VERSION 3

struct Cmd {
    int frame;
    std::string op, arg;
    int x, y;
};

static const struct { const char *name; u32 bit; } KEYMAP[] = {
    {"A", 0}, {"B", 1}, {"SELECT", 2}, {"START", 3}, {"RIGHT", 4},
    {"LEFT", 5}, {"UP", 6}, {"DOWN", 7}, {"R", 8}, {"L", 9},
    {"X", 16}, {"Y", 17},
};

static u32 keybit(const std::string &n)
{
    for (auto &k : KEYMAP)
        if (n == k.name) return 1u << k.bit;
    fprintf(stderr, "unknown key '%s'\n", n.c_str());
    exit(2);
}

static std::vector<Cmd> load_script(const char *path)
{
    std::vector<Cmd> out;
    char line[256];
    FILE *f = fopen(path, "r");

    if (!f) {
        fprintf(stderr, "cannot open script %s\n", path);
        exit(2);
    }
    while (fgets(line, sizeof line, f)) {
        char op[64] = {0}, arg[64] = {0};
        Cmd c{};
        char *h = strchr(line, '#');

        if (h) *h = 0;
        if (sscanf(line, "%d %63s %d %d", &c.frame, op, &c.x, &c.y) < 2)
            continue;
        c.op = op;
        /* Re-read the tail as a word for the key commands. */
        if (sscanf(line, "%*d %*s %63s", arg) == 1) c.arg = arg;
        out.push_back(c);
    }
    fclose(f);
    return out;
}

static void put32(FILE *f, u32 v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, u16 v) { fwrite(&v, 2, 1, f); }

static void dump_engine(FILE *f, GPU2D &g)
{
    put32(f, g.DispCnt);
    for (int i = 0; i < 4; i++) put16(f, g.BGCnt[i]);
    for (int i = 0; i < 4; i++) put16(f, g.BGXPos[i]);
    for (int i = 0; i < 4; i++) put16(f, g.BGYPos[i]);
    for (int i = 0; i < 2; i++) put32(f, (u32)g.BGXRef[i]);
    for (int i = 0; i < 2; i++) put32(f, (u32)g.BGYRef[i]);
    for (int i = 0; i < 2; i++) put16(f, (u16)g.BGRotA[i]);
    for (int i = 0; i < 2; i++) put16(f, (u16)g.BGRotB[i]);
    for (int i = 0; i < 2; i++) put16(f, (u16)g.BGRotC[i]);
    for (int i = 0; i < 2; i++) put16(f, (u16)g.BGRotD[i]);
    fwrite(g.Win0Coords, 1, 4, f);
    fwrite(g.Win1Coords, 1, 4, f);
    fwrite(g.WinCnt, 1, 4, f);
    fwrite(g.BGMosaicSize, 1, 2, f);
    fwrite(g.OBJMosaicSize, 1, 2, f);
    put16(f, g.BlendCnt);
    fputc(g.EVA, f);
    fputc(g.EVB, f);
    fputc(g.EVY, f);
    fputc(0, f);
}

static void snapshot(NDS &nds, RefRenderer &rend, const std::string &dir,
                     int frame)
{
    char path[512];
    FILE *f;
    void *top = nullptr, *bot = nullptr;

    snprintf(path, sizeof path, "%s/f%05d.snap", dir.c_str(), frame);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fwrite(SNAP_MAGIC, 1, 4, f);
    put32(f, SNAP_VERSION);
    put32(f, (u32)frame);
    put16(f, nds.PowerControl9);
    put16(f, nds.GPU.MasterBrightnessA);
    put16(f, nds.GPU.MasterBrightnessB);
    put16(f, 0);
    fwrite(nds.GPU.VRAMCNT, 1, 9, f);
    fputc(nds.GPU.VRAMSTAT, f);
    put16(f, 0);
    dump_engine(f, nds.GPU.GPU2D_A);
    dump_engine(f, nds.GPU.GPU2D_B);
    fwrite(nds.GPU.Palette, 1, sizeof nds.GPU.Palette, f);
    fwrite(nds.GPU.OAM, 1, sizeof nds.GPU.OAM, f);
    fwrite(nds.GPU.VRAM_A, 1, sizeof nds.GPU.VRAM_A, f);
    fwrite(nds.GPU.VRAM_B, 1, sizeof nds.GPU.VRAM_B, f);
    fwrite(nds.GPU.VRAM_C, 1, sizeof nds.GPU.VRAM_C, f);
    fwrite(nds.GPU.VRAM_D, 1, sizeof nds.GPU.VRAM_D, f);
    fwrite(nds.GPU.VRAM_E, 1, sizeof nds.GPU.VRAM_E, f);
    fwrite(nds.GPU.VRAM_F, 1, sizeof nds.GPU.VRAM_F, f);
    fwrite(nds.GPU.VRAM_G, 1, sizeof nds.GPU.VRAM_G, f);
    fwrite(nds.GPU.VRAM_H, 1, sizeof nds.GPU.VRAM_H, f);
    fwrite(nds.GPU.VRAM_I, 1, sizeof nds.GPU.VRAM_I, f);

    /* melonDS renders out of "flat" buffers: the nine banks composited down
     * into one linear image per region, per the VRAMCNT mapping. Dumping those
     * as well splits the problem in two - our bank mapper can be checked
     * against these, and our renderer can be checked while fed these. */
    fwrite(nds.GPU.VRAMFlat_ABG, 1, sizeof nds.GPU.VRAMFlat_ABG, f);
    fwrite(nds.GPU.VRAMFlat_BBG, 1, sizeof nds.GPU.VRAMFlat_BBG, f);
    fwrite(nds.GPU.VRAMFlat_AOBJ, 1, sizeof nds.GPU.VRAMFlat_AOBJ, f);
    fwrite(nds.GPU.VRAMFlat_BOBJ, 1, sizeof nds.GPU.VRAMFlat_BOBJ, f);
    fwrite(nds.GPU.VRAMFlat_ABGExtPal, 1, sizeof nds.GPU.VRAMFlat_ABGExtPal, f);
    fwrite(nds.GPU.VRAMFlat_BBGExtPal, 1, sizeof nds.GPU.VRAMFlat_BBGExtPal, f);
    fwrite(nds.GPU.VRAMFlat_AOBJExtPal, 1,
           sizeof nds.GPU.VRAMFlat_AOBJExtPal, f);
    fwrite(nds.GPU.VRAMFlat_BOBJExtPal, 1,
           sizeof nds.GPU.VRAMFlat_BOBJExtPal, f);

    /* The 3D plane, as the compositor saw it: 0 alpha means transparent. */
    for (int y = 0; y < 192; y++)
        fwrite(rend.Line3D(y), 4, 256, f);

    nds.GPU.GetFramebuffers(&top, &bot);
    fwrite(top, 1, 256 * 192 * 4, f);
    fwrite(bot, 1, 256 * 192 * 4, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *rom = nullptr, *script = nullptr, *save = nullptr;
    std::string dir = "out/ref";
    int frames = 600, every = 0, begin = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) begin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) save = argv[++i];
        else if (argv[i][0] != '-') rom = argv[i];
    }
    if (!rom) {
        fprintf(stderr, "usage: ndsref <rom> [-f frames] [-o dir] "
                        "[-s script] [-e every] [-b begin] [-w save]\n");
        return 2;
    }

    std::vector<Cmd> cmds;
    if (script) cmds = load_script(script);

    FILE *rf = fopen(rom, "rb");
    if (!rf) {
        fprintf(stderr, "cannot open %s\n", rom);
        return 2;
    }
    fseek(rf, 0, SEEK_END);
    long romlen = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    auto romdata = std::make_unique<u8[]>(romlen);
    if (fread(romdata.get(), 1, romlen, rf) != (size_t)romlen) {
        fprintf(stderr, "short read on %s\n", rom);
        return 2;
    }
    fclose(rf);

    NDSArgs args{};
    args.BitDepth = AudioBitDepth::_16Bit;
    args.Interpolation = AudioInterpolation::None;
    /* Interpreter only. The JIT wants a large fixed mapping that this sandbox
     * will not give it, and nothing here is speed-critical enough to care. */
    args.JIT = std::nullopt;
    /* Several megabytes of timing tables live inside NDS, so it does not go
     * on the stack. */
    auto ndsp = std::make_unique<NDS>(std::move(args));
    NDS &nds = *ndsp;

    std::optional<NDSCart::NDSCartArgs> cartargs = std::nullopt;
    if (save) {
        FILE *sf = fopen(save, "rb");

        if (!sf) {
            fprintf(stderr, "cannot open %s\n", save);
            return 2;
        }
        fseek(sf, 0, SEEK_END);
        long savelen = ftell(sf);
        fseek(sf, 0, SEEK_SET);
        NDSCart::NDSCartArgs a{};
        a.SRAM = std::make_unique<u8[]>(savelen);
        if (fread(a.SRAM.get(), 1, savelen, sf) != (size_t)savelen) {
            fprintf(stderr, "short read on %s\n", save);
            return 2;
        }
        fclose(sf);
        a.SRAMLength = (u32)savelen;
        cartargs = std::move(a);
    }

    auto cart = NDSCart::ParseROM(std::move(romdata), (u32)romlen, nullptr,
                                  std::move(cartargs));
    if (!cart) {
        fprintf(stderr, "%s: not a valid NDS ROM\n", rom);
        return 2;
    }
    nds.SetNDSCart(std::move(cart));
    auto rendp = std::make_unique<RefRenderer>(nds);
    RefRenderer *rend = rendp.get();
    nds.GPU.SetRenderer(std::move(rendp));
    nds.Reset();
    /* No BIOS or firmware images here, so the ROM is direct-booted: the
     * loader's job is done for it and execution starts at the ARM9 entry. */
    nds.SetupDirectBoot(rom);
    nds.Start();

    char mk[600];
    snprintf(mk, sizeof mk, "mkdir -p '%s'", dir.c_str());
    if (system(mk) != 0) return 2;

    u32 keys = 0;
    size_t next = 0;
    int shots = 0;

    for (int frame = 0; frame < frames; frame++) {
        bool forced = false;

        while (next < cmds.size() && cmds[next].frame <= frame) {
            const Cmd &c = cmds[next++];

            if (c.op == "touch") nds.TouchScreen((u16)c.x, (u16)c.y);
            else if (c.op == "release") nds.ReleaseScreen();
            else if (c.op == "key") keys |= keybit(c.arg);
            else if (c.op == "up") keys &= ~keybit(c.arg);
            else if (c.op == "snap") forced = true;
            else fprintf(stderr, "frame %d: unknown command '%s'\n",
                         c.frame, c.op.c_str());
        }
        nds.SetKeyMask(~keys & 0xFFFF);
        nds.RunFrame();

        if (forced || (every && frame >= begin && (frame - begin) % every == 0)) {
            snapshot(nds, *rend, dir, frame);
            shots++;
        }
    }
    printf("%d frames, %d snapshots in %s\n", frames, shots, dir.c_str());
    return 0;
}
