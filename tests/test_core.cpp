// Linux unit tests: run the mod's signature scanner against the real game
// executable (mapped at its virtual layout) and exercise the fill planner.
#include "../src/core.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <regex>
#include <string>
#include <map>

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("  ok   %s\n", msg); else { printf("  FAIL %s\n", msg); fails++; } } while (0)

// pull SIG_* string literals out of main.cpp so the test uses the shipped patterns
static std::map<std::string, std::string> load_sigs(const char* path) {
    std::ifstream f(path); std::string s((std::istreambuf_iterator<char>(f)), {});
    std::regex rx("static const char\\* (SIG_[A-Z_]+)\\s*=\\s*\"([^\"]+)\"");
    std::map<std::string, std::string> out;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), rx); it != std::sregex_iterator(); ++it)
        out[(*it)[1]] = (*it)[2];
    return out;
}

#ifndef SRC_DIR
#define SRC_DIR "."
#endif
// Checks against the real game executable (not part of the repository): every
// signature must match exactly once at the expected address, hooks must steal
// position-independent bytes. Skipped when the exe is not available.
static void binary_checks(const char* exe) {
    std::ifstream f(exe, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
    if (file.size() < 0x1000) { printf("game executable not found (%s): skipping signature checks\n", exe); return; }
    uint32_t pe = *(uint32_t*)&file[0x3c];
    uint16_t ns = *(uint16_t*)&file[pe + 6];
    uint32_t soi = *(uint32_t*)&file[pe + 24 + 56];
    uint16_t optsz = *(uint16_t*)&file[pe + 20];
    std::vector<uint8_t> img(soi);
    uint32_t textVa = 0, textSz = 0;
    for (int i = 0; i < ns; i++) {
        uint8_t* sh = &file[pe + 24 + optsz + 40 * i];
        uint32_t vs = *(uint32_t*)(sh + 8), va = *(uint32_t*)(sh + 12), rs = *(uint32_t*)(sh + 16), ro = *(uint32_t*)(sh + 20);
        memcpy(&img[va], &file[ro], rs < vs ? rs : vs);
        if (!memcmp(sh, ".text", 5)) { textVa = va; textSz = vs; }
    }
    printf("image mapped: size 0x%x, .text 0x%x+0x%x\n", soi, textVa, textSz);

    auto sigs = load_sigs(SRC_DIR "/../src/main.cpp");
    printf("signatures from main.cpp: %zu\n", sigs.size());
    std::map<std::string, uint32_t> expect = {
        {"SIG_PLACE", 0x48a660}, {"SIG_REMOVE", 0x48b180}, {"SIG_BOUNDS", 0x48e8a0},
        {"SIG_CURSOR_CALL", 0x2d33fb}, {"SIG_PLACE_CALL", 0x2d3743},
        {"SIG_SERVER_CHECK", 0x2d36f2}, {"SIG_PAY", 0x48eeb0},
        {"SIG_PLACE_COMMIT", 0x487270}, {"SIG_REMOVE_COMMIT", 0x487af0}, {"SIG_MATERIAL_ITEM", 0xe75240}, {"SIG_TEARDOWN_EVENT", 0x490540}, {"SIG_REFUND_BUILD", 0x48ae10}, {"SIG_BLIT_CHECK", 0xa81327}, {"SIG_WRITE_TABLE", 0x48f1c0}, {"SIG_BEGIN_UNDO", 0x4919a0}, {"SIG_POP_UNDO", 0x492480}, {"SIG_HUD_UPDATE", 0xb25350}, {"SIG_UI_SOUND", 0xb26b8f}, {"SIG_CURSOR_QUERY", 0x287b10}, {"SIG_CURSOR_XF", 0x28bc80}, {"SIG_SYS_DEPS", 0x9bc1a0}, {"SIG_HOVER_FX", 0x28e2f0}, {"SIG_DISM_DEPS", 0x2d3c99},
        {"SIG_PAY_PLACE", 0x2d39ab}, {"SIG_PAY_REMOVE", 0x2d46b0}, {"SIG_PAY_REMOVE_PREFAB", 0x2d4b5a}, {"SIG_PAY_UNDO", 0x2d6040}, {"SIG_MISSING_CALL", 0x2975f2},
    };
    CHECK(sigs.size() == expect.size(), "all signatures extracted");
    for (auto& [name, rva] : expect) {
        const uint8_t* hits[4];
        int n = core::scan(&img[textVa], textSz, sigs[name].c_str(), hits, 4);
        char m[160];
        snprintf(m, sizeof m, "%s unique (%d) at expected rva 0x%x (got 0x%lx)", name.c_str(), n, rva,
                 n ? (unsigned long)(hits[0] - &img[0]) : 0ul);
        CHECK(n == 1 && (uint32_t)(hits[0] - &img[0]) == rva, m);
    }
    // the hook steals 10 bytes: mov [rsp+18],rsi ; mov [rsp+20],rdi
    const uint8_t stolen[10] = {0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7C,0x24,0x20};
    CHECK(!memcmp(&img[0x48a660], stolen, 10), "hook prologue is two position-independent movs");
    const uint8_t payStolen[7] = {0x40,0x55,0x56,0x48,0x83,0xEC,0x78};
    CHECK(!memcmp(&img[0x48eeb0], payStolen, 7), "pay hook steals push rbp; push rsi; sub rsp,78h");
    {
        const uint32_t sig = 0xa81327;
        const uint8_t site[4] = {0x41,0x80,0xFB,0x80};
        CHECK(!memcmp(&img[sig + 0x29], site, 4), "blit patch site = cmp r11b,80h");
        CHECK(sig + 0x29 + 35 == 0xa81373 && img[0xa81373] == 0x41 && img[0xa81374] == 0x80 && img[0xa81375] == 0xBA, "blit continue = cmp byte [r10+0A0h],0");
        CHECK(sig + 0x104 == 0xa8142b, "blit skip target");
        CHECK(sig + 0xdb == 0xa81402 && img[0xa81402] == 0x41 && img[0xa81403] == 0x80 && img[0xa81404] == 0x7A, "blit building write path = cmp byte [r10+30h],0");
    }
    { const uint32_t u = 0xb26b8f;
      uint32_t ctx = u + 7 + *(int32_t*)&img[u + 3]; uint32_t call = u + 0x2d + *(int32_t*)&img[u + 0x29];
      CHECK(ctx == 0x281e498 && call == 0xa429f0 && img[u + 0x28] == 0xE8, "ui sound: context global and play function"); }
    CHECK(!memcmp(&img[0xb25350], "\x40\x55\x53\x56\x57", 5), "hud hook steals push rbp/rbx/rsi/rdi");
    CHECK(!memcmp(&img[0x287b10], "\x48\x89\x5C\x24\x18", 5), "cursor query hook steals mov [rsp+18h],rbx");
    CHECK(0x2d3c99 + 0x15 + *(int32_t*)&img[0x2d3c99 + 0x11] == 0x9bc1a0, "dismantle deps call targets 9bc1a0");
    CHECK(!memcmp(&img[0x2d4197], "\x48\x8B\x85\x70\x03\x00\x00", 7), "dismantle reads pickup writer at deps+0x180 (rbp+0x370)");
    { const uint32_t h = 0x28e2f0;
      auto rel = [&](uint32_t at) { return at + 5 + *(int32_t*)&img[at + 1]; };
      CHECK(rel(h + 0x4fd) == 0x9b9490 && rel(h + 0x51d) == 0x29d420 && rel(h + 0x607) == 0x29d3d0, "hover fn: time / stop / start calls");
      CHECK(h + 0x5e7 + 7 + *(int32_t*)&img[h + 0x5e7 + 3] == 0x150ba30 && *(uint32_t*)&img[h + 0x436] == 0xb0718612, "hover fn: spawn defaults and highlight guid");
      CHECK(!memcmp(&img[h], "\x48\x89\x5C\x24\x10", 5), "hover fn hook steals mov [rsp+10h],rbx"); }
    const uint8_t popStolen[5] = {0x48,0x89,0x5C,0x24,0x10};
    CHECK(!memcmp(&img[0x492480], popStolen, 5), "pop hook steals mov [rsp+10h],rbx");
    const uint8_t commitStolen[5] = {0x48,0x8B,0xC4,0x55,0x53};
    CHECK(!memcmp(&img[0x487270], commitStolen, 5), "commit hook steals mov rax,rsp; push rbp; push rbx");
    CHECK(0x2d36f2 + 81 == 0x2d3743, "server branch sits directly before the placement call");
    // call-site distance check used by the mod
    CHECK(0x2d3743 > 0x2d33fb && 0x2d3743 - 0x2d33fb < 0x400, "call-site layout check passes");

}

int main(int argc, char** argv) {
    const char* exe = argc > 1 ? argv[1] : (getenv("ENSHROUDED_EXE") ? getenv("ENSHROUDED_EXE") : "enshrouded.exe");
    binary_checks(exe);
    // ---------------------------------------------------------------- planner
    const int64_t M = (int64_t)1 << 32;
    float step2[3] = {2.f, 2.f, 2.f};
    core::Plan pl;
    // line of 5 blocks along x (corners 0 and 8 m; 3rd click repeats corner 2)
    core::I3 line[3] = {{0, 0, 0}, {8 * M, 0, 0}, {8 * M, 0, 0}};
    CHECK(core::plan_fill(line, 3, step2, 2000, &pl) && pl.count[0] == 5 && pl.count[1] == 1 && pl.count[2] == 1, "line 0..8m step 2 -> 5x1x1");
    core::I3 last = core::plan_pos(pl, 4, 0, 0), first = core::plan_pos(pl, 0, 0, 0), mid = core::plan_pos(pl, 2, 0, 0);
    CHECK(first.x == 0 && last.x == 8 * M && mid.x == 4 * M, "line endpoints and midpoint exact");
    // plane on the floor, corners in any order
    core::I3 plane[3] = {{10 * M, 5 * M, -4 * M}, {4 * M, 5 * M, 6 * M}, {4 * M, 5 * M, 6 * M}};
    CHECK(core::plan_fill(plane, 3, step2, 2000, &pl) && pl.count[0] == 4 && pl.count[1] == 1 && pl.count[2] == 6, "floor plane 6x10m -> 4x1x6");
    CHECK(pl.total == 24, "plane total 24");
    // cube: base diagonal then height
    core::I3 cube[3] = {{0, 0, 0}, {4 * M, 0, 4 * M}, {4 * M, 6 * M, 4 * M}};
    CHECK(core::plan_fill(cube, 3, step2, 2000, &pl) && pl.count[0] == 3 && pl.count[1] == 4 && pl.count[2] == 3, "cube 4x6x4 -> 3x4x3");
    core::I3 top = core::plan_pos(pl, 2, 3, 2);
    CHECK(top.x == 4 * M && top.y == 6 * M && top.z == 4 * M, "cube far corner exact");
    // negative world coordinates
    core::I3 neg[3] = {{-100 * M, -3 * M, -50 * M}, {-96 * M, -3 * M, -50 * M}, {-96 * M, -3 * M, -50 * M}};
    CHECK(core::plan_fill(neg, 3, step2, 2000, &pl) && pl.count[0] == 3, "negative coordinates");
    // limit
    core::I3 huge[3] = {{0, 0, 0}, {200 * M, 0, 200 * M}, {200 * M, 40 * M, 200 * M}};
    CHECK(!core::plan_fill(huge, 3, step2, 2000, &pl) && pl.total > 2000, "over MaxShapes is refused");
    // half-metre shapes
    float stepHalf[3] = {0.5f, 0.5f, 0.5f};
    core::I3 small[3] = {{0, 0, 0}, {M, 0, 0}, {M, 0, 0}};
    CHECK(core::plan_fill(small, 3, stepHalf, 2000, &pl) && pl.count[0] == 3, "0.5 m shapes: 1 m line -> 3");
    // single point (same spot 3 times) -> one shape
    core::I3 pt[3] = {{7 * M, 1 * M, 2 * M}, {7 * M, 1 * M, 2 * M}, {7 * M, 1 * M, 2 * M}};
    CHECK(core::plan_fill(pt, 3, step2, 2000, &pl) && pl.total == 1, "same spot 3x -> 1 shape");
    // slightly off-grid corner (float noise) still gives whole steps
    core::I3 noisy[3] = {{0, 0, 0}, {8 * M + 3000, 0, 0}, {8 * M + 3000, 0, 0}};
    CHECK(core::plan_fill(noisy, 3, step2, 2000, &pl) && pl.count[0] == 5, "noise does not add a step");

    // shapes side by side: spacing is exactly the shape size
    {
        core::I3 odd[2] = {{0, 0, 0}, {(int64_t)(3.5 * M), 0, 0}};
        CHECK(core::plan_fill(odd, 2, step2, 2000, &pl) && pl.count[0] == 3, "3.5 m with 2 m shapes -> 3");
        core::I3 p1 = core::plan_pos(pl, 1, 0, 0), p2 = core::plan_pos(pl, 2, 0, 0);
        CHECK(p1.x == 2 * M && p2.x == 4 * M, "spacing exactly 2 m (no overlap)");
        core::I3 tiny[2] = {{0, 0, 0}, {M / 2, 0, 0}};
        CHECK(core::plan_fill(tiny, 2, step2, 2000, &pl) && pl.total == 1, "0.5 m offset with 2 m shapes -> 1 shape, no overlap");
        core::I3 back[2] = {{10 * M, 0, 0}, {4 * M, 0, 0}};
        CHECK(core::plan_fill(back, 2, step2, 2000, &pl) && pl.count[0] == 4 && core::plan_pos(pl, 3, 0, 0).x == 4 * M &&
              core::plan_pos(pl, 1, 0, 0).x == 8 * M, "runs from corner 1 toward corner 2 (negative direction)");
        core::I3 box2[2] = {{0, 0, 0}, {4 * M, 2 * M, 6 * M}};
        CHECK(core::plan_fill(box2, 2, step2, 2000, &pl) && pl.count[0] == 3 && pl.count[1] == 2 && pl.count[2] == 4, "2-corner box 3x2x4");
    }

    // fill modes
    {
        core::I3 room[2] = {{0, 0, 0}, {8 * M, 6 * M, 10 * M}};       // 5 x 4 x 6 shapes
        CHECK(core::plan_fill(room, 2, step2, 2000, &pl) && pl.total == 120, "room solid 5x4x6 = 120");
        CHECK(core::fill_count(pl, core::FILL_HOLLOW) == 120 - 3 * 2 * 4, "hollow removes the 3x2x4 inside");
        CHECK(core::fill_count(pl, core::FILL_WALLS) == 120 - 3 * 4 * 4, "walls only: no floor/ceiling inside");
        long n = 0; for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) for (int k = 0; k < 6; k++) n += core::fill_keep(pl, i, j, k, core::FILL_HOLLOW);
        CHECK(n == core::fill_count(pl, core::FILL_HOLLOW), "hollow keep matches count");
        n = 0; for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) for (int k = 0; k < 6; k++) n += core::fill_keep(pl, i, j, k, core::FILL_WALLS);
        CHECK(n == core::fill_count(pl, core::FILL_WALLS), "walls keep matches count");
        core::I3 floor2[2] = {{0, 0, 0}, {8 * M, 0, 8 * M}};
        CHECK(core::plan_fill(floor2, 2, step2, 2000, &pl) && core::fill_count(pl, core::FILL_HOLLOW) == pl.total, "hollow floor stays full");
        core::I3 big[2] = {{0, 0, 0}, {2000 * M, 2000 * M, 2000 * M}};
        CHECK(!core::plan_fill(big, 2, step2, 50000000, &pl), "huge area refused without overflow");
    }

    // ----------------------------------------------------- axis lock
    {
        const int64_t T = M / 10;
        core::I3 a{0, 0, 0}, b{6 * M, 0, 4 * M}, c{9 * M, 3 * M, -7 * M};   // floor plane, c drifts
        core::I3 l = core::lock_third(a, b, c, T);
        CHECK(l.x == 6 * M && l.z == 4 * M && l.y == 3 * M, "plane: corner 3 only adds height");
        core::I3 b2{6 * M, 0, 0};                                         // line along x
        l = core::lock_third(a, b2, c, T);
        CHECK(l.x == 6 * M && l.y == 3 * M && l.z == -7 * M, "line: corner 3 free on the other two axes");
        core::I3 b3{6 * M, 2 * M, 4 * M};                                 // already a box
        l = core::lock_third(a, b3, c, T);
        CHECK(l.x == b3.x && l.y == b3.y && l.z == b3.z, "box: corner 3 ignored");
        core::I3 b4{6 * M, 1000, 4 * M};                                  // sub-tolerance wobble on y
        l = core::lock_third(a, b4, c, T);
        CHECK(l.y == 3 * M, "tiny difference does not count as a used axis");
        core::I3 wall[3] = {a, {0, 4 * M, 8 * M}, core::lock_third(a, {0, 4 * M, 8 * M}, {4 * M, 9 * M, 1 * M}, T)};
        float s2[3] = {2.f, 2.f, 2.f};
        CHECK(core::plan_fill(wall, 3, s2, 2000, &pl) && pl.count[1] == 3 && pl.count[2] == 5 && pl.count[0] == 3,
              "wall 4x8 then 4 m thickness -> 3x3x5");
    }

    // ----------------------------------------------------- centre rounding
    CHECK(core::centre_fixed(1.f, 3.f) == 2 * M, "centre of 1..3 m is 2 m");
    CHECK(core::centre_fixed(-3.f, -1.f) == -2 * M, "centre of -3..-1 m is -2 m");

    // ----------------------------------------------------- pattern parser
    uint8_t b[8], m[8];
    CHECK(core::parse_sig("48 ?? 8b ?", b, m, 8) == 4 && m[1] == 0 && m[3] == 0 && b[2] == 0x8b, "wildcards parse");
    CHECK(core::parse_sig("4G", b, m, 8) == -1, "bad hex rejected");

    printf(fails ? "\n%d FAILED\n" : "\nall tests passed\n", fails);
    return fails ? 1 : 0;
}
