// Blueprints (issue #49): save the blocks of an area to a file, build them again elsewhere.
// Included by main.cpp after the area code; the windows are in blueprint_ui.h.
//
// Blueprint is a third mode next to Prefab and Area, with two actions (tools wheel): Save
// and Place. Both start with one left click, which opens a window on the exact window's
// screen (its right-button hiding, the drawn box beside it, Build sending a real click).
//
//   Save   - the clicked block is the reference of a box set by width / depth / height in
//            the window (core::exact_corners, as Exact mode); a name can be typed. Save
//            sends the click: every cell of the box (the size of the shape in hand) is
//            probed with the mod's removal dry run (price_removal: RemoveShape with refunds
//            noted, not added), which reports the volume of every building material in
//            it. A cell is saved with its main material when that fills at least half of it.
//            The material table names each material by its item id (from the game's refund
//            builder, 48ae10), so files survive game patches that renumber material bytes.
//   Place  - a list of the saved blueprints, then a page with the costs, the rotation and
//            the reference: the ghost follows the aimed block (behind the window: hold the
//            right button) until Place here fixes it; buttons move it by one cell. Build
//            sends the click; the place hook prices one shape per saved cell with that
//            cell's material item (PlaceShape takes the material from its 4th argument, the
//            material's item definition; see bp_find_item), and the commit hook writes every
//            shape with the replace blit's force byte set to its material (the commit itself
//            builds the material picked in the game). Undo budget and partial build as Area.
// Building pieces and objects are not saved yet; a dedicated server cannot use blueprints yet.

static const long kBpMaxCells = 65536;          // a save may hold this many cells (MaxShapes limits building)
static const int kBpMaxFiles = 64;
enum { BP_SAVE = 0, BP_PLACE = 1 };              // g_bpSub
enum { BPW_NONE = 0, BPW_SAVE = 1, BPW_PLACE = 2 };   // g_bpWin
struct Blueprint {
    bool ok; core::BpHeader h; uint8_t cell[kBpMaxCells]; int filled;
    wchar_t path[MAX_PATH];
};
static Blueprint g_bp;                           // the one picked in the window (UI thread; the server reads it once armed)
static volatile LONG g_bpSub = BP_SAVE;          // what a click in Blueprint mode does
static volatile LONG g_bpRot = 0;                // quarter turns about the vertical axis
static volatile LONG g_bpGhost = 0;              // the drawn box is the blueprint (vkdraw: its cells from g_bpBits)
static volatile LONG g_bpPage = 0;               // place window: 0 = the list, 1 = the chosen blueprint
static volatile LONG g_bpPlaced = 0;             // the ghost stays at g_bpAnchor (else it follows the aim)
static I3 g_bpAnchor;                            // the reference cell's centre while placed
static I3 g_bpMinArm;                            // Build: centre of the turned blueprint's low cell
static uint32_t g_bpBits[1250];                  // the blueprint's cells in the drawn box (turned), one bit each
static volatile LONG g_bpBitsOk = 0;
static wchar_t g_bpFile[kBpMaxFiles][MAX_PATH];  // file names (no folder) in the list, sorted by name
static char g_bpName[kBpMaxFiles][96];           // the same as UTF-8, without ".btbp"
static int g_bpFiles = 0;
static LONG g_bpSel = 0;
static int g_bpOff[3];                           // place window: the ghost moved by cells (right, away, up as the player faces)
static int g_bpSaveOff[3];                       // save window: the reference point moved by cells (right, away, up)
static char g_bpTyped[64];                       // save window: the name typed so far (UTF-8)
static volatile LONG g_bpTypedLen = 0;
static volatile LONG g_bpNameEdit = 0;         // the name was clicked: keys type into it (until Enter or another click)
static bool bp_tool() { return g_cfg.toolBlueprint != 0; }
static bool remote_session();

static void bp_dir(wchar_t* out) {
    lstrcpyW(out, g_dir); lstrcatW(out, L"BuildingTools-Blueprints\\");
}
static void bp_utf8(const wchar_t* w, char* out, int cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr);
    if (n <= 0) out[0] = 0; else out[cap - 1] = 0;
}

