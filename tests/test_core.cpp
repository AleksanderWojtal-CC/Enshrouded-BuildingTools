// Linux unit tests: run the mod's signature scanner against the real game
// executable (mapped at its virtual layout) and exercise the fill planner.
#include "../src/core.h"
#include "../src/strings.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <regex>
#include <string>
#include <map>
#include <algorithm>

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
        {"SIG_PLACE", 0x491930}, {"SIG_REMOVE", 0x492410}, {"SIG_BOUNDS", 0x495830},
        {"SIG_CURSOR_CALL", 0x2d851b}, {"SIG_PLACE_CALL", 0x2d8863},
        {"SIG_SERVER_CHECK", 0x2d8812}, {"SIG_PAY", 0x495e40},
        {"SIG_PLACE_COMMIT", 0x48e610}, {"SIG_REMOVE_COMMIT", 0x48ee40}, {"SIG_MATERIAL_ITEM", 0xe8c1b0}, {"SIG_REFUND_BUILD", 0x4920f0}, {"SIG_BLIT_CHECK", 0xa8e1e7}, {"SIG_POP_UNDO", 0x499460}, {"SIG_HUD_UPDATE", 0xb34c20}, {"SIG_UI_SOUND", 0xb3645a}, {"SIG_CURSOR_XF", 0x2904e0}, {"SIG_SYS_DEPS", 0x9c7ac0}, {"SIG_HOVER_FX", 0x292b70}, {"SIG_DISM_DEPS", 0x2d8db9}, {"SIG_INPUT_ENCODE", 0x3ab930},
        {"SIG_PAY_PLACE", 0x2d8acb}, {"SIG_PAY_REMOVE", 0x2d97dc}, {"SIG_PAY_REMOVE_PREFAB", 0x2d9da5}, {"SIG_PAY_UNDO", 0x2db270}, {"SIG_MISSING_CALL", 0x29bfb2}, {"SIG_STOCKS", 0x40e4f0}, {"SIG_PICK_CALL", 0x2b4707}, {"SIG_ZONE_VIS_CALL", 0x860170},
        {"SIG_UI_DRAW_RECT", 0x11a8a00}, {"SIG_UI_DRAW_ROTATED", 0x11a9000}, {"SIG_UI_BOX_BLEND", 0x11ca820},
        {"SIG_UI_SCROLL_PARAMS", 0xa3edf0}, {"SIG_UI_SCROLL_BEGIN", 0xa3df60}, {"SIG_UI_SCROLL_END", 0xa400a0},
        {"SIG_UI_SCROLL_SPEED", 0xd4e320}, {"SIG_PIECE_CHECK_CALL", 0x2d94f2}, {"SIG_UI_SETTING_OPTION_CALL", 0xc54b53},
        {"SIG_TOGGLE_SCREEN", 0xd74790}, {"SIG_UI_SMALL_TOGGLE", 0xc37e69},
    };
    bool allThere = true;
    for (auto& e : expect) allThere &= sigs.count(e.first) == 1;
    CHECK(allThere, "every checked signature is in main.cpp");
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
    CHECK(!memcmp(&img[0x491930], stolen, 10), "hook prologue is two position-independent movs");
    const uint8_t payStolen[7] = {0x40,0x55,0x56,0x48,0x83,0xEC,0x78};
    CHECK(!memcmp(&img[0x495e40], payStolen, 7), "pay hook steals push rbp; push rsi; sub rsp,78h");
    {
        const uint32_t sig = 0xa8e1e7;
        const uint8_t site[4] = {0x41,0x80,0xFB,0x80};
        CHECK(!memcmp(&img[sig + 0x29], site, 4), "blit patch site = cmp r11b,80h");
        CHECK(sig + 0x29 + 35 == 0xa8e233 && img[0xa8e233] == 0x41 && img[0xa8e234] == 0x80 && img[0xa8e235] == 0xBA, "blit continue = cmp byte [r10+0A0h],0");
        CHECK(sig + 0x104 == 0xa8e2eb, "blit skip target");
        CHECK(sig + 0xdb == 0xa8e2c2 && img[0xa8e2c2] == 0x41 && img[0xa8e2c3] == 0x80 && img[0xa8e2c4] == 0x7A, "blit building write path = cmp byte [r10+30h],0");
    }
    { const uint32_t u = 0xb3645a;
      uint32_t ctx = u + 7 + *(int32_t*)&img[u + 3]; uint32_t call = u + 0x2d + *(int32_t*)&img[u + 0x29];
      CHECK(ctx == 0x28404a8 && call == 0xa4f250 && img[u + 0x28] == 0xE8, "ui sound: context global and play function"); }
    CHECK(!memcmp(&img[0xb34c20], "\x48\x8B\xC4\x55\x53", 5), "hud hook steals mov rax,rsp; push rbp; push rbx");
    CHECK(!memcmp(&img[0x28c350], "\x48\x89\x5C\x24\x18", 5), "cursor query hook steals mov [rsp+18h],rbx");
    CHECK(0x2d8db9 + 0x15 + *(int32_t*)&img[0x2d8db9 + 0x11] == 0x9c7ac0, "dismantle deps call targets 9c7ac0");
    CHECK(!memcmp(&img[0x2d92c4], "\x48\x8B\x85\x50\x03\x00\x00", 7), "dismantle reads pickup writer at deps+0x180 (rbp+0x350)");
    CHECK(*(int32_t*)&img[0x2d94f2 + 0x1c] == 0x1d8, "dismantle aim state at rbp+0x1d8");
    CHECK(!memcmp(&img[0x4920f0], "\x48\x8B\xC4\x48\x89\x48\x08", 7), "refund hook steals mov rax,rsp; mov [rax+8],rcx");
    CHECK(!memcmp(&img[0x48ee40], "\x48\x89\x5C\x24\x18", 5), "remove commit hook steals mov [rsp+18h],rbx");
    CHECK(!memcmp(&img[0x3aa8a0], "\x48\x8D\x81\xD0\xFF\x00\x00\xC3", 8) && 0x860170 + 0xc + *(int32_t*)&img[0x860170 + 8] == 0x3aa8a0, "zone vis getter: lea rax,[rcx+0xffd0]; ret");
    { const uint32_t h = 0x292b70;
      auto rel = [&](uint32_t at) { return at + 5 + *(int32_t*)&img[at + 1]; };
      CHECK(rel(h + 0x4fd) == 0x9c4d00 && rel(h + 0x51d) == 0x2a1e90 && rel(h + 0x607) == 0x2a1e40, "hover fn: time / stop / start calls");
      CHECK(h + 0x5e7 + 7 + *(int32_t*)&img[h + 0x5e7 + 3] == 0x1529a88 && *(uint32_t*)&img[h + 0x436] == 0xb0718612, "hover fn: spawn defaults and highlight guid");
      CHECK(!memcmp(&img[h], "\x48\x89\x5C\x24\x10", 5), "hover fn hook steals mov [rsp+10h],rbx"); }
    const uint8_t popStolen[5] = {0x48,0x89,0x5C,0x24,0x10};
    CHECK(!memcmp(&img[0x499460], popStolen, 5), "pop hook steals mov [rsp+10h],rbx");
    const uint8_t commitStolen[5] = {0x48,0x8B,0xC4,0x55,0x53};
    CHECK(!memcmp(&img[0x48e610], commitStolen, 5), "commit hook steals mov rax,rsp; push rbp; push rbx");
    CHECK(0x2d8812 + 81 == 0x2d8863, "server branch sits directly before the placement call");
    // call-site distance check used by the mod
    CHECK(0x2d8863 > 0x2d851b && 0x2d8863 - 0x2d851b < 0x400, "call-site layout check passes");

}

