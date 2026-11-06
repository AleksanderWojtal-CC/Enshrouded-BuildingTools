// Blueprints (issue #49): save the blocks of an area to a file, build them again elsewhere.
// Included by main.cpp after the area code; the window is in blueprint_ui.h.
//
//   Save   - Blueprint mode, right click two corners (as an Area remove). Nothing is removed:
//            every cell of the box (the size of the shape in hand) is probed with the mod's
//            removal dry run (RemoveShape with refunds suppressed, as the fill bucket does),
//            which reports the volume of every building material in it. A cell is saved
//            with its main material when that fills at least half of it. The material table
//            names each material by its item id (from the game's refund builder, 48ae10),
//            so files survive game patches that renumber the material bytes.
//   Build  - Blueprint mode, left click: the clicked shape's centre is the blueprint's
//            bottom centre and the blueprint window opens (the exact window's screen,
//            ghost and right-button hiding). Build arms the mod and the input thread sends
//            a click; the place hook then prices one shape per saved cell with that cell's
//            material (the player's material byte, pdata+0x454, set around each dry run
//            and commit; the cost entry's item taken from the refund builder for that
//            material) and the commit hook writes them, with the usual undo budget and
//            partial build.
// Building pieces and objects are not saved yet; a dedicated server cannot build blueprints yet.

