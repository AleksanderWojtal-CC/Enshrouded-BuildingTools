// The blueprint window (screen 52 while g_exactOpen and g_bpWin), built like the exact window:
// a full-height panel at the left with its own backdrop, the drawn ghost beside it, the right
// button hides it. Included by main.cpp after the exact window.

// Build in the window: arm the mod; the input thread sends the click once the window is down.
static bool bp_confirm() {
    if (!g_exactOpen || g_exactArmed || !g_bpWin || !g_bp.ok) return false;
    EnterCriticalSection(&g_stateLock);
    g_exactPhase = 0;
    g_exactArmTick = GetTickCount();
    g_exactFire = g_exactArmTick + 200;          // after the window has closed
    g_exactArmed = 1;
    LeaveCriticalSection(&g_stateLock);
    g_menuReq = 2;
    LOG(.str("blueprint: build armed, turn ").i(g_bpRot));
    return true;
}

static void draw_bp_window() {
    alignas(16) uint8_t win[0x40] = {}, root[0x40] = {}, panel[0x40] = {}, btns[0x40] = {}, list[0x40] = {}, keys[0x40] = {};
    static DWORD s_lastDraw = 0;
    const DWORD now = GetTickCount();
    const bool fresh = now - s_lastDraw > 500;        // the window just came up (or back): the files again
    s_lastDraw = now;
    if (fresh) {
        bp_list_files();
        if (g_bpFiles) bp_load(g_bpFile[g_bpSel]); else g_bp.ok = false;
    }
    bp_ghost_bits();
    UiStringView name{ "buildingToolsBlueprint", 22 };
    g_beginWindow(win, &name, 1, 9, -1, 1);
    ui.beginRoot(root, ui.windowContext(win));
    ui.setStretch(ui_node(root), UiVec2{ 1, 1 });
    UiVec2 rgap{ 0, 0 };
    ui.setSpacing(ui_node(root), &rgap, 0);
    if (GetTickCount() - g_hoverTick > 100) update_preview_box();

    ui.beginBox(panel, 0);
    void* n = ui_node(panel);
    {
        const float* r = (const float*)((uint8_t*)n + 0x114);
        if (r[2] > 0 && r[3] > 0) ui.fillRect(r, 0x3f800000c0000000ull);
        const float* rr = (const float*)((uint8_t*)ui_node(root) + 0x114);
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

    ui_text(T(S_BP_TITLE), STYLE_TITLE);
    ui_text(T(S_BP_HELP), STYLE_BODY);

    alignas(16) static uint8_t st[0x500];
    static float offset[4];
    bool scroll = ui_scroll_begin(st, sizeof st, offset, 1000);
    ui.beginBox(list, 0);
    UiVec2 lgap{ 0, 18 };
    ui.setSpacing(ui_node(list), &lgap, 0);
    ui.setLayout(ui_node(list), &topLeft);
    ui.setStretch(ui_node(list), UiVec2{ 1, 0 });
    if (!g_bpFiles) ui_text(T(S_BP_NONE), STYLE_BODY);
    else {
        const char* names[kBpMaxFiles];
        for (int q = 0; q < g_bpFiles; q++) names[q] = g_bpName[q];
        LONG v;
        if (ui_option(T(S_BP_PICK), names, g_bpFiles, g_bpSel, &v)) { g_bpSel = v; bp_load(g_bpFile[v]); }
        static const char* const turns[4] = { "0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0" };
        if (ui_option(T(S_BP_ROTATION), turns, 4, g_bpRot & 3, &v)) { g_bpRot = v; LOG(.str("blueprint: turn ").i(v)); }
        const char* const offOn[] = { T(S_OPT_OFF), T(S_OPT_ON) };
        if (ui_option(T(S_BP_PARTIAL), offOn, 2, g_cfg.bpPartial ? 1 : 0, &v)) { g_cfg.bpPartial = v != 0; write_full_ini(g_cfg); }
        if (!g_bp.ok) ui_text(T(S_BP_BAD_FILE), STYLE_BODY);
        else {
            int rn[3]; float rs[3]; bp_dims(g_bpRot, rn, rs);
            Buf b; b.fmt(T(S_BP_SIZE), num(rn[0]).s, num(rn[2]).s, num(rn[1]).s, num(g_bp.filled).s);
            ui_text(b.z(), STYLE_BODY);
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
            if (unknown) ui_text(T(S_BP_STOCK_HINT), STYLE_BODY);
            if (shortN) { Buf sh; sh.fmt(T(S_BP_SHORT), num(shortN).s); ui_text(sh.z(), STYLE_BODY); }
            ui_text(T(S_BP_OBJECTS_LATER), STYLE_BODY);
        }
    }
    ui.endBox(list);
    if (scroll) ui.endScroll(st);

    ui.beginBox(btns, 0);
    UiVec2 bgap{ 24, 0 };
    ui.setSpacingX(ui_node(btns), &bgap, 0);
    if (ui_button(T(S_EXACT_BUILD), g_bp.ok)) bp_confirm();
    if (ui_button(T(S_EXACT_CANCEL))) g_menuReq = 2;
    ui.endBox(btns);
    ui.beginBox(keys, 0);
    UiVec2 kgap{ 0, 4 };
    ui.setSpacing(ui_node(keys), &kgap, 0);
    exact_hint(VK_RBUTTON, S_EXACT_KEY_LOOK);
    exact_hint(VK_RETURN, S_EXACT_BUILD);
    exact_hint(VK_ESCAPE, S_EXACT_CANCEL);
    ui.endBox(keys);
    ui.endBox(panel);
    ui.endBox(root);
    ui.endWindow(win);
}