// Translation files (lang/<code>.txt): one per language of the game's menu, only keys
// of src/strings.h, each once, every text with the same {n} placeholders as English,
// and lang/en.txt equal to the built-in English.
static const char* const kKeys[S_COUNT] = {
#define K(id, key, en) key,
    BT_STRINGS(K)
#undef K
};
static const char* const kEn[S_COUNT] = {
#define E(id, key, en) en,
    BT_STRINGS(E)
#undef E
};
static std::string placeholders(const std::string& t) {
    std::string out;
    for (size_t i = 0; i + 2 < t.size(); i++)
        if (t[i] == '{' && t[i + 1] >= '0' && t[i + 1] <= '3' && t[i + 2] == '}') { out += t[i + 1]; }
    std::sort(out.begin(), out.end());
    return out;
}
static void translation_checks() {
    std::map<std::string, int> index;
    for (int k = 0; k < S_COUNT; k++) {
        CHECK(index.count(kKeys[k]) == 0 || !printf("    duplicate key %s\n", kKeys[k]), kKeys[k]);
        index[kKeys[k]] = k;
    }
    for (int l = 0; l < kLanguageCount; l++) {
        std::string code = kLanguages[l].code, path = std::string(SRC_DIR) + "/../lang/" + code + ".txt";
        std::ifstream f(path);
        std::string head = "lang/" + code + ".txt";
        if (!f) { CHECK(false, (head + " exists").c_str()); continue; }
        std::vector<int> seen(S_COUNT, 0);
        std::string line; int bad = 0, n = 0;
        while (std::getline(f, line)) {
            n++;
            if (n == 1 && line.size() >= 3 && (uint8_t)line[0] == 0xef) line = line.substr(3);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos || line[a] == '#' || line[a] == ';') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) { printf("    %s:%d no '='\n", head.c_str(), n); bad++; continue; }
            std::string key = line.substr(a, eq - a), val = line.substr(eq + 1);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            val.erase(0, val.find_first_not_of(" \t") == std::string::npos ? val.size() : val.find_first_not_of(" \t"));
            auto it = index.find(key);
            if (it == index.end()) { printf("    %s:%d unknown key %s\n", head.c_str(), n, key.c_str()); bad++; continue; }
            int k = it->second;
            if (seen[k]++) { printf("    %s:%d duplicate %s\n", head.c_str(), n, key.c_str()); bad++; }
            std::string v; for (size_t i = 0; i < val.size(); i++) { if (val[i] == '\\' && i + 1 < val.size() && val[i + 1] == 'n') { v += '\n'; i++; } else v += val[i]; }
            if (placeholders(v) != placeholders(kEn[k])) { printf("    %s:%d %s placeholders differ from English\n", head.c_str(), n, key.c_str()); bad++; }
            if (code == "en" && v != kEn[k]) { printf("    %s:%d %s differs from src/strings.h\n", head.c_str(), n, key.c_str()); bad++; }
            if (code != "en" && v.empty() && k != S_MENU_TRANSLATION_NOTE) { printf("    %s:%d %s is empty\n", head.c_str(), n, key.c_str()); bad++; }
        }
        int missing = 0;
        for (int k = 0; k < S_COUNT; k++) if (!seen[k]) { printf("    %s: missing %s\n", head.c_str(), kKeys[k]); missing++; }
        CHECK(bad == 0 && missing == 0, (head + ": every key once, placeholders as English").c_str());
    }
}