static const int kBpMaxCells = 20000;
static const int kBpMaxFiles = 64;
struct Blueprint {
    bool ok; core::BpHeader h; uint8_t cell[kBpMaxCells]; int filled;
    wchar_t path[MAX_PATH];
};
static Blueprint g_bp;                           // the one picked in the window (UI thread; the server reads it once armed)
static volatile LONG g_bpRot = 0;                // quarter turns about the vertical axis
static volatile LONG g_bpGhost = 0;              // the drawn box is the blueprint (vkdraw: its cells from g_bpBits)
static uint32_t g_bpBits[1250];                  // the blueprint's cells in the drawn box (turned), one bit each
static volatile LONG g_bpBitsOk = 0;
static wchar_t g_bpFile[kBpMaxFiles][MAX_PATH];  // file names (no folder) in the window's list, newest first
static char g_bpName[kBpMaxFiles][96];           // the same as UTF-8, without ".btbp"
static int g_bpFiles = 0;
static LONG g_bpSel = 0;
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
// Centre of cell (ri, j, rk) of the turned blueprint; the clicked cell is the bottom centre.
static I3 bp_cell_pos(const I3& o, const int* rn, const float* rs, int ri, int j, int rk) {
    const int cx = rn[0] / 2, cz = rn[2] / 2;
    return I3{ o.x + (int64_t)(ri - cx) * bp_fix(rs[0]), o.y + (int64_t)j * bp_fix(rs[1]), o.z + (int64_t)(rk - cz) * bp_fix(rs[2]) };
}
// the drawn box of the blueprint being placed (fixed point)
static bool bp_box(int64_t lo[3], int64_t hi[3], float step[3]) {
    if (!g_bp.ok || !g_bpWin) return false;
    int rn[3]; float rs[3]; bp_dims(g_bpRot, rn, rs);
    I3 a = bp_cell_pos(g_scPos[0], rn, rs, 0, 0, 0), b = bp_cell_pos(g_scPos[0], rn, rs, rn[0] - 1, rn[1] - 1, rn[2] - 1);
    const int64_t pa[3] = { a.x, a.y, a.z }, pb[3] = { b.x, b.y, b.z };
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
// The blueprint files, newest first (the names start with the date), into the window's list.
static void bp_list_files() {
    wchar_t p[MAX_PATH]; bp_dir(p); lstrcatW(p, L"*.btbp");
    WIN32_FIND_DATAW fd;
    g_bpFiles = 0;
    HANDLE h = FindFirstFileW(p, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (lstrlenW(fd.cFileName) >= MAX_PATH - 1) continue;
            // insertion by name, descending
            int at = 0;
            while (at < g_bpFiles && lstrcmpiW(g_bpFile[at], fd.cFileName) > 0) at++;
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
// Writes the blueprint; the name is the date and time ("Blueprint 2026-11-06 14-05-09").
static bool bp_write(const core::BpHeader& h, const uint8_t* cells, char* nameOut, int cap) {
    static uint8_t buf[16 + 12 + 6 + 4 + 2 + core::BP_MAX_MATS * 16 + 4 + kBpMaxCells * 4 + 64];
    int size = core::bp_encode(h, cells, buf, sizeof buf);
    if (!size) return false;
    wchar_t p[MAX_PATH]; bp_dir(p);
    CreateDirectoryW(p, nullptr);
    SYSTEMTIME t; GetLocalTime(&t);
    Buf b; b.str("Blueprint ").u(t.wYear, 10, 4).c('-').u(t.wMonth, 10, 2).c('-').u(t.wDay, 10, 2).c(' ')
        .u(t.wHour, 10, 2).c('-').u(t.wMinute, 10, 2).c('-').u(t.wSecond, 10, 2);
    const char* name = b.z();
    copy_text(nameOut, name, cap);
    int n = lstrlenW(p);
    for (int q = 0; name[q] && n < MAX_PATH - 8; q++) p[n++] = (wchar_t)(uint8_t)name[q];
    p[n] = 0; lstrcatW(p, L".btbp");
    HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; BOOL ok = WriteFile(f, buf, (DWORD)size, &w, nullptr);
    CloseHandle(f);
    return ok && w == (DWORD)size;
}

// ---- save (server, the dismantle system's right click; g_stateLock held)
// Cell probe: the volume of every removable material in the cell, as connected delete measures it.
static uint8_t bp_save_click(CostList* list, void* ctx, void* state, Aabb* bx, IntXf* xf, uint8_t check) {
    g_rpend.ctx = ctx; g_rpend.tid = GetCurrentThreadId(); g_rpend.tick = GetTickCount(); g_rpend.n = 0; g_rpend.only = 0;
    g_rpend.suppress = true;                           // a blueprint click removes nothing
    if (g_exactOpen) return 0;                         // a right click to look around behind the window
    if (g_scOp != OP_SAVE) { reset_corners(); g_scOp = OP_SAVE; }
    LONG n = g_sc;
    if (n < 0 || n > 1) n = 0;
    g_scPos[n] = I3{ xf->pos[0], xf->pos[1], xf->pos[2] };       // removal transforms are shape centres
    LOG(.str("blueprint save: corner ").i(n + 1).str(" at (").fix(xf->pos[0]).c(',').fix(xf->pos[1]).c(',').fix(xf->pos[2]).c(')'));
    if (n == 0) {
        for (int a = 0; a < 3; a++) g_scStep[a] = bx->mx[a] - bx->mn[a];
        g_scCursorOff = 0x338;
        uint8_t* cc = live_cc();
        if (cc && readable(cc + 0x338, 0x28)) {
            for (int a = 0; a < 3; a++) g_scOff[a] = xf->pos[a] - ((int64_t*)(cc + 0x338))[a];
            set_sc_rot((const float*)(cc + 0x338 + 0x18));
        } else { for (int a = 0; a < 3; a++) g_scOff[a] = 0; g_scRotOk = false; }
        g_sc = 1; g_corners = 1;
        flash(T(S_BP_CORNER)); beep(MB_OK);
        return 0;
    }
    g_sc = 0; g_corners = 0;
    float step[3] = { bx->mx[0] - bx->mn[0], bx->mx[1] - bx->mn[1], bx->mx[2] - bx->mn[2] };
    I3 c[3] = { g_scPos[0], g_scPos[1], g_scPos[1] };
    core::Plan pl = {};
    const long cap = g_cfg.maxShapes < kBpMaxCells ? g_cfg.maxShapes : kBpMaxCells;
    bool ok = core::plan_fill(c, 2, step, 50000000, &pl) && pl.total <= cap;
    LOG(.str("blueprint save: step=(").f(step[0]).c(',').f(step[1]).c(',').f(step[2]).str(") counts=")
        .i(pl.count[0]).c('x').i(pl.count[1]).c('x').i(pl.count[2]).str(ok ? "" : " REFUSED (too big)"));
    if (!ok) { Buf b; b.fmt(T(S_TOO_BIG), num(pl.total).s, num(cap).s); flash(b.z()); beep(MB_ICONHAND); return 0; }
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
    int filled = 0, partial = 0;
    for (int j = 0; j < pl.count[1]; j++)
        for (int i = 0; i < pl.count[0]; i++)
            for (int k = 0; k < pl.count[2]; k++) {
                I3 q = core::plan_pos(pl, i, j, k);
                IntXf ix = *xf; Aabb b2 = *bx;
                const int64_t d[3] = { q.x - xf->pos[0], q.y - xf->pos[1], q.z - xf->pos[2] };
                for (int a = 0; a < 3; a++) { ix.pos[a] += d[a]; float df = (float)d[a] * kInv; b2.mn[a] += df; b2.mx[a] += df; }
                memset(g_remVols, 0, sizeof(g_remVols));
                g_ownRemoveTid = GetCurrentThreadId();
                g_remove(list, ctx, state, &b2, &ix, nullptr, check);
                g_ownRemoveTid = 0;
                int best = 0; float bv = 0.f;
                for (int m = 0x80; m < 0x100; m++) if (g_remVols[m] > bv) { bv = g_remVols[m]; best = m; }
                if (!best) continue;
                if (bv < vox * 0.5f) { partial++; continue; }
                if (matOf[best] < 0) {
                    if (h.nm >= core::BP_MAX_MATS) continue;
                    matOf[best] = h.nm; h.m[h.nm].mat = (uint8_t)best; h.nm++;
                }
                // box space: counted from the low corner whatever order the corners were clicked in
                const int bi = pl.stepFix[0] >= 0 ? i : pl.count[0] - 1 - i;
                const int bj = pl.stepFix[1] >= 0 ? j : pl.count[1] - 1 - j;
                const int bk = pl.stepFix[2] >= 0 ? k : pl.count[2] - 1 - k;
                cells[core::bp_index(h.n, bi, bj, bk)] = (uint8_t)matOf[best];
                h.m[matOf[best]].cells++;
                filled++;
            }
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
    if (!bp_write(h, cells, name, sizeof name)) { flash(T(S_BP_WRITE_FAIL)); beep(MB_ICONHAND); LOG(.str("blueprint save: write failed ").u(GetLastError())); return 0; }
    Buf b; b.fmt(T(S_BP_SAVED), name, num(filled).s, num(h.nm).s); flash(b.z()); beep(MB_ICONASTERISK);
    LOG(.str("blueprint saved: ").str(name));
    return 0;
}

// ---- build (server, the blueprint system's left click; g_stateLock held)
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
    static int mat[core::BP_MAX_MATS]; static CostEntry tmpl[core::BP_MAX_MATS];
    const int missing = bp_map(ctx, mat, tmpl);
    if (missing) { Buf b; b.fmt(T(S_BP_NO_MATERIAL), num(missing).s); flash(b.z()); }
    const uint64_t listBefore = list->count;
    pend_begin(ctx, target, false, state ? *(uint32_t*)state : 0);
    g_pend.bp = true; g_pend.pdata = pdata;
    const uint8_t savedMat = pdata[0x454];
    const I3 tc = box_centre(box);                       // positions are centres; the dry run takes the cursor
    const int64_t off[3] = { target.pos[0] - tc.x, target.pos[1] - tc.y, target.pos[2] - tc.z };
    CursorXf xf = target;
    const int* n = g_bp.h.n;
    long priced = 0; int logged = 0;
    for (int j = 0; j < rn[1]; j++)                      // bottom layer first
        for (int rk = 0; rk < rn[2]; rk++)
            for (int ri = 0; ri < rn[0]; ri++) {
                int i, k; core::bp_unturn(g_bpRot, ri, rk, n[0], n[2], &i, &k);
                const uint8_t mi = g_bp.cell[core::bp_index(n, i, j, k)];
                if (mi == core::BP_EMPTY || mi >= g_bp.h.nm || mat[mi] < 0) continue;
                if (g_pend.n >= kMaxPending) continue;
                I3 p = bp_cell_pos(g_scPos[0], rn, rs, ri, j, rk);
                xf.pos[0] = p.x + off[0]; xf.pos[1] = p.y + off[1]; xf.pos[2] = p.z + off[2];
                pdata[0x454] = (uint8_t)mat[mi];
                g_pendMat[g_pend.n] = (uint8_t)mat[mi];
                const uint64_t before = list->count;
                if (price_shape(list, ctx, state, pdata, &xf, false)) priced++;
                CostEntry* e = (CostEntry*)list->data;
                for (uint64_t q = before; q < list->count; q++) {
                    if (logged < 3) {
                        logged++;
                        Buf lb; lb.str("  cost entry of ").x(mat[mi]).str(": game ");
                        for (int b = 0; b < 0x18; b++) lb.u(e[q].b[b], 16, 2);
                        lb.str(" template "); for (int b = 0; b < 0x14; b++) lb.u(tmpl[mi].b[b], 16, 2);
                        log_line(lb);
                    }
                    memcpy(e[q].b, tmpl[mi].b, 0x14);    // the item of this material, the game's count
                }
            }
    pdata[0x454] = savedMat;
    g_pendCost[g_pend.n] = (uint32_t)list->count;
    LOG(.str("blueprint build: turn ").i(g_bpRot).str(", priced ").i(priced).str(" of ").i(g_bp.filled)
        .str(" blocks, cost entries ").u(list->count - listBefore));
    if (!priced) { g_pend.n = 0; g_pend.bp = false; list->count = listBefore; flash(T(S_BP_NOTHING)); beep(MB_ICONHAND); return 0; }
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

// A left click in Blueprint mode (server world, g_stateLock held): the first one sets the
// spot and opens the window; the click Build sends builds.
static uint8_t bp_place_click(CostList* list, void* ctx, void* state, uint8_t* pdata, const CursorXf& target) {
    if (g_exactOpen) { LOG(.str("  blueprint window open: click ignored")); return 0; }
    if (g_bpWin && g_exactArmed) {
        g_exactArmed = 0; g_exactFire = 0;
        LOG(.str("blueprint: build click"));
        uint8_t r = bp_fill(list, ctx, state, pdata, target);
        g_bpWin = 0; g_bpGhost = 0;
        return r;
    }
    if (remote_session()) { flash(T(S_BP_REMOTE)); beep(MB_ICONHAND); return 0; }
    Aabb box; shape_bounds(state, &target, &box);
    reset_corners();
    g_scOp = OP_BUILD;
    g_scPos[0] = box_centre(box);
    for (int a = 0; a < 3; a++) g_scStep[a] = box.mx[a] - box.mn[a];
    LOG(.str("blueprint: spot (").fix(g_scPos[0].x).c(',').fix(g_scPos[0].y).c(',').fix(g_scPos[0].z).str("), window opens"));
    g_bpWin = 1;
    exact_begin(OP_BUILD);
    beep(MB_OK);
    return 0;
}
