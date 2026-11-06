// The blueprint windows (screen 52 while g_exactOpen and g_bpWin), built like the exact window:
// a full-height panel at the left with its own backdrop, the drawn box beside it, the right
// button hides it. Included by main.cpp after the exact window.

// Save / Build: arm the mod; the input thread sends the click once the window is down.
static void bp_arm() {
    EnterCriticalSection(&g_stateLock);
    g_exactPhase = 0;
    g_exactArmTick = GetTickCount();
    g_exactFire = g_exactArmTick + 200;          // after the window has closed
    g_exactArmed = 1;
    LeaveCriticalSection(&g_stateLock);
    g_menuReq = 2;
}
static bool bp_save_confirm() {
    if (!g_exactOpen || g_exactArmed || g_bpWin != BPW_SAVE) return false;
    I3 c[2]; bp_save_corners(c);
    core::Plan pl = {}; I3 c3[3] = { c[0], c[1], c[1] };
    const long cap = kBpMaxCells;
    if (!core::plan_fill(c3, 2, g_scStep, 50000000, &pl) || pl.total > cap) {
        Buf b; b.fmt(T(S_EXACT_TOO_BIG), num(cap).s); flash(b.z()); beep(MB_ICONHAND);
        return false;
    }
    g_exactC[0] = c[0]; g_exactC[1] = c[1];
    write_full_ini(g_cfg);
    LOG(.str("blueprint: save armed, ").i(pl.count[0]).c('x').i(pl.count[1]).c('x').i(pl.count[2]).str(" cells"));
    bp_arm();
    return true;
}
static bool bp_build_confirm() {
    if (!g_exactOpen || g_exactArmed || g_bpWin != BPW_PLACE || g_bpPage != 1 || !g_bp.ok) return false;
    g_bpMinArm = bp_min_from(bp_anchor_now());
    write_full_ini(g_cfg);
    LOG(.str("blueprint: build armed, turn ").i(g_bpRot).str(", low cell (").fix(g_bpMinArm.x).c(',').fix(g_bpMinArm.y).c(',').fix(g_bpMinArm.z).c(')'));
    bp_arm();
    return true;
}
static bool bp_confirm() {                       // Enter: ends typing the name, else Save / Build
    if (g_bpNameEdit) { g_bpNameEdit = 0; return false; }
    return g_bpWin == BPW_SAVE ? bp_save_confirm() : bp_build_confirm();
}
static int bp_clamp(int v) { return v > core::kExactMax ? core::kExactMax : v < -core::kExactMax ? -core::kExactMax : v; }