int main(int argc, char** argv) {
    const char* exe = argc > 1 ? argv[1] : (getenv("ENSHROUDED_EXE") ? getenv("ENSHROUDED_EXE") : "enshrouded.exe");
    binary_checks(exe);
    translation_checks();
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

    // roofs
    {
        const float step1[3] = { 1.f, 1.f, 1.f };
        auto keepCount = [&](const core::Plan& p, int mode) {
            long n = 0;
            for (int i = 0; i < p.count[0]; i++) for (int j = 0; j < p.count[1]; j++) for (int k = 0; k < p.count[2]; k++)
                n += core::fill_keep(p, i, j, k, mode);
            return n;
        };
        const int R = core::FILL_ROOF;
        auto fo = [](int shape, int rot = 0, int thick = 1, int o = 0) { return core::FillOpts{ shape, rot, thick, o }; };
        // flat 7 x 4 area: gable slopes across the short side (z)
        core::I3 flat[2] = {{0, 0, 0}, {6 * M, 0, 3 * M}};
        CHECK(core::plan_area(flat, 2, step1, 2000, R, fo(core::ROOF_GABLE), &pl) && !pl.roofAxisX && pl.count[1] == 2,
              "flat 7x4 gable: ridge along the long side, grows to 2 high");
        CHECK(core::fill_count(pl, R) == 7 * 4 && keepCount(pl, R) == 7 * 4, "45 degree gable: one shape per column");
        CHECK(core::fill_keep(pl, 3, 0, 0, R) && core::fill_keep(pl, 3, 1, 1, R) && core::fill_keep(pl, 3, 1, 2, R) &&
              core::fill_keep(pl, 3, 0, 3, R) && !core::fill_keep(pl, 3, 1, 0, R), "gable steps: up one per shape in");
        CHECK(core::plan_area(flat, 2, step1, 2000, R, fo(core::ROOF_GABLE, 1), &pl) && pl.roofAxisX && pl.count[1] == 4,
              "turned gable: ridge along the short side, 4 high");
        // hip over a flat 5 x 5: a pyramid 3 high
        core::I3 sq[2] = {{0, 0, 0}, {4 * M, 0, 4 * M}};
        CHECK(core::plan_area(sq, 2, step1, 2000, R, fo(core::ROOF_HIP), &pl) && pl.count[1] == 3, "hip 5x5 -> 3 high");
        CHECK(core::fill_count(pl, R) == 25 && core::fill_keep(pl, 2, 2, 2, R) && core::fill_keep(pl, 1, 1, 3, R) &&
              core::fill_keep(pl, 0, 0, 4, R), "hip pyramid");
        // shed over a flat 5 wide: rises one per shape, 5 high, 4 ways round
        CHECK(core::plan_area(sq, 2, step1, 2000, R, fo(core::ROOF_SHED), &pl) && pl.count[1] == 5 &&
              core::fill_keep(pl, 0, 0, 0, R) && core::fill_keep(pl, 0, 4, 4, R), "shed rises across the area");
        CHECK(core::plan_area(sq, 2, step1, 2000, R, fo(core::ROOF_SHED, 2), &pl) && core::fill_keep(pl, 0, 4, 0, R) &&
              core::fill_keep(pl, 0, 0, 4, R), "shed turned twice rises the other way");
        // gambrel / mansard: steep below, flatter above
        core::I3 wide[2] = {{0, 0, 0}, {8 * M, 0, 20 * M}};       // 9 across -> D 4, 5 high
        CHECK(core::plan_area(wide, 2, step1, 2000, R, fo(core::ROOF_GAMBREL), &pl) && pl.count[1] == 5, "gambrel 5 high");
        int lo, hi; core::roof_column(pl, 1, 0, &lo, &hi);
        int lo2, hi2; core::roof_column(pl, 4, 0, &lo2, &hi2);
        CHECK(hi == 2 && hi2 == 4 && lo2 == 4, "gambrel: steep lower slope (2 up in the first shape), flat-ish top");
        CHECK(core::plan_area(wide, 2, step1, 2000, R, fo(core::ROOF_MANSARD), &pl) && keepCount(pl, R) == core::fill_count(pl, R),
              "mansard keep matches count");
        // 3D area keeps its height (steep: columns stack)
        core::I3 tall[2] = {{0, 0, 0}, {10 * M, 8 * M, 8 * M}};
        CHECK(core::plan_area(tall, 2, step1, 2000, R, fo(core::ROOF_GABLE), &pl) && pl.count[1] == 9, "3D gable keeps its height");
        CHECK(core::fill_keep(pl, 0, 8, 4, R) && core::fill_keep(pl, 0, 0, 0, R) && core::fill_keep(pl, 0, 2, 1, R) &&
              core::fill_keep(pl, 0, 1, 1, R) && !core::fill_keep(pl, 0, 0, 1, R), "steep gable: each column stacks up to its own top");
        // overhang: the plan widens and drops; the slope carries on down
        CHECK(core::plan_area(flat, 2, step1, 2000, R, fo(core::ROOF_GABLE, 0, 1, 2), &pl) && pl.count[0] == 11 && pl.count[2] == 8 &&
              pl.count[1] == 4 && pl.roofBase == 2 && pl.origin[0] == -2 * M && pl.origin[1] == -2 * M, "overhang 2 widens and drops the plan");
        CHECK(core::fill_keep(pl, 0, 0, 0, R) && core::fill_keep(pl, 5, 2, 2, R) && core::fill_keep(pl, 5, 3, 3, R) &&
              keepCount(pl, R) == 11 * 8, "overhang: eave 2 below the area edge, edge at the area's level");
        // thickness
        CHECK(core::plan_area(flat, 2, step1, 2000, R, fo(core::ROOF_GABLE, 0, 2), &pl) && pl.roofBase == 1 && pl.count[1] == 3 &&
              keepCount(pl, R) == core::fill_count(pl, R) && !core::fill_keep(pl, 0, 0, 1, R) && core::fill_keep(pl, 0, 1, 1, R) && core::fill_keep(pl, 0, 2, 1, R) &&
              core::fill_keep(pl, 0, 0, 0, R) && core::fill_keep(pl, 0, 1, 0, R),
              "roof thickness 2: each step two deep");
        core::I3 room[2] = {{0, 0, 0}, {9 * M, 9 * M, 9 * M}};
        CHECK(core::plan_area(room, 2, step1, 2000, core::FILL_HOLLOW, fo(0, 0, 2), &pl) &&
              core::fill_count(pl, core::FILL_HOLLOW) == 1000 - 6 * 6 * 6 && keepCount(pl, core::FILL_HOLLOW) == 1000 - 216, "hollow 2 thick");
        CHECK(core::fill_count(pl, core::FILL_WALLS) == 1000 - 6 * 10 * 6 && keepCount(pl, core::FILL_WALLS) == 1000 - 360, "walls 2 thick");
        // every column connected, keep == count, for every shape / turn / overhang / thickness
        bool okAll = true;
        core::I3 r[2] = {{0, 0, 0}, {9 * M, 3 * M, 6 * M}};
        for (int sh = 0; sh < core::ROOF_COUNT; sh++) for (int rt = 0; rt < 4; rt++) for (int t = 1; t <= 3; t++) for (int o = 0; o <= 3; o++)
            for (int flatToo = 0; flatToo < 2; flatToo++) {
                core::I3 c2[2] = { r[0], r[1] }; if (flatToo) c2[1].y = 0;
                if (!core::plan_area(c2, 2, step1, 20000, R, fo(sh, rt, t, o), &pl)) { okAll = false; continue; }
                for (int i = 0; i < pl.count[0]; i++) for (int k = 0; k < pl.count[2]; k++) {
                    int col = 0; for (int j = 0; j < pl.count[1]; j++) col += core::fill_keep(pl, i, j, k, R);
                    // round roofs: exactly the columns inside the outline (the area's ellipse widened by the overhang)
                    bool want = !core::roof_is_round(sh) || core::round_inside(core::ROUND_FULL, 0, i, k, pl.count[0], pl.count[2]);
                    if ((col >= 1) != want) okAll = false;
                }
                if (keepCount(pl, R) != core::fill_count(pl, R)) okAll = false;
            }
        CHECK(okAll, "every roof column holds a shape, keep matches count (all shapes and options)");
        // corner 2 below corner 1: the roof still rises upward
        core::I3 down[2] = {{0, 4 * M, 0}, {0, 0, 8 * M}};
        CHECK(core::plan_area(down, 2, step1, 2000, R, fo(core::ROOF_GABLE), &pl) && pl.stepFix[1] < 0, "downward area");
        CHECK(core::fill_keep(pl, 0, pl.count[1] - 1, 0, R) && core::fill_keep(pl, 0, 0, 4, R), "downward area: eaves at the bottom, ridge at the top");
        // pose packing
        bool poses = true;
        for (int sh = 0; sh < core::ROOF_COUNT; sh++) for (int rt = 0; rt < core::roof_turns(sh); rt++) {
            int s2, r2; int v = core::roof_pose_pack(sh, rt); core::roof_pose_unpack(v, &s2, &r2);
            if (v > 15 || s2 != sh || r2 != rt) poses = false;
        }
        CHECK(poses, "roof shape + turn fit 4 bits");
        // cone over a flat 9 x 9: a circle, 45 degrees, 5 high, peak in the middle
        core::I3 c9[2] = {{0, 0, 0}, {8 * M, 0, 8 * M}};
        CHECK(core::plan_area(c9, 2, step1, 2000, R, fo(core::ROOF_CONE), &pl) && pl.count[1] == 5 && pl.roofBase == 0 &&
              core::roof_turns(core::ROOF_CONE) == 1, "cone 9x9 -> 5 high");
        CHECK(core::fill_keep(pl, 4, 4, 4, R) && !core::fill_keep(pl, 4, 3, 4, R) && core::fill_keep(pl, 0, 0, 4, R) &&
              !core::fill_keep(pl, 0, 0, 0, R) && core::fill_keep(pl, 2, 2, 4, R), "cone: peak in the middle, round outline, empty corners");
        {   // same outline as the full round fill of the area (sits on round walls)
            core::Plan rp; core::FillOpts rf = {}; rf.thick = 1;
            core::plan_area(c9, 2, step1, 2000, core::FILL_ROUND, rf, &rp);
            bool same = true;
            for (int i = 0; i < 9; i++) for (int k = 0; k < 9; k++) {
                int col = 0; for (int j = 0; j < pl.count[1]; j++) col += core::fill_keep(pl, i, j, k, R);
                if ((col > 0) != core::fill_keep(rp, i, 0, k, core::FILL_ROUND)) same = false;
            }
            CHECK(same, "cone outline = round fill outline");
        }
        // the cone is a closed surface: every column's span reaches down to its lowest neighbour's top
        {
            bool closed = true;
            for (int i = 0; i < 9; i++) for (int k = 0; k < 9; k++) {
                int lo, hi; core::roof_column(pl, i, k, &lo, &hi);
                if (hi < lo) continue;
                const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                for (int q = 0; q < 4; q++) {
                    int x = i + nb[q][0], z = k + nb[q][1], lo2, hi2;
                    if (x < 0 || z < 0 || x >= 9 || z >= 9) continue;
                    core::roof_column(pl, x, z, &lo2, &hi2);
                    if (hi2 >= lo2 && (hi2 < lo - 1 || hi < lo2 - 1)) closed = false;
                }
            }
            CHECK(closed, "cone steps touch (no gaps)");
        }
        // dome over 9 x 9: steep at the eave (an edge column reaches up), round on top
        CHECK(core::plan_area(c9, 2, step1, 2000, R, fo(core::ROOF_DOME), &pl) && pl.count[1] == 5 && pl.roofBase == 0, "dome 9x9 -> 5 high");
        {
            int lo, hi; core::roof_column(pl, 4, 0, &lo, &hi);
            int loC, hiC; core::roof_column(pl, 4, 4, &loC, &hiC);
            CHECK(lo == 0 && hi >= 2 && hiC == 4, "dome: edge column from the eave up the steep side, top in the middle");
        }
        // overhang: cone slope carries on down, dome gets a flat brim
        CHECK(core::plan_area(c9, 2, step1, 2000, R, fo(core::ROOF_CONE, 0, 1, 2), &pl) && pl.count[0] == 13 && pl.roofBase >= 2,
              "cone overhang drops");
        CHECK(core::plan_area(c9, 2, step1, 2000, R, fo(core::ROOF_DOME, 0, 1, 2), &pl) && pl.count[0] == 13 && pl.roofBase == 0 &&
              core::fill_keep(pl, 6, 0, 0, R) && core::fill_keep(pl, 6, 0, 1, R) && !core::fill_keep(pl, 6, 1, 1, R), "dome overhang = level brim");
        // ellipse: rectangle area
        core::I3 el[2] = {{0, 0, 0}, {14 * M, 0, 6 * M}};
        CHECK(core::plan_area(el, 2, step1, 5000, R, fo(core::ROOF_DOME, 0, 2), &pl) && pl.count[1] == 5 &&
              keepCount(pl, R) == core::fill_count(pl, R), "dome over an ellipse");
        CHECK(core::plan_area(flat, 2, step1, 2000, core::FILL_SOLID, fo(core::ROOF_GABLE, 1, 1, 3), &pl) && pl.count[1] == 1 && pl.total == 28, "solid unchanged");
    }

    // round fills
    {
        const float step1[3] = { 1.f, 1.f, 1.f };
        const int O = core::FILL_ROUND;
        core::Plan pl;
        auto ro = [](int shape, int rot, int hollow, int thick = 1, int lying = 0) {
            core::FillOpts f = {}; f.thick = thick; f.roundShape = shape; f.roundRot = rot; f.roundHollow = hollow; f.roundLying = lying; return f;
        };
        auto keepCount = [&](const core::Plan& p) {
            long n = 0;
            for (int i = 0; i < p.count[0]; i++) for (int j = 0; j < p.count[1]; j++) for (int k = 0; k < p.count[2]; k++)
                n += core::fill_keep(p, i, j, k, O);
            return n;
        };
        // full circle 9 x 9, 2 high: corners out, centre and edge middles in, symmetric
        core::I3 sq[2] = {{0, 0, 0}, {8 * M, 1 * M, 8 * M}};
        CHECK(core::plan_area(sq, 2, step1, 5000, O, ro(core::ROUND_FULL, 0, 0), &pl) && pl.roundAxis == 1, "round: ground plan");
        CHECK(!core::fill_keep(pl, 0, 0, 0, O) && core::fill_keep(pl, 4, 0, 4, O) && core::fill_keep(pl, 0, 1, 4, O) &&
              core::fill_keep(pl, 4, 1, 8, O), "full circle: corners out, centre and sides in");
        bool sym = true;
        for (int i = 0; i < 9; i++) for (int k = 0; k < 9; k++)
            if (core::fill_keep(pl, i, 0, k, O) != core::fill_keep(pl, 8 - i, 0, k, O) ||
                core::fill_keep(pl, i, 0, k, O) != core::fill_keep(pl, k, 0, i, O)) sym = false;
        CHECK(sym, "full circle is symmetric");
        CHECK(core::fill_count(pl, O) == keepCount(pl), "full circle: count matches keep");
        // hollow ring: no centre, no gap a step can pass through (4-neighbour flood from the centre stays inside)
        for (int t = 1; t <= 3; t++) {
            core::I3 big[2] = {{0, 0, 0}, {14 * M, 0, 10 * M}};
            CHECK(core::plan_area(big, 2, step1, 5000, O, ro(core::ROUND_FULL, 0, 1, t), &pl), "ellipse 15 x 11 planned");
            const int nx = pl.count[0], nz = pl.count[2];
            CHECK(!core::fill_keep(pl, 7, 0, 5, O) && core::fill_keep(pl, 0, 0, 5, O) && core::fill_keep(pl, 7, 0, 0, O), "ring: centre empty, sides walled");
            CHECK(core::fill_keep(pl, t - 1, 0, 5, O) && !core::fill_keep(pl, t, 0, 5, O), "ring is `thick` shapes thick at the side");
            std::vector<int> seen(nx * nz, 0), st{ 7 + nx * 5 }; seen[7 + nx * 5] = 1; bool leak = false;
            while (!st.empty()) {
                int c = st.back(); st.pop_back(); int x = c % nx, z = c / nx;
                const int d[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                for (auto& e : d) {
                    int X = x + e[0], Z = z + e[1];
                    if (X < 0 || Z < 0 || X >= nx || Z >= nz) { leak = true; continue; }
                    if (seen[X + nx * Z] || core::fill_keep(pl, X, 0, Z, O)) continue;
                    seen[X + nx * Z] = 1; st.push_back(X + nx * Z);
                }
            }
            CHECK(!leak, "ring wall has no gaps");
            CHECK(core::fill_count(pl, O) == keepCount(pl), "ring: count matches keep");
        }
        // quarter: the curve runs from corner 1 to corner 2, also with corner 1 on the far side
        core::I3 q[2] = {{0, 0, 0}, {5 * M, 0, 5 * M}};
        CHECK(core::plan_area(q, 2, step1, 5000, O, ro(core::ROUND_QUARTER, 0, 1), &pl), "quarter planned");
        CHECK(core::fill_keep(pl, 0, 0, 0, O) && core::fill_keep(pl, 5, 0, 5, O) && !core::fill_keep(pl, 5, 0, 0, O) &&
              !core::fill_keep(pl, 1, 0, 4, O), "quarter wall from corner 1 to corner 2, open toward its centre");
        core::I3 qb[2] = {{5 * M, 0, 5 * M}, {0, 0, 0}};
        CHECK(core::plan_area(qb, 2, step1, 5000, O, ro(core::ROUND_QUARTER, 0, 1), &pl) &&
              core::fill_keep(pl, 0, 0, 0, O) && core::fill_keep(pl, 5, 0, 5, O) && !core::fill_keep(pl, 5, 0, 0, O), "quarter from the far corner");
        CHECK(core::plan_area(q, 2, step1, 5000, O, ro(core::ROUND_QUARTER, 1, 1), &pl) &&
              core::fill_keep(pl, 0, 0, 0, O) && core::fill_keep(pl, 5, 0, 5, O) && !core::fill_keep(pl, 0, 0, 5, O), "quarter turned bends the other way");
        CHECK(core::plan_area(q, 2, step1, 5000, O, ro(core::ROUND_QUARTER, 2, 0), &pl) &&
              core::fill_keep(pl, 0, 0, 0, O) && !core::fill_keep(pl, 5, 0, 5, O), "quarter turned twice: centre on corner 1");
        // half: flat side on the long side, open there when hollow
        core::I3 h[2] = {{0, 0, 0}, {10 * M, 0, 5 * M}};
        CHECK(core::plan_area(h, 2, step1, 5000, O, ro(core::ROUND_HALF, 0, 1), &pl) && pl.roundSide == 0, "half: flat on the long side");
        CHECK(core::fill_keep(pl, 0, 0, 0, O) && core::fill_keep(pl, 10, 0, 0, O) && core::fill_keep(pl, 5, 0, 5, O) &&
              !core::fill_keep(pl, 5, 0, 0, O) && !core::fill_keep(pl, 5, 0, 3, O), "half wall: ends and top in, flat side open");
        CHECK(core::plan_area(h, 2, step1, 5000, O, ro(core::ROUND_HALF, 1, 0), &pl) && pl.roundSide == 1 &&
              core::fill_keep(pl, 5, 0, 0, O) && core::fill_keep(pl, 0, 0, 5, O) && !core::fill_keep(pl, 0, 0, 0, O), "half turned: flat on the far side");
        // standing wall (one shape thin): the round stands in it, an arch with the flat side down
        core::I3 w[2] = {{0, 0, 0}, {8 * M, 4 * M, 0}};
        CHECK(core::plan_area(w, 2, step1, 5000, O, ro(core::ROUND_HALF, 0, 1), &pl) && pl.roundAxis == 2, "round in a standing wall");
        CHECK(core::fill_keep(pl, 0, 0, 0, O) && core::fill_keep(pl, 4, 4, 0, O) && !core::fill_keep(pl, 4, 0, 0, O) &&
              !core::fill_keep(pl, 0, 4, 0, O), "arch: feet and top in, opening below");
        core::I3 wd[2] = {{8 * M, 4 * M, 0}, {0, 0, 0}};
        CHECK(core::plan_area(wd, 2, step1, 5000, O, ro(core::ROUND_HALF, 0, 1), &pl) && core::fill_keep(pl, 4, 0, 0, O) &&
              !core::fill_keep(pl, 4, 4, 0, O), "arch from the top corner: still standing on the ground");
        // lying: a tube along the longer side, the round standing across it (half: a tunnel, flat side down)
        core::I3 tube[2] = {{0, 0, 0}, {9 * M, 4 * M, 8 * M}};
        CHECK(core::plan_area(tube, 2, step1, 5000, O, ro(core::ROUND_HALF, 0, 1, 1, 1), &pl) && pl.roundAxis == 0, "lying: along x");
        bool same = true;
        for (int i = 0; i < 10; i++) for (int j = 0; j < 5; j++) for (int k = 0; k < 9; k++)
            if (core::fill_keep(pl, i, j, k, O) != core::fill_keep(pl, 0, j, k, O)) same = false;
        CHECK(same && core::fill_keep(pl, 3, 0, 0, O) && core::fill_keep(pl, 3, 4, 4, O) && !core::fill_keep(pl, 3, 0, 4, O),
              "tunnel: the same cross-section all along, feet and top in, opening below");
        core::I3 flatTube[2] = {{0, 0, 0}, {4 * M, 0, 9 * M}};
        CHECK(core::plan_area(flatTube, 2, step1, 5000, O, ro(core::ROUND_FULL, 0, 0, 1, 1), &pl) && pl.roundAxis == 2, "lying: along z when z is longer");
        // every pose fits the state (as the roof fill with poses 10..15) and comes back
        bool ok = true;
        for (int sh = 0; sh < core::ROUND_COUNT; sh++)
            for (int rt = 0; rt < core::round_turns(sh); rt++)
                for (int hl = 0; hl < 2; hl++)
                    for (int ly = 0; ly < 2; ly++)
                        for (int t = 1; t <= 3; t++) {
                            core::FillOpts f = ro(sh, rt, hl, t, ly), back = {};
                            uint32_t bits = core::net_fill_bits(O, f);
                            if (core::net_fill_unpack(bits, &back) != O || back.roundShape != sh || back.roundRot != rt ||
                                back.roundHollow != hl || back.roundLying != ly || (hl && back.thick != t)) ok = false;
                            if (bits & ~((3u << core::NET_FILL_SHIFT) | (15u << core::NET_ROOF_SHIFT) | (3u << core::NET_OVERHANG_SHIFT) |
                                         (3u << core::NET_THICK_SHIFT))) ok = false;
                        }
        for (int sh = 0; sh < core::ROOF_COUNT; sh++)
            for (int rt = 0; rt < core::roof_turns(sh); rt++) {
                core::FillOpts f = {}, back = {}; f.roofShape = sh; f.roofRot = rt; f.overhang = 3; f.thick = 2;
                if (core::net_fill_unpack(core::net_fill_bits(core::FILL_ROOF, f), &back) != core::FILL_ROOF || back.roofShape != sh ||
                    back.roofRot != rt || back.overhang != 3 || back.thick != 2) ok = false;
            }
        for (int sh = core::ROOF_CONE; sh <= core::ROOF_DOME; sh++)
            for (int t = 1; t <= 3; t++)
                for (int o = 0; o <= 3; o++) {
                    core::FillOpts f = {}, back = {}; f.roofShape = sh; f.overhang = o; f.thick = t;
                    uint32_t bits = core::net_fill_bits(core::FILL_ROOF, f);
                    if (core::net_fill_unpack(bits, &back) != core::FILL_ROOF || back.roofShape != sh || back.roofRot != 0 ||
                        back.overhang != o || back.thick != t) ok = false;
                    if (bits & ~((3u << core::NET_FILL_SHIFT) | (15u << core::NET_ROOF_SHIFT) | (3u << core::NET_OVERHANG_SHIFT) |
                                 (3u << core::NET_THICK_SHIFT))) ok = false;
                }
        CHECK(ok, "round and roof settings round trip through the state");
    }

    // diagonal fills
    {
        const float step1[3] = { 1.f, 1.f, 1.f };
        const int O = core::FILL_DIAG;
        core::Plan pl;
        auto dg = [](int rot, int lying, int thick = 1) {
            core::FillOpts f = {}; f.thick = thick; f.diagRot = rot; f.diagLying = lying; return f;
        };
        // face-connected: all kept cells of the plane form one piece through shared faces
        auto connected = [](const core::Plan& p) {
            const int nu = p.count[p.diagU], nv = p.count[p.diagV];
            static int stack[4096]; static bool seen[4096];
            if (nu * nv > 4096) return false;
            int kept = 0, start = -1;
            for (int c = 0; c < nu * nv; c++) {
                seen[c] = false;
                if (core::diag_keep(p.diagFlip, p.thick, c % nu, c / nu, nu, nv)) { kept++; if (start < 0) start = c; }
            }
            if (start < 0) return false;
            int top = 0, found = 0; stack[top++] = start; seen[start] = true;
            while (top) {
                int c = stack[--top], u = c % nu, v = c / nu; found++;
                const int du[4] = { 1, -1, 0, 0 }, dv[4] = { 0, 0, 1, -1 };
                for (int k = 0; k < 4; k++) {
                    int a = u + du[k], b = v + dv[k];
                    if (!core::diag_keep(p.diagFlip, p.thick, a, b, nu, nv) || seen[a + nu * b]) continue;
                    seen[a + nu * b] = true; stack[top++] = a + nu * b;
                }
            }
            return found == kept;
        };
        // standing 8 x 8, 3 high: the wall runs from corner 1 to corner 2 in the ground plan
        core::I3 sq[2] = {{0, 0, 0}, {7 * M, 2 * M, 7 * M}};
        CHECK(core::plan_area(sq, 2, step1, 5000, O, dg(0, 0), &pl) && pl.diagU == 0 && pl.diagV == 2, "diag standing: ground plan");
        CHECK(core::fill_keep(pl, 0, 0, 0, O) && core::fill_keep(pl, 7, 2, 7, O) && !core::fill_keep(pl, 7, 0, 0, O) &&
              !core::fill_keep(pl, 0, 1, 7, O), "corner 1 to corner 2, not the other corners");
        CHECK(core::fill_count(pl, O) == 15 * 3, "45 degrees: a staircase of 15 cells per layer");
        CHECK(connected(pl), "no gaps along the staircase");
        CHECK(core::plan_area(sq, 2, step1, 5000, O, dg(1, 0), &pl) && core::fill_keep(pl, 7, 0, 0, O) &&
              core::fill_keep(pl, 0, 0, 7, O) && !core::fill_keep(pl, 0, 0, 0, O), "turned: the other diagonal");
        // corner 2 on the negative side: still from corner 1 to corner 2
        core::I3 neg[2] = {{0, 0, 0}, {-9 * M, 0, 4 * M}};
        CHECK(core::plan_area(neg, 2, step1, 5000, O, dg(0, 0), &pl) && core::fill_keep(pl, 0, 0, 0, O) &&
              core::fill_keep(pl, 9, 0, 4, O) && !core::fill_keep(pl, 9, 0, 0, O) && connected(pl), "mirrored area");
        long c1 = core::fill_count(pl, O);
        CHECK(c1 == 14, "10 x 5: 10 + 5 - 1 cells");
        CHECK(core::plan_area(neg, 2, step1, 5000, O, dg(0, 0, 3), &pl) && core::fill_count(pl, O) > c1 && connected(pl), "thicker");
        // thickness counts the slots of the band: brute force count matches fill_count
        {
            long n = 0;
            for (int i = 0; i < pl.count[0]; i++) for (int j = 0; j < pl.count[1]; j++) for (int k = 0; k < pl.count[2]; k++)
                n += core::fill_keep(pl, i, j, k, O);
            CHECK(n == core::fill_count(pl, O), "count matches the cells");
        }
        // standing wall one thin: a diagonal brace in it
        core::I3 w[2] = {{0, 0, 0}, {0, 4 * M, 8 * M}};
        CHECK(core::plan_area(w, 2, step1, 5000, O, dg(0, 0), &pl) && pl.diagU == 2 && pl.diagV == 1 &&
              core::fill_keep(pl, 0, 0, 0, O) && core::fill_keep(pl, 0, 4, 8, O) && !core::fill_keep(pl, 0, 4, 0, O), "brace in a wall");
        // lying: a ramp along the longer side; over a flat area 45 degrees, growing upward
        core::I3 flat[2] = {{0, 0, 0}, {5 * M, 0, 2 * M}};
        CHECK(core::plan_area(flat, 2, step1, 5000, O, dg(0, 1), &pl) && pl.diagU == 0 && pl.diagV == 1 && pl.count[1] == 6 &&
              core::fill_keep(pl, 0, 0, 0, O) && core::fill_keep(pl, 5, 5, 2, O) && !core::fill_keep(pl, 5, 0, 1, O), "ramp up along x");
        CHECK(core::plan_area(flat, 2, step1, 5000, O, dg(1, 1), &pl) && core::fill_keep(pl, 0, 5, 0, O) && core::fill_keep(pl, 5, 0, 2, O),
              "ramp turned: down");
        CHECK(core::plan_area(flat, 2, step1, 5000, O, dg(2, 1), &pl) && pl.diagU == 2 && pl.count[1] == 3, "ramp along the shorter side");
        core::I3 r3[2] = {{0, 0, 0}, {9 * M, 3 * M, 1 * M}};
        CHECK(core::plan_area(r3, 2, step1, 5000, O, dg(0, 1), &pl) && pl.count[1] == 4 && core::fill_count(pl, O) == 13 * 2 &&
              connected(pl), "3D area: ramp corner to corner, height kept");
        // any size, thickness and turn: no gaps, both ends kept
        bool all = true;
        for (int x = 0; x < 14; x++)
            for (int z = 0; z < 14; z++)
                for (int t = 1; t <= 3; t++)
                    for (int rt = 0; rt < 2; rt++) {
                        core::I3 c2[2] = {{0, 0, 0}, {x * M, 0, z * M}};
                        if (!core::plan_area(c2, 2, step1, 5000, O, dg(rt, 0, t), &pl) || !connected(pl)) all = false;
                        if (!core::fill_keep(pl, rt ? x : 0, 0, 0, O) || !core::fill_keep(pl, rt ? 0 : x, 0, z, O)) all = false;
                    }
        CHECK(all, "every size: gap-free, from corner to corner");
        // settings round trip
        bool ok = true;
        for (int ly = 0; ly < 2; ly++)
            for (int rt = 0; rt < core::diag_turns(ly); rt++)
                for (int t = 1; t <= 3; t++) {
                    core::FillOpts f = dg(rt, ly, t), back = {};
                    uint32_t bits = core::net_fill_bits(O, f);
                    if (core::net_fill_unpack(bits, &back) != O || back.diagRot != rt || back.diagLying != ly || back.thick != t) ok = false;
                }
        for (int sh = 0; sh < core::ROUND_COUNT; sh++)
            for (int rt = 0; rt < core::round_turns(sh); rt++) {
                core::FillOpts f = {}, back = {}; f.roundShape = sh; f.roundRot = rt; f.roundLying = 1;
                if (core::net_fill_unpack(core::net_fill_bits(core::FILL_ROUND, f), &back) != core::FILL_ROUND) ok = false;
            }
        CHECK(ok, "diagonal settings round trip through the state, rounds still rounds");
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

    // ----------------------------------------------------- client -> server state
    {
        const uint32_t s = core::NET_TAG | core::NET_ON | core::NET_AREA | (3u << core::NET_STYLE_SHIFT) |
                           (1u << core::NET_FILL_SHIFT) | (2u << core::NET_COUNT_SHIFT) | (5u << core::NET_SEQ_SHIFT) |
                           ((uint32_t)core::NET_VERSION << core::NET_VERSION_SHIFT);
        int64_t cam[3] = { 1234 * M + 987654321, -77 * M - 5, 3 * M };
        const int64_t orig[3] = { cam[0], cam[1], cam[2] };
        core::net_pack(s, cam);
        bool small = true;
        for (int a = 0; a < 3; a++) if (core::iabs64(cam[a] - orig[a]) >= 0x10000) small = false;
        CHECK(small, "state moves the camera by less than 2^16 units (0.015 mm)");
        uint32_t back = 0;
        CHECK(core::net_unpack(cam, &back) && back == s, "state round trip (negative coordinates too)");
        CHECK(core::net_field(back, core::NET_COUNT_SHIFT, 2) == 2 && core::net_field(back, core::NET_SEQ_SHIFT, 3) == 5 &&
              core::net_field(back, core::NET_STYLE_SHIFT, 2) == 3, "state fields");
        int64_t plain[3] = { orig[0], orig[1], orig[2] };
        CHECK(!core::net_unpack(plain, &back), "camera without mod data is rejected");
        int64_t bad[3] = { cam[0], cam[1], cam[2] ^ 1 };
        CHECK(!core::net_unpack(bad, &back), "damaged state is rejected");
        // random camera positions almost never pass
        uint64_t x = 88172645463325252ull; int pass = 0;
        for (int i = 0; i < 1000000; i++) {
            int64_t r[3];
            for (int a = 0; a < 3; a++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; r[a] = (int64_t)x; }
            if (core::net_unpack(r, &back)) pass++;
        }
        CHECK(pass <= 2, "random camera positions are rejected");
        // server corner index (2 corners)
        CHECK(core::net_corner_index(0, -1, 1, 1, 2) == 0, "first corner");
        CHECK(core::net_corner_index(1, 1, 2, 0, 2) == 1, "final corner");
        CHECK(core::net_corner_index(1, 2, 2, 0, 2) == -1, "repeated click ignored");
        CHECK(core::net_corner_index(0, 1, 2, 0, 2) == 0, "server missed corner 1: starts over");
        CHECK(core::net_corner_index(1, 2, 3, 1, 2) == 0, "client started over (cancel): server starts over");
        CHECK(core::net_corner_index(2, 2, 3, 0, 3) == 2, "3 corners: final");
    }

    printf(fails ? "\n%d FAILED\n" : "\nall tests passed\n", fails);
    return fails ? 1 : 0;
}