// ---- turned dimensions and positions (rotation r = quarter turns)
static void bp_dims(int r, int* rn, float* rs) {
    const core::BpHeader& h = g_bp.h;
    const bool odd = (r & 1) != 0;
    rn[0] = odd ? h.n[2] : h.n[0]; rn[1] = h.n[1]; rn[2] = odd ? h.n[0] : h.n[2];
    rs[0] = odd ? h.step[2] : h.step[0]; rs[1] = h.step[1]; rs[2] = odd ? h.step[0] : h.step[2];
}
static int64_t bp_fix(float m) {               // metres -> fixed point, snapped to 1/16 m
    double k = (double)(int64_t)((double)m * 16.0 + 0.5);
    return (int64_t)(k * (core::kFix / 16.0));
}
// Centre of cell (ri, j, rk) of the turned blueprint whose low cell is at mn.
static I3 bp_cell_pos(const I3& mn, const float* rs, int ri, int j, int rk) {
    return I3{ mn.x + (int64_t)ri * bp_fix(rs[0]), mn.y + (int64_t)j * bp_fix(rs[1]), mn.z + (int64_t)rk * bp_fix(rs[2]) };
}
// The low cell of the turned blueprint when its reference cell (g_cfg.bpPlaceRef, as the
// player faces) is at `anchor`.
static I3 bp_min_from(const I3& anchor) {
    int rn[3]; float rs[3]; bp_dims(g_bpRot, rn, rs);
    const int w = rn[g_exRAxis] - 1, d = rn[g_exFAxis] - 1, h = rn[1] - 1;
    I3 c[2];
    core::exact_corners(anchor, g_cfg.bpPlaceRef, w, d, h, g_exFAxis, g_exFSign, g_exRAxis, g_exRSign, rs, c);
    return I3{ c[0].x < c[1].x ? c[0].x : c[1].x, c[0].y < c[1].y ? c[0].y : c[1].y, c[0].z < c[1].z ? c[0].z : c[1].z };
}
// p moved by off cells (right, away, up as the player faced at the click) of size step
static I3 bp_shift(const I3& p, const int* off, const float* step) {
    int64_t v[3] = { p.x, p.y, p.z };
    v[g_exRAxis] += (int64_t)g_exRSign * off[0] * bp_fix(step[g_exRAxis]);
    v[g_exFAxis] += (int64_t)g_exFSign * off[1] * bp_fix(step[g_exFAxis]);
    v[1] += (int64_t)off[2] * bp_fix(step[1]);
    return I3{ v[0], v[1], v[2] };
}
// The save box's corners: Exact mode's rule from the clicked block moved by the window's offsets.
static void bp_save_corners(I3 out[2]) {
    I3 p = bp_shift(g_scPos[0], g_bpSaveOff, g_scStep);
    core::exact_corners(p, g_cfg.bpRef, g_cfg.bpW, g_cfg.bpD, g_cfg.bpH, g_exFAxis, g_exFSign, g_exRAxis, g_exRSign, g_scStep, out);
}
static bool live_centre(I3* out);                // main.cpp: the aimed shape's centre now
// The reference cell now: fixed, or where the player aims (the click's spot until the aim moves).
static I3 bp_aim_now() {                        // fixed, or the aimed block (the click's until the aim moves)
    if (g_bpPlaced) return g_bpAnchor;
    I3 a;
    if (!g_exactArmed && live_centre(&a)) return a;
    return g_scPos[0];
}
static I3 bp_anchor_now() {                      // ... moved by the window's offsets
    int rn[3]; float rs[3]; bp_dims(g_bpRot, rn, rs);
    return bp_shift(bp_aim_now(), g_bpOff, rs);
}
// the drawn box of the blueprint being placed (fixed point)
static bool bp_box(int64_t lo[3], int64_t hi[3], float step[3]) {
    if (!g_bp.ok || g_bpWin != BPW_PLACE || g_bpPage != 1) return false;
    int rn[3]; float rs[3]; bp_dims(g_bpRot, rn, rs);
    I3 mn = g_exactArmed ? g_bpMinArm : bp_min_from(bp_anchor_now());
    I3 b = bp_cell_pos(mn, rs, rn[0] - 1, rn[1] - 1, rn[2] - 1);
    const int64_t pa[3] = { mn.x, mn.y, mn.z }, pb[3] = { b.x, b.y, b.z };
    for (int k = 0; k < 3; k++) {
        int64_t half = bp_fix(rs[k]) / 2;
        lo[k] = pa[k] - half; hi[k] = pb[k] + half; step[k] = rs[k];
    }
    return true;
}
// the turned cells as bits for the drawn box (UI thread, when the blueprint or the turn changes)
static void bp_ghost_bits() {
    static LONG key = -1; static uint32_t keyFile = 0;
    uint32_t f = 0; for (int q = 0; g_bp.path[q]; q++) f = f * 31 + g_bp.path[q];
    if (key == g_bpRot && keyFile == f && g_bpBitsOk) return;
    g_bpBitsOk = 0;
    key = g_bpRot; keyFile = f;
    if (!g_bp.ok) return;
    int rn[3]; float rs[3]; bp_dims(g_bpRot, rn, rs);
    if ((long)rn[0] * rn[1] * rn[2] > 40000) return;       // too many cells for the bits: the box alone
    memset(g_bpBits, 0, sizeof g_bpBits);
    const int* n = g_bp.h.n;
    for (int j = 0; j < n[1]; j++)
        for (int k = 0; k < n[2]; k++)
            for (int i = 0; i < n[0]; i++) {
                if (g_bp.cell[core::bp_index(n, i, j, k)] == core::BP_EMPTY) continue;
                int ri, rk; core::bp_turn(g_bpRot, i, k, n[0], n[2], &ri, &rk);
                int id = ri + rn[0] * (j + rn[1] * rk);
                g_bpBits[id >> 5] |= 1u << (id & 31);
            }
    g_bpBitsOk = 1;
}