// ---- the name, typed on the keyboard while the save window is up (input thread)
// Keys are read with GetAsyncKeyState and turned into text with the game window's keyboard
// layout (Shift, Caps Lock and AltGr letters such as Polish ą work); Backspace deletes.
static void bp_type_keys(bool focus) {
    static bool prev[256]; static bool init = false;
    if (!init) { for (bool& p : prev) p = true; init = true; }
    if (g_bpNameEdit && (g_bpWin != BPW_SAVE || !g_exactOpen || g_exactHidden)) g_bpNameEdit = 0;   // the window went (or hid)
    bool live = focus && g_bpNameEdit && g_bpWin == BPW_SAVE && g_exactOpen && !g_exactHidden && g_menuOpen;
    BYTE kb[256] = {};
    auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (held(VK_SHIFT)) kb[VK_SHIFT] = 0x80;
    if (held(VK_CONTROL)) kb[VK_CONTROL] = 0x80;
    if (held(VK_MENU)) kb[VK_MENU] = 0x80;
    if (held(VK_RMENU)) { kb[VK_RMENU] = 0x80; kb[VK_MENU] = 0x80; kb[VK_CONTROL] = 0x80; }
    if (GetKeyState(VK_CAPITAL) & 1) kb[VK_CAPITAL] = 1;
    const bool ctrlOnly = kb[VK_CONTROL] && !kb[VK_MENU];
    HWND w = game_window();
    HKL layout = GetKeyboardLayout(w ? GetWindowThreadProcessId(w, nullptr) : 0);
    for (int vk = 8; vk < 0xff; vk++) {
        const bool typing = vk == VK_BACK || vk == VK_SPACE || (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') ||
                            (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) || (vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF) || vk == 0xE2;
        if (!typing) continue;
        bool d = held(vk);
        if (d && !prev[vk] && live && !ctrlOnly) {
            int n = g_bpTypedLen;
            if (vk == VK_BACK) {
                while (n > 0 && ((uint8_t)g_bpTyped[n - 1] & 0xC0) == 0x80) n--;       // UTF-8 continuation bytes
                if (n > 0) n--;
            } else {
                BYTE k2[256]; memcpy(k2, kb, sizeof k2); k2[vk] = 0x80;
                wchar_t wc[4] = {};
                int r = ToUnicodeEx((UINT)vk, MapVirtualKeyW((UINT)vk, 0), k2, wc, 4, 4, layout);   // 4: keep the dead-key state
                if (r == 1 && wc[0] >= 32) {
                    char u[8]; int un = WideCharToMultiByte(CP_UTF8, 0, wc, 1, u, sizeof u, nullptr, nullptr);
                    if (un > 0 && n + un < (int)sizeof g_bpTyped - 1 && n < 60) { memcpy(g_bpTyped + n, u, (size_t)un); n += un; }
                }
            }
            g_bpTyped[n] = 0; g_bpTypedLen = n;
        }
        prev[vk] = d;
    }
}

// The window frame shared by both windows (as the exact window).
struct BpFrame { alignas(16) uint8_t win[0x40]; alignas(16) uint8_t root[0x40]; alignas(16) uint8_t panel[0x40]; alignas(16) uint8_t list[0x40]; uint8_t* st; bool scroll; };
static void bp_frame_begin(BpFrame& f, const char* title, const char* help) {
    memset(f.win, 0, sizeof f.win); memset(f.root, 0, sizeof f.root); memset(f.panel, 0, sizeof f.panel); memset(f.list, 0, sizeof f.list);
    UiStringView name{ "buildingToolsBlueprint", 22 };
    g_beginWindow(f.win, &name, 1, 9, -1, 1);
    ui.beginRoot(f.root, ui.windowContext(f.win));
    ui.setStretch(ui_node(f.root), UiVec2{ 1, 1 });
    UiVec2 rgap{ 0, 0 };
    ui.setSpacing(ui_node(f.root), &rgap, 0);
    if (GetTickCount() - g_hoverTick > 100) update_preview_box();
    ui.beginBox(f.panel, 0);
    void* n = ui_node(f.panel);
    {
        const float* r = (const float*)((uint8_t*)n + 0x114);
        if (r[2] > 0 && r[3] > 0) ui.fillRect(r, 0x3f800000c0000000ull);
        const float* rr = (const float*)((uint8_t*)ui_node(f.root) + 0x114);
        float x = rr[2] > 0 && r[2] > 0 ? (r[0] + r[2] - rr[0]) / rr[2] : 0.f;
        g_exactPanelX = x > 0.f && x < 1.f ? x : 0.f;
    }
    const float pad[4] = { 120, 50, 60, 40 };
    ui.setPadding(n, pad);
    UiVec2 gap{ 0, 18 };
    ui.setSpacing(n, &gap, 0);
    uint16_t topLeft = 0x101;
    ui.setSelfAlign(n, &topLeft);
    ui.setLayout(n, &topLeft);
    ui.setWidth(n, 1180);
    ui.setStretch(n, UiVec2{ 0, 1 });
    ui_text(title, STYLE_TITLE);
    if (help && help[0]) ui_text(help, STYLE_BODY);
    alignas(16) static uint8_t st[0x500];
    static float offset[4];
    f.st = st;
    f.scroll = ui_scroll_begin(st, sizeof st, offset, 1000);
    ui.beginBox(f.list, 0);
    UiVec2 lgap{ 0, 18 };
    ui.setSpacing(ui_node(f.list), &lgap, 0);
    ui.setLayout(ui_node(f.list), &topLeft);
    ui.setStretch(ui_node(f.list), UiVec2{ 1, 0 });
}
static void bp_frame_rows_end(BpFrame& f) {
    ui.endBox(f.list);
    if (f.scroll) ui.endScroll(f.st);
}
static void bp_frame_end(BpFrame& f) {
    ui.endBox(f.panel);
    ui.endBox(f.root);
    ui.endWindow(f.win);
}
// a row of buttons side by side
struct BpRow { alignas(16) uint8_t box[0x40]; };
static void bp_row_begin(BpRow& r) {
    memset(r.box, 0, sizeof r.box);
    ui.beginBox(r.box, 0);
    UiVec2 bgap{ 24, 0 };
    ui.setSpacingX(ui_node(r.box), &bgap, 0);
}
static void bp_row_end(BpRow& r) { ui.endBox(r.box); }
static void bp_keys(Str enterLabel) {
    alignas(16) uint8_t keys[0x40] = {};
    ui.beginBox(keys, 0);
    UiVec2 kgap{ 0, 4 };
    ui.setSpacing(ui_node(keys), &kgap, 0);
    exact_hint(VK_RBUTTON, S_EXACT_KEY_LOOK);
    if (enterLabel != S_COUNT) exact_hint(VK_RETURN, enterLabel);
    exact_hint(VK_ESCAPE, S_EXACT_CANCEL);
    ui.endBox(keys);
}

// Three rows moving something by cells: right (- left), away (- closer), up (- down).
static void bp_offset_rows(Str header, int* off) {
    ui_text(T(header), STYLE_BODY);
    const Str labels[3] = { S_BP_RIGHT, S_BP_AWAY, S_BP_UP };
    for (int k = 0; k < 3; k++) { int v; if (ui_number(T(labels[k]), off[k], &v)) off[k] = bp_clamp(v); }
}

// ---- Save: reference, sizes, name, Save
static void draw_bp_save_window() {
    BpFrame f; bp_frame_begin(f, T(S_BP_SAVE_TITLE), T(S_BP_SAVE_HELP));
    {   // the name: a click starts typing, Enter or another click ends it
        Buf b; b.fmt(T(S_BP_NAME), g_bpTyped); if (g_bpNameEdit) b.str("_");
        BpRow r; bp_row_begin(r);
        if (ui_button(b.z())) g_bpNameEdit = !g_bpNameEdit;
        bp_row_end(r);
    }
    ui_text(T(S_BP_NAME_HINT), STYLE_BODY);
    {   LONG v;
        if (ref_option(T(S_EXACT_REF), g_cfg.bpRef, &v)) g_cfg.bpRef = v; }
    bp_offset_rows(S_BP_MOVE_POINT, g_bpSaveOff);
    ui_text(T(S_BP_SIZE_HDR), STYLE_BODY);
    const Str labels[3] = { S_EXACT_WIDTH, S_EXACT_DEPTH, S_EXACT_HEIGHT };
    int* fields[3] = { &g_cfg.bpW, &g_cfg.bpD, &g_cfg.bpH };
    for (int k = 0; k < 3; k++) { int v; if (ui_number(T(labels[k]), *fields[k], &v)) *fields[k] = bp_clamp(v); }
    {
        I3 c[2]; bp_save_corners(c);
        core::Plan pl = {}; I3 c3[3] = { c[0], c[1], c[1] };
        Buf b;
        if (core::plan_fill(c3, 2, g_scStep, 50000000, &pl))
            b.fmt(T(S_BP_CELLS), tenths((int)(g_scStep[0] * 10.f + 0.5f)).s, num(pl.count[g_exRAxis]).s, num(pl.count[g_exFAxis]).s, num(pl.count[1]).s);
        ui_text(b.z(), STYLE_BODY);
    }
    ui_text(T(S_BP_OBJECTS_LATER), STYLE_BODY);
    bp_frame_rows_end(f);
    BpRow r; bp_row_begin(r);
    if (ui_button(T(S_BP_SAVE_BTN))) bp_save_confirm();
    if (ui_button(T(S_BP_CLEAR_NAME), g_bpTypedLen > 0)) { g_bpTyped[0] = 0; g_bpTypedLen = 0; }
    if (ui_button(T(S_EXACT_CANCEL))) g_menuReq = 2;
    bp_row_end(r);
    bp_keys(S_BP_SAVE_BTN);
    bp_frame_end(f);
}

// ---- Place: page 0 the list, page 1 the chosen blueprint
static void draw_bp_place_window() {
    static DWORD s_lastDraw = 0;
    const DWORD now = GetTickCount();
    if (now - s_lastDraw > 500) bp_list_files();     // the window just came up (or back)
    s_lastDraw = now;
    if (g_bpPage == 0) {
        BpFrame f; bp_frame_begin(f, T(S_BP_LIST_TITLE), "");
        if (!g_bpFiles) ui_text(T(S_BP_NONE), STYLE_BODY);
        for (int q = 0; q < g_bpFiles; q++)
            if (ui_button(g_bpName[q])) {
                g_bpSel = q;
                if (bp_load(g_bpFile[q])) { g_bpPage = 1; g_bpPlaced = 0; }
                else flash(T(S_BP_BAD_FILE));
            }
        bp_frame_rows_end(f);
        BpRow r; bp_row_begin(r);
        if (ui_button(T(S_EXACT_CANCEL))) g_menuReq = 2;
        bp_row_end(r);
        bp_keys(S_COUNT);
        bp_frame_end(f);
        return;
    }
    bp_ghost_bits();
    BpFrame f; bp_frame_begin(f, g_bpSel < g_bpFiles ? g_bpName[g_bpSel] : T(S_BP_LIST_TITLE), T(g_bpPlaced ? S_BP_PLACED : S_BP_FOLLOWING));
    int rn[3]; float rs[3]; bp_dims(g_bpRot, rn, rs);
    {   Buf b; b.fmt(T(S_BP_SIZE), num(rn[g_exRAxis]).s, num(rn[g_exFAxis]).s, num(rn[1]).s, num(g_bp.filled).s); ui_text(b.z(), STYLE_BODY); }
    LONG v;
    static const char* const turns[4] = { "0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0" };
    if (ui_option(T(S_BP_ROTATION), turns, 4, g_bpRot & 3, &v)) { g_bpRot = v; LOG(.str("blueprint: turn ").i(v)); }
    if (ref_option(T(S_BP_REF), g_cfg.bpPlaceRef, &v)) g_cfg.bpPlaceRef = v;
    {
        BpRow r; bp_row_begin(r);
        if (!g_bpPlaced) { if (ui_button(T(S_BP_PLACE_BTN))) { g_bpAnchor = bp_aim_now(); g_bpPlaced = 1; LOG(.str("blueprint: placed")); } }
        else if (ui_button(T(S_BP_FOLLOW_BTN))) g_bpPlaced = 0;
        bp_row_end(r);
    }
    bp_offset_rows(S_BP_MOVE, g_bpOff);
    const char* const offOn[] = { T(S_OPT_OFF), T(S_OPT_ON) };
    if (ui_option(T(S_BP_PARTIAL), offOn, 2, g_cfg.bpPartial ? 1 : 0, &v)) g_cfg.bpPartial = v != 0;
    if (g_bp.h.objects > 0) {
        const char* const objOpt[] = { T(S_BP_OBJ_LEAVE), T(S_BP_OBJ_BUILD) };
        Buf ol; ol.fmt(T(S_BP_OBJ_COUNT), num(g_bp.h.objects).s);
        if (ui_option(ol.z(), objOpt, 2, g_cfg.bpObjects ? 1 : 0, &v)) g_cfg.bpObjects = v != 0;
    }
    bool fits = true;
    for (int a = 0; a < 3; a++) if (g_scStep[a] - rs[a] > 0.05f || rs[a] - g_scStep[a] > 0.05f) fits = false;
    if (!fits) {
        Buf w; w.fmt(T(S_BP_SHAPE), tenths((int)(rs[0] * 10.f + 0.5f)).s, tenths((int)(rs[1] * 10.f + 0.5f)).s, tenths((int)(rs[2] * 10.f + 0.5f)).s);
        ui_text(w.z(), STYLE_BODY);
    }
    ui_text(T(S_BP_COSTS), STYLE_BODY);
    int shortN = 0; bool unknown = false;
    for (int q = 0; q < g_bp.h.nm; q++) {
        const core::BpMat& m = g_bp.h.m[q];
        int64_t have = creative_on() ? -2 : stock_of(m.item);
        Buf label; label.i(q + 1).str(" (").x(m.item).c(')');
        Buf hv; if (have == -2) hv.str("-"); else if (have < 0) { hv.str(T(S_BP_MAT_UNKNOWN)); unknown = true; } else hv.i(have);
        if (have >= 0 && have < (int64_t)m.items) shortN++;
        Buf row; row.fmt(T(S_BP_MAT_ROW), label.z(), num(m.items).s, hv.z());
        ui_text(row.z(), STYLE_BODY);
    }
    if (g_cfg.bpObjects)                                 // the objects: one of each item per object
        for (int q = 0; q < g_bp.h.objects; q++) {
            const uint32_t item = g_bp.h.obj[q].item;
            bool first = true;
            for (int p = 0; p < q && first; p++) first = g_bp.h.obj[p].item != item;
            if (!first) continue;
            int need = 0;
            for (int p = q; p < g_bp.h.objects; p++) need += g_bp.h.obj[p].item == item;
            int64_t have = creative_on() ? -2 : stock_of(item);
            Buf label; label.x(item);
            Buf hv; if (have == -2) hv.str("-"); else if (have <= 0) { hv.str(T(S_BP_MAT_UNKNOWN)); unknown = true; } else hv.i(have);
            Buf row; row.fmt(T(S_BP_OBJ_ROW), label.z(), num(need).s, hv.z());
            ui_text(row.z(), STYLE_BODY);
        }
    if (unknown) ui_text(T(S_BP_STOCK_HINT), STYLE_BODY);
    if (shortN) { Buf sh; sh.fmt(T(S_BP_SHORT), num(shortN).s); ui_text(sh.z(), STYLE_BODY); }
    ui_text(T(S_BP_OBJECTS_LATER), STYLE_BODY);
    bp_frame_rows_end(f);
    BpRow r; bp_row_begin(r);
    if (ui_button(T(S_EXACT_BUILD), g_bp.ok)) bp_build_confirm();
    if (ui_button(T(S_BP_BACK))) { g_bpPage = 0; g_bpPlaced = 0; }
    if (ui_button(T(S_EXACT_CANCEL))) g_menuReq = 2;
    bp_row_end(r);
    bp_keys(S_EXACT_BUILD);
    bp_frame_end(f);
}
static void draw_bp_window() { if (g_bpWin == BPW_SAVE) draw_bp_save_window(); else draw_bp_place_window(); }