// ---- files
static bool bp_load(const wchar_t* name) {
    wchar_t p[MAX_PATH]; bp_dir(p); lstrcatW(p, name);
    g_bp.ok = false; g_bpBitsOk = 0;
    lstrcpynW(g_bp.path, p, MAX_PATH);
    DWORD size = 0;
    char* d = read_file(p, &size);
    if (!d) { LOG(.str("blueprint: cannot read the file")); return false; }
    bool ok = core::bp_decode((const uint8_t*)d, (int)size, &g_bp.h, g_bp.cell, kBpMaxCells);
    HeapFree(GetProcessHeap(), 0, d);
    if (!ok) { LOG(.str("blueprint: not a readable blueprint (").u(size).str(" bytes)")); return false; }
    long total = (long)g_bp.h.n[0] * g_bp.h.n[1] * g_bp.h.n[2];
    g_bp.filled = 0;
    for (long q = 0; q < total; q++) if (g_bp.cell[q] != core::BP_EMPTY) g_bp.filled++;
    g_bp.ok = true;
    char u[96]; bp_utf8(name, u, sizeof u);
    LOG(.str("blueprint: loaded ").str(u).c(' ').i(g_bp.h.n[0]).c('x').i(g_bp.h.n[1]).c('x').i(g_bp.h.n[2])
        .str(" cells of ").f(g_bp.h.step[0]).str(" m, ").i(g_bp.filled).str(" blocks, ").i(g_bp.h.nm).str(" materials"));
    return true;
}
// The blueprint files, by name, into the window's list.
static void bp_list_files() {
    wchar_t p[MAX_PATH]; bp_dir(p); lstrcatW(p, L"*.btbp");
    WIN32_FIND_DATAW fd;
    g_bpFiles = 0;
    HANDLE h = FindFirstFileW(p, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (lstrlenW(fd.cFileName) >= MAX_PATH - 1) continue;
            int at = 0;
            while (at < g_bpFiles && lstrcmpiW(g_bpFile[at], fd.cFileName) < 0) at++;
            if (at >= kBpMaxFiles) continue;
            int last = g_bpFiles < kBpMaxFiles ? g_bpFiles : kBpMaxFiles - 1;
            for (int q = last; q > at; q--) lstrcpyW(g_bpFile[q], g_bpFile[q - 1]);
            lstrcpyW(g_bpFile[at], fd.cFileName);
            if (g_bpFiles < kBpMaxFiles) g_bpFiles++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (int q = 0; q < g_bpFiles; q++) {
        bp_utf8(g_bpFile[q], g_bpName[q], sizeof g_bpName[q]);
        int n = lstrlenA(g_bpName[q]);
        if (n > 5) g_bpName[q][n - 5] = 0;          // ".btbp"
    }
    if (g_bpSel >= g_bpFiles) g_bpSel = 0;
}
// Writes the blueprint under the typed name (characters Windows does not allow in file
// names become '_'), or the date and time; " (2)", " (3)"... when the name is taken.
static bool bp_write(const core::BpHeader& h, const uint8_t* cells, const char* typed, char* nameOut, int cap) {
    static uint8_t buf[16 + 12 + 6 + 4 + 2 + core::BP_MAX_MATS * 16 + 4 + kBpMaxCells * 4 + 64];
    int size = core::bp_encode(h, cells, buf, sizeof buf);
    if (!size) return false;
    wchar_t dir[MAX_PATH]; bp_dir(dir);
    CreateDirectoryW(dir, nullptr);
    wchar_t base[80]; int bn = 0;
    if (typed && typed[0]) {
        bn = MultiByteToWideChar(CP_UTF8, 0, typed, -1, base, 64);
        bn = bn > 0 ? bn - 1 : 0;
        for (int q = 0; q < bn; q++) {
            wchar_t c = base[q];
            if (c < 32 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') base[q] = '_';
        }
        while (bn > 0 && (base[bn - 1] == ' ' || base[bn - 1] == '.')) bn--;   // Windows drops them
    }
    if (!bn) {
        SYSTEMTIME t; GetLocalTime(&t);
        Buf b; b.str("Blueprint ").u(t.wYear, 10, 4).c('-').u(t.wMonth, 10, 2).c('-').u(t.wDay, 10, 2).c(' ')
            .u(t.wHour, 10, 2).c('-').u(t.wMinute, 10, 2).c('-').u(t.wSecond, 10, 2);
        const char* d = b.z();
        for (bn = 0; d[bn] && bn < 60; bn++) base[bn] = (wchar_t)(uint8_t)d[bn];
    }
    base[bn] = 0;
    for (int k = 1; k < 100; k++) {
        wchar_t p[MAX_PATH]; lstrcpyW(p, dir); lstrcatW(p, base);
        wchar_t name[96]; lstrcpyW(name, base);
        if (k > 1) { wchar_t sfx[8] = { ' ', '(', (wchar_t)(k < 10 ? '0' + k : '0' + k / 10), 0 }; int s = 3;
                     if (k >= 10) sfx[s++] = (wchar_t)('0' + k % 10); sfx[s++] = ')'; sfx[s] = 0;
                     lstrcatW(p, sfx); lstrcatW(name, sfx); }
        lstrcatW(p, L".btbp");
        HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { if (GetLastError() == ERROR_FILE_EXISTS) continue; return false; }
        DWORD w = 0; BOOL ok = WriteFile(f, buf, (DWORD)size, &w, nullptr);
        CloseHandle(f);
        bp_utf8(name, nameOut, cap);
        return ok && w == (DWORD)size;
    }
    return false;
}

// ---- save (server, the Save click; g_stateLock held)
// The box: corners fixed at Save (g_exactC). Each cell is probed with a removal dry run in
// the placement context (as Replace measures what it may repaint).
static uint8_t bp_save_box(void* ctx, void* state, const CursorXf& tmpl) {
    Aabb box; shape_bounds(state, &tmpl, &box);
    float step[3] = { box.mx[0] - box.mn[0], box.mx[1] - box.mn[1], box.mx[2] - box.mn[2] };
    I3 c[3] = { g_exactC[0], g_exactC[1], g_exactC[1] };
    core::Plan pl = {};
    const long cap = kBpMaxCells;
    bool ok = core::plan_fill(c, 2, step, 50000000, &pl) && pl.total <= cap;
    LOG(.str("blueprint save: step=(").f(step[0]).c(',').f(step[1]).c(',').f(step[2]).str(") counts=")
        .i(pl.count[0]).c('x').i(pl.count[1]).c('x').i(pl.count[2]).str(ok ? "" : " REFUSED (too big)"));
    if (!ok) { Buf b; b.fmt(T(S_TOO_BIG), num(pl.total).s, num(cap).s); flash(b.z()); beep(MB_ICONHAND); return 0; }
    bp_probe_request(g_exactC, step, ctx, state);                  // stage 2 diagnostic: the props in the box go to the log
    static core::BpHeader h; memset(&h, 0, sizeof h);
    static uint8_t cells[kBpMaxCells];
    for (int a = 0; a < 3; a++) { h.n[a] = pl.count[a]; h.step[a] = step[a]; }
    h.shape = state ? *(uint32_t*)state : 0;
    const long total = (long)h.n[0] * h.n[1] * h.n[2];
    memset(cells, core::BP_EMPTY, (size_t)total);
    int vx[3]; for (int a = 0; a < 3; a++) { vx[a] = (int)(step[a] * 2.f + 0.5f); if (vx[a] < 1) vx[a] = 1; }
    const float vox = (float)(vx[0] * vx[1] * vx[2]);       // building voxels in a full cell (0.5 m each)
    int matOf[256]; for (int m = 0; m < 256; m++) matOf[m] = -1;
    const DWORD t0 = GetTickCount();
    const I3 tc = box_centre(box);
    const int64_t off[3] = { tmpl.pos[0] - tc.x, tmpl.pos[1] - tc.y, tmpl.pos[2] - tc.z };
    static CostEntry scratchBuf[256];
    int filled = 0, partial = 0;
    for (int j = 0; j < pl.count[1]; j++)
        for (int i = 0; i < pl.count[0]; i++)
            for (int k = 0; k < pl.count[2]; k++) {
                I3 q = core::plan_pos(pl, i, j, k);
                CursorXf xf = tmpl;
                xf.pos[0] = q.x + off[0]; xf.pos[1] = q.y + off[1]; xf.pos[2] = q.z + off[2];
                CostList scratch = { scratchBuf, 0, 256, nullptr };
                memset(g_remVols, 0, sizeof(g_remVols));
                price_removal(&scratch, ctx, state, &xf);
                int best = 0; float bv = 0.f;
                for (int m = 0x80; m < 0x100; m++) if (g_remVols[m] > bv) { bv = g_remVols[m]; best = m; }
                if (!best) continue;
                if (bv < vox * 0.5f) { partial++; continue; }
                if (matOf[best] < 0) {
                    if (h.nm >= core::BP_MAX_MATS) continue;
                    matOf[best] = h.nm; h.m[h.nm].mat = (uint8_t)best; h.nm++;
                }
                // box space: counted from the low corner whatever way the box was set
                const int bi = pl.stepFix[0] >= 0 ? i : pl.count[0] - 1 - i;
                const int bj = pl.stepFix[1] >= 0 ? j : pl.count[1] - 1 - j;
                const int bk = pl.stepFix[2] >= 0 ? k : pl.count[2] - 1 - k;
                cells[core::bp_index(h.n, bi, bj, bk)] = (uint8_t)matOf[best];
                h.m[matOf[best]].cells++;
                filled++;
            }
    g_refundN = 0; g_refundList = nullptr;              // the probes queue no refund
    // the item of every material (the game's refund builder) and what building it costs
    void* reg = *(void**)((uint8_t*)ctx + 0xf8);
    for (int q = 0; q < h.nm; q++) {
        static uint32_t cnt[256]; memset(cnt, 0, sizeof cnt);
        cnt[h.m[q].mat] = 4096;
        CostEntry e[4];
        int ne = build_refund_entries(ctx, cnt, e, 4);
        h.m[q].item = ne > 0 ? *(uint32_t*)(e[0].b + 0x10) : 0;
        uint32_t per = g_materialItem && reg ? g_materialItem(reg, h.m[q].mat, vox) : 0;
        h.m[q].items = per * h.m[q].cells;
        LOG(.str("  material ").x(h.m[q].mat).str(" item ").x(h.m[q].item).str(" cells ").u(h.m[q].cells).str(" items ").u(h.m[q].items));
    }
    LOG(.str("blueprint save: ").i(filled).str(" blocks, ").i(partial).str(" cells less than half full left out, ")
        .i(h.nm).str(" materials, probed in ").u(GetTickCount() - t0).str(" ms"));
    if (!filled) { flash(T(S_BP_EMPTY)); beep(MB_ICONHAND); return 0; }
    char name[96];
    if (!bp_write(h, cells, g_bpTyped, name, sizeof name)) { flash(T(S_BP_WRITE_FAIL)); beep(MB_ICONHAND); LOG(.str("blueprint save: write failed ").u(GetLastError())); return 0; }
    Buf b; b.fmt(T(S_BP_SAVED), name, num(filled).s, num(h.nm).s); flash(b.z()); beep(MB_ICONASTERISK);
    LOG(.str("blueprint saved: ").str(name));
    g_bpTyped[0] = 0; g_bpTypedLen = 0; g_bpNameEdit = 0;
    return 0;
}

// ---- build (server, the Build click; g_stateLock held)
// The cost entry template (first 0x14 bytes: item reference and id) of a material, from the
// game's refund builder in this context. False = the game has no item for that material.
static bool bp_entry(void* ctx, int m, CostEntry* out) {
    static uint32_t cnt[256]; memset(cnt, 0, sizeof cnt);
    cnt[m] = 4096;
    CostEntry e[4];
    if (build_refund_entries(ctx, cnt, e, 4) < 1) return false;
    *out = e[0];
    return true;
}
// Material byte and cost entry template for each material of the blueprint (by item id).
static int bp_map(void* ctx, int* mat, CostEntry* tmpl) {
    int missing = 0;
    for (int q = 0; q < g_bp.h.nm; q++) {
        const core::BpMat& bm = g_bp.h.m[q];
        mat[q] = -1;
        CostEntry e;
        if (bm.mat >= 0x80 && bp_entry(ctx, bm.mat, &e) && (!bm.item || *(uint32_t*)(e.b + 0x10) == bm.item)) { mat[q] = bm.mat; tmpl[q] = e; }
        else if (bm.item)
            for (int m = 0x80; m < 0x100 && mat[q] < 0; m++)
                if (bp_entry(ctx, m, &e) && *(uint32_t*)(e.b + 0x10) == bm.item) { mat[q] = m; tmpl[q] = e; }
        if (mat[q] < 0) missing++;
        LOG(.str("  blueprint material ").i(q).str(": item ").x(bm.item).str(" saved as ").x(bm.mat).str(" -> ")
            .str(mat[q] < 0 ? "not found" : "").x(mat[q] < 0 ? 0 : mat[q]));
    }
    return missing;
}
// PlaceShape prices the material of its 4th argument: the selected material's item
// definition (+0 item id, +4 the 16 bytes the cost entry copies, +0x454 the voxel material
// byte), read-only game data (writing it crashed in 0.32.0-dev1). The mod finds the item
// definition of every blueprint material in the same block of memory (item id at +0 and
// material byte at +0x454 both match). Read-only scan, cached per item.
struct BpItem { uint32_t id; uint8_t mat; uint8_t* p; };
static BpItem g_bpItemCache[128]; static int g_bpItemN = 0;
static uint8_t* bp_find_item(uint8_t* pdata, uint32_t id, uint8_t mat) {
    auto match = [&](const uint8_t* c) { return *(const uint32_t*)c == id && (mat ? c[0x454] == mat : c[0x454] >= 0x80); };
    if (match(pdata)) return pdata;
    for (int q = 0; q < g_bpItemN; q++) if (g_bpItemCache[q].id == id && g_bpItemCache[q].mat == mat) return g_bpItemCache[q].p;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(pdata, &mi, sizeof mi)) return nullptr;
    uint8_t* base = (uint8_t*)mi.AllocationBase;
    uint8_t* found = nullptr; uint64_t scanned = 0;
    const DWORD t0 = GetTickCount();
    for (uint8_t* r = base; !found && VirtualQuery(r, &mi, sizeof mi) && mi.AllocationBase == base; r = (uint8_t*)mi.BaseAddress + mi.RegionSize) {
        const DWORD okProt = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
        if (mi.State != MEM_COMMIT || !(mi.Protect & okProt) || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) continue;
        uint8_t* lo = (uint8_t*)mi.BaseAddress; uint8_t* hi = lo + mi.RegionSize;
        if (hi - lo < 0x460) continue;
        scanned += mi.RegionSize;
        for (uint8_t* c = lo + (((uintptr_t)pdata - (uintptr_t)lo) & 7); c + 0x460 <= hi; c += 8)
            if (match(c)) { found = c; break; }
        if (scanned > (512ull << 20)) break;              // never more than 512 MB
    }
    LOG(.str("  blueprint: item ").x(id).str(" material ").x(mat).str(found ? " found" : " NOT found").str(" (scanned ")
        .u(scanned >> 20).str(" MB in ").u(GetTickCount() - t0).str(" ms)"));
    if (found && g_bpItemN < 128) g_bpItemCache[g_bpItemN++] = BpItem{ id, mat, found };
    return found;
}
static uint8_t g_bpMatByte[core::BP_MAX_MATS];   // the build's material byte per blueprint material (commit: cave force)
static uint8_t bp_commit_material(int i) {       // main.cpp's commit hook, per queued shape
    uint8_t mi = g_pendMat[i];
    return mi < core::BP_MAX_MATS ? g_bpMatByte[mi] : 0;
}
static uint8_t bp_fill(CostList* list, void* ctx, void* state, uint8_t* pdata, const CursorXf& target) {
    if (!g_bp.ok || !pdata) { flash(T(S_BP_NOTHING)); beep(MB_ICONHAND); return 0; }
    Aabb box; shape_bounds(state, &target, &box);
    int rn[3]; float rs[3]; bp_dims(g_bpRot, rn, rs);
    for (int a = 0; a < 3; a++) {
        float held = box.mx[a] - box.mn[a];
        if (held - rs[a] > 0.05f || rs[a] - held > 0.05f) {
            Buf b; b.fmt(T(S_BP_SHAPE), tenths((int)(rs[0] * 10.f + 0.5f)).s, tenths((int)(rs[1] * 10.f + 0.5f)).s, tenths((int)(rs[2] * 10.f + 0.5f)).s);
            flash(b.z()); beep(MB_ICONHAND);
            LOG(.str("blueprint build: the held shape is ").f(box.mx[0] - box.mn[0]).c('x').f(box.mx[1] - box.mn[1]).c('x').f(box.mx[2] - box.mn[2])
                .str(", the blueprint's cells ").f(rs[0]).c('x').f(rs[1]).c('x').f(rs[2]));
            return 0;
        }
    }
    if (!g_caveForce) { LOG(.str("blueprint build: no blit patch")); return 0; }
    LOG(.str("  blueprint: material in hand item ").x(*(uint32_t*)pdata).str(" material ").x(pdata[0x454]));
    // Each material by its saved item id, straight from the game's item data (the same in
    // every world; the refund builder gave nothing for materials not found yet in a new
    // world, 0.32.0-dev4). Only a file without item ids goes through the refund builder.
    static int mat[core::BP_MAX_MATS]; static CostEntry tmpl[core::BP_MAX_MATS]; static uint8_t* item[core::BP_MAX_MATS];
    int missing = 0; bool mapped = false;
    for (int q = 0; q < g_bp.h.nm; q++) {
        const core::BpMat& bm = g_bp.h.m[q];
        item[q] = nullptr; g_bpMatByte[q] = 0; mat[q] = -1;
        if (bm.item) {
            item[q] = bp_find_item(pdata, bm.item, bm.mat);                     // the saved material byte
            if (!item[q]) item[q] = bp_find_item(pdata, bm.item, 0);           // the game renumbered it
        } else {
            if (!mapped) { bp_map(ctx, mat, tmpl); mapped = true; }
            if (mat[q] >= 0) item[q] = bp_find_item(pdata, *(uint32_t*)(tmpl[q].b + 0x10), (uint8_t)mat[q]);
        }
        if (!item[q]) { missing++; mat[q] = -1; LOG(.str("  blueprint material ").i(q).str(": item ").x(bm.item).str(" not found")); continue; }
        mat[q] = item[q][0x454];
        g_bpMatByte[q] = (uint8_t)mat[q];
        LOG(.str("  blueprint material ").i(q).str(": item ").x(bm.item).str(" saved as ").x(bm.mat).str(" -> ").x(mat[q]));
    }
    if (missing) { Buf b; b.fmt(T(S_BP_NO_MATERIAL), num(missing).s); flash(b.z()); }
    const uint64_t listBefore = list->count;
    pend_begin(ctx, target, false, state ? *(uint32_t*)state : 0);
    g_pend.bp = true;
    const I3 tc = box_centre(box);                       // positions are centres; the dry run takes the cursor
    const int64_t off[3] = { target.pos[0] - tc.x, target.pos[1] - tc.y, target.pos[2] - tc.z };
    CursorXf xf = target;
    const int* n = g_bp.h.n;
    const I3 mn = g_bpMinArm;
    long priced = 0; int logged = 0;
    for (int j = 0; j < rn[1]; j++)                      // bottom layer first
        for (int rk = 0; rk < rn[2]; rk++)
            for (int ri = 0; ri < rn[0]; ri++) {
                int i, k; core::bp_unturn(g_bpRot, ri, rk, n[0], n[2], &i, &k);
                const uint8_t mi = g_bp.cell[core::bp_index(n, i, j, k)];
                if (mi == core::BP_EMPTY || mi >= g_bp.h.nm || mat[mi] < 0) continue;
                if (g_pend.n >= kMaxPending) continue;
                I3 p = bp_cell_pos(mn, rs, ri, j, rk);
                xf.pos[0] = p.x + off[0]; xf.pos[1] = p.y + off[1]; xf.pos[2] = p.z + off[2];
                g_pendMat[g_pend.n] = mi;
                const uint64_t before = list->count;
                if (price_shape(list, ctx, state, item[mi], &xf, false)) priced++;
                CostEntry* e = (CostEntry*)list->data;
                for (uint64_t q = before; q < list->count && logged < 4; q++) {
                    logged++;
                    Buf lb; lb.str("  cost entry of ").x(mat[mi]).str(": ");
                    for (int b = 0; b < 0x18; b++) lb.u(e[q].b[b], 16, 2);
                    log_line(lb);
                }
            }
    g_pendCost[g_pend.n] = (uint32_t)list->count;
    LOG(.str("blueprint build: turn ").i(g_bpRot).str(", priced ").i(priced).str(" of ").i(g_bp.filled)
        .str(" blocks, cost entries ").u(list->count - listBefore));
    if (!priced) { g_pend.n = 0; g_pend.bp = false; list->count = listBefore; flash(T(S_BP_NOTHING)); beep(MB_ICONHAND); return 0; }
    // Partial build: a blueprint keeps every block the inventory can pay for, material by
    // material (bottom layer first), instead of stopping at the first block it cannot (Area
    // mode's rule left 14 of 1185 blocks when one of 7 materials ran short).
    if (!creative_on() && g_cfg.bpPartial && g_pend.n > 1 && stock_known(list) && !affordable(nullptr, list, (uint32_t)list->count)) {
        struct Budget { uint32_t item; int64_t left; } bud[64]; int nb = 0;
        CostEntry* e = (CostEntry*)list->data;
        auto slot = [&](uint32_t item) -> Budget* {
            for (int q = 0; q < nb; q++) if (bud[q].item == item) return &bud[q];
            if (nb == 64) return nullptr;
            bud[nb] = Budget{ item, stock_of(item) }; return &bud[nb++];
        };
        const int n0 = g_pend.n;
        int keep = 0; uint32_t out = g_pendCost[0];
        for (int i = 0; i < n0; i++) {
            const uint32_t a0 = g_pendCost[i], a1 = g_pendCost[i + 1];
            bool fits = true;
            for (uint32_t q = a0; q < a1 && fits; q++) {
                Budget* s = slot(*(uint32_t*)(e[q].b + 0x10));
                if (!s) { fits = false; break; }
                int64_t need = 0;                          // this shape's entries of the same item together
                for (uint32_t r = a0; r < a1; r++) if (*(uint32_t*)(e[r].b + 0x10) == s->item) need += *(uint32_t*)(e[r].b + 0x14);
                fits = s->left >= need;
            }
            if (!fits) continue;
            for (uint32_t q = a0; q < a1; q++) { Budget* s = slot(*(uint32_t*)(e[q].b + 0x10)); s->left -= *(uint32_t*)(e[q].b + 0x14); }
            memmove(&e[out], &e[a0], (size_t)(a1 - a0) * sizeof(CostEntry));
            memcpy(g_pendPos[keep], g_pendPos[i], 24);
            g_pendMat[keep] = g_pendMat[i];
            g_pendCost[keep] = out;
            out += a1 - a0; keep++;
        }
        g_pendCost[keep] = out;
        list->count = out;
        g_pend.n = keep;
        LOG(.str("blueprint build: partial, ").i(keep).str(" of ").i(n0).str(" blocks affordable (per material)"));
        if (!keep) { g_pend.bp = false; list->count = listBefore; flash(T(S_BP_NOT_ENOUGH_ANY)); beep(MB_ICONHAND); return 0; }
        g_partialOf = n0;
    }
    if (!creative_on() && !g_cfg.bpPartial && stock_known(list) && !affordable(nullptr, list, (uint32_t)list->count)) {
        LOG(.str("blueprint build: not enough materials, partial build off"));
        g_pend.n = 0; g_pend.bp = false; list->count = listBefore;
        flash(T(S_BP_NOT_ENOUGH)); beep(MB_ICONHAND);
        return 0;
    }
    g_pend.tick = GetTickCount();
    g_actList = list; g_actShapes = g_pend.n;
    return 1;
}

// A left click in Blueprint mode (server world, g_stateLock held): the first one notes the
// clicked block and opens the window (Save or Place); the click Save / Build sends acts.
static uint8_t bp_place_click(CostList* list, void* ctx, void* state, uint8_t* pdata, const CursorXf& place, const CursorXf* aim) {
    if (g_exactOpen) { LOG(.str("  blueprint window open: click ignored")); return 0; }
    if (g_bpWin && g_exactArmed) {
        const LONG w = g_bpWin;
        g_exactArmed = 0; g_exactFire = 0;
        LOG(.str("blueprint: ").str(w == BPW_SAVE ? "save" : "build").str(" click"));
        uint8_t r = w == BPW_SAVE ? bp_save_box(ctx, state, place) : bp_fill(list, ctx, state, pdata, place);
        g_bpWin = BPW_NONE; g_bpGhost = 0;
        return r;
    }
    if (remote_session()) { flash(T(S_BP_REMOTE)); beep(MB_ICONHAND); return 0; }
    // save: the aimed block (the building's own block); place: the ghost's spot next to the surface
    const CursorXf& t = g_bpSub == BP_SAVE && aim ? *aim : place;
    Aabb box; shape_bounds(state, &t, &box);
    reset_corners();
    g_scOp = OP_BUILD;
    g_scPos[0] = box_centre(box);
    for (int a = 0; a < 3; a++) g_scStep[a] = box.mx[a] - box.mn[a];
    g_scCursorOff = g_bpSub == BP_SAVE && aim ? 0x338 : 0x300;          // the live aim (place window)
    g_scOff[0] = g_scPos[0].x - t.pos[0]; g_scOff[1] = g_scPos[0].y - t.pos[1]; g_scOff[2] = g_scPos[0].z - t.pos[2];
    set_sc_rot(t.rot);
    g_bpPlaced = 0; g_bpPage = 0;
    for (int a = 0; a < 3; a++) { g_bpOff[a] = 0; g_bpSaveOff[a] = 0; }
    g_bpNameEdit = 0;
    g_bpWin = g_bpSub == BP_SAVE ? BPW_SAVE : BPW_PLACE;
    LOG(.str("blueprint: ").str(g_bpSub == BP_SAVE ? "save" : "place").str(" window, block (")
        .fix(g_scPos[0].x).c(',').fix(g_scPos[0].y).c(',').fix(g_scPos[0].z).c(')'));
    exact_begin(OP_BUILD);
    beep(MB_OK);
    return 0;
}
