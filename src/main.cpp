// Enshrouded Building Tools: dinput8.dll proxy mod (single player).
//
// Modes
//   Prefab    - vanilla placement (one shape per click).
//   Structure - click 3 corners; the box they span (line, plane or cube) is
//               filled with the current shape. Click the same spot twice to
//               finish a line or plane early.
// Replace toggle (both modes): what is under the target is removed first and
// the shape is placed with the current material in its place.
//
// Built freestanding with clang (mingw target), no C runtime.

#include <windows.h>
#include "core.h"

using core::CursorXf;
using core::IntXf;
using core::Aabb;
using core::I3;

#define MOD_NAME    "Building Tools"
#define MOD_VERSION "0.14.0"

// -------------------------------------------------------------- freestanding
extern "C" {
void* memcpy(void* d, const void* s, size_t n) { auto* a = (uint8_t*)d; auto* b = (const uint8_t*)s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, size_t n) { auto* a = (uint8_t*)d; while (n--) *a++ = (uint8_t)c; return d; }
int memcmp(const void* x, const void* y, size_t n) { auto* a = (const uint8_t*)x; auto* b = (const uint8_t*)y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
void* memmove(void* d, const void* s, size_t n) { auto* a = (uint8_t*)d; auto* b = (const uint8_t*)s; if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; } return d; }
int _fltused = 0;
}

// ------------------------------------------------------------------- logging
static CRITICAL_SECTION g_logLock;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static wchar_t g_dir[MAX_PATH];
static bool g_logOn = true;

struct Buf {
    char s[1024]; int n = 0;
    Buf& c(char ch) { if (n < (int)sizeof(s) - 1) s[n++] = ch; return *this; }
    Buf& str(const char* t) { while (*t) c(*t++); return *this; }
    Buf& u(uint64_t v, int base = 10, int width = 0) {
        char t[24]; int k = 0;
        do { int d = (int)(v % base); t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v && k < 24);
        while (k < width && k < 24) t[k++] = '0';
        while (k) c(t[--k]);
        return *this;
    }
    Buf& i(int64_t v) { if (v < 0) { c('-'); return u((uint64_t)(-v)); } return u((uint64_t)v); }
    Buf& x(uint64_t v) { str("0x"); return u(v, 16); }
    Buf& f(double v, int dec = 3) {
        if (v != v) return str("nan");
        if (v < 0) { c('-'); v = -v; }
        if (v > 9.2e18) return str("inf");
        double m = 1; for (int k = 0; k < dec; k++) m *= 10;
        uint64_t t = (uint64_t)(v * m + 0.5);
        u(t / (uint64_t)m); c('.'); u(t % (uint64_t)m, 10, dec);
        return *this;
    }
    Buf& fix(int64_t v) { return f((double)v / core::kFix); } // fixed point -> metres
    const char* z() { s[n] = 0; return s; }
};

static void log_line(Buf& b) {
    if (!g_logOn || g_log == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t);
    Buf o;
    o.u(t.wHour, 10, 2).c(':').u(t.wMinute, 10, 2).c(':').u(t.wSecond, 10, 2).c('.').u(t.wMilliseconds, 10, 3).c(' ');
    o.str(b.z()).str("\r\n");
    EnterCriticalSection(&g_logLock);
    DWORD w; WriteFile(g_log, o.s, o.n, &w, nullptr);
    LeaveCriticalSection(&g_logLock);
}
#define LOG(expr) do { Buf _b; _b expr; log_line(_b); } while (0)

static void open_log() {
    wchar_t p[MAX_PATH];
    lstrcpyW(p, g_dir); lstrcatW(p, L"BuildingTools.log");
    wchar_t prev[MAX_PATH];
    lstrcpyW(prev, g_dir); lstrcatW(prev, L"BuildingTools.prev.log");
    MoveFileExW(p, prev, MOVEFILE_REPLACE_EXISTING);    // keep the last session (e.g. after a crash)
    g_log = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// -------------------------------------------------------------------- config
struct Config {
    int keyStructure = VK_F6;
    int keyReplace   = VK_F7;
    int keyCancel    = VK_F8;
    int keyStyle     = VK_F9;
    int maxShapes    = 2000;
    int overlay      = 1;
    int sound        = 1;
    int log          = 1;
    int corners      = 2;      // area corners: 2 = opposite corners of a box, 3 = base + height
    int keyHover     = VK_F10;
    int keyFarther   = VK_PRIOR;   // Page Up
    int keyCloser    = VK_NEXT;    // Page Down
    int wheelKey     = 'Q';        // hold + mouse wheel = free placement distance
    int keyFill      = VK_F11;     // area fill: solid / hollow / walls
    int refundPopup  = 1;          // show the game's item pop-up for refunds
    int preview      = 1;          // show the area with the game's ghost highlight
    int hoverDist10  = 40;         // free placement distance in tenths of a metre
    int panelScale   = 100;        // info panel size in percent
    int panelBottom  = 0;      // panel distance from the bottom edge (pixels at 1080p)
    int panelTimeout = 5;      // seconds without change before the panel hides (0 = never)
    int panelKeys    = 1;      // key hint line
};
static Config g_cfg;

static int ini_int(const wchar_t* sec, const wchar_t* key, int def) {
    wchar_t p[MAX_PATH];
    lstrcpyW(p, g_dir); lstrcatW(p, L"BuildingTools.ini");
    return (int)GetPrivateProfileIntW(sec, key, def, p);
}

// Writes the whole ini with comments and the given values (first start, or an ini
// from an older version that lacks newer options: its values are kept).
struct IniText {
    char s[6144]; int n = 0;
    IniText& t(const char* x) { while (*x && n < (int)sizeof(s) - 1) s[n++] = *x++; return *this; }
    IniText& i(int v) { char b[16]; int k = 0; bool neg = v < 0; unsigned u = neg ? -v : v;
        do { b[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 15);
        if (neg) t("-"); while (k) { char c[2] = { b[--k], 0 }; t(c); } return *this; }
};
static void write_full_ini(const Config& c) {
    static IniText o; o.n = 0;
    o.t("; Enshrouded Building Tools " MOD_VERSION "\r\n")
     .t("; Close the game before editing. Keys are Windows virtual-key codes (decimal):\r\n")
     .t(";   F1..F12 = 112..123   A..Z = 65..90   0..9 = 48..57   Page Up 33  Page Down 34\r\n")
     .t(";   Insert 45  Delete 46  Home 36  End 35  Numpad 0..9 = 96..105  (0 = key off)\r\n")
     .t("[Keys]\r\n")
     .t("; Prefab mode <-> Area mode\r\nToggleAreaMode=").i(c.keyStructure).t("\r\n")
     .t("; Replace on/off\r\nToggleReplace=").i(c.keyReplace).t("\r\n")
     .t("; Clear area corners\r\nCancelCorners=").i(c.keyCancel).t("\r\n")
     .t("; Replace style: full shape / overlap only\r\nToggleReplaceStyle=").i(c.keyStyle).t("\r\n")
     .t("; Area fill: solid / hollow room / walls only\r\nCycleAreaFill=").i(c.keyFill).t("\r\n")
     .t("; Free placement on/off\r\nToggleFreePlacement=").i(c.keyHover).t("\r\n")
     .t("; Free placement distance: hold this key + mouse wheel (81 = Q), or the two keys below\r\nFreePlacementWheelKey=").i(c.wheelKey).t("\r\n")
     .t("FreePlacementFarther=").i(c.keyFarther).t("\r\n")
     .t("FreePlacementCloser=").i(c.keyCloser).t("\r\n")
     .t("\r\n[Area]\r\n")
     .t("; 2 = two opposite corners, 3 = two base corners then the height\r\nCorners=").i(c.corners).t("\r\n")
     .t("; Free placement start distance in tenths of a metre (40 = 4 m)\r\nFreePlacementDistance=").i(c.hoverDist10).t("\r\n")
     .t("; Show the area being set up with the game's ghost highlight (1 = on)\r\nPreview=").i(c.preview).t("\r\n")
     .t("\r\n[Limits]\r\n")
     .t("; Most shapes one area action may build, replace or remove (max 20000)\r\nMaxShapes=").i(c.maxShapes).t("\r\n")
     .t("\r\n[Interface]\r\n")
     .t("; Info panel at the bottom of the screen (0 = off)\r\nOverlay=").i(c.overlay).t("\r\n")
     .t("; Panel size in percent, lift from the bottom edge (pixels at 1080p)\r\nPanelScale=").i(c.panelScale).t("\r\n")
     .t("PanelBottom=").i(c.panelBottom).t("\r\n")
     .t("; Seconds without change before the panel hides (0 = always shown), key hint line\r\nPanelTimeout=").i(c.panelTimeout).t("\r\n")
     .t("PanelKeys=").i(c.panelKeys).t("\r\n")
     .t("; Sound: 1 = game sounds, 2 = Windows sounds, 0 = off\r\nSound=").i(c.sound).t("\r\n")
     .t("; Item pop-up when Replace gives materials back (1 = on)\r\nRefundPopup=").i(c.refundPopup).t("\r\n")
     .t("\r\n[Debug]\r\nLog=").i(c.log).t("\r\n");
    wchar_t p[MAX_PATH];
    lstrcpyW(p, g_dir); lstrcatW(p, L"BuildingTools.ini");
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, o.s, (DWORD)o.n, &w, nullptr); CloseHandle(h); }
}

static void load_config() {
    g_cfg.keyStructure = ini_int(L"Keys", L"ToggleAreaMode", ini_int(L"Keys", L"ToggleStructureMode", VK_F6));
    g_cfg.keyReplace   = ini_int(L"Keys", L"ToggleReplace", VK_F7);
    g_cfg.keyCancel    = ini_int(L"Keys", L"CancelCorners", VK_F8);
    g_cfg.keyStyle     = ini_int(L"Keys", L"ToggleReplaceStyle", VK_F9);
    g_cfg.maxShapes    = ini_int(L"Limits", L"MaxShapes", 2000);
    g_cfg.overlay      = ini_int(L"Interface", L"Overlay", 1);
    g_cfg.sound        = ini_int(L"Interface", L"Sound", 1);
    g_cfg.log          = ini_int(L"Debug", L"Log", 1);
    g_cfg.corners      = ini_int(L"Area", L"Corners", 2) == 3 ? 3 : 2;
    g_cfg.keyHover     = ini_int(L"Keys", L"ToggleFreePlacement", VK_F10);
    g_cfg.keyFarther   = ini_int(L"Keys", L"FreePlacementFarther", VK_PRIOR);
    g_cfg.keyCloser    = ini_int(L"Keys", L"FreePlacementCloser", VK_NEXT);
    g_cfg.wheelKey     = ini_int(L"Keys", L"FreePlacementWheelKey", 'Q');
    g_cfg.keyFill      = ini_int(L"Keys", L"CycleAreaFill", VK_F11);
    g_cfg.refundPopup  = ini_int(L"Interface", L"RefundPopup", 1);
    g_cfg.preview      = ini_int(L"Area", L"Preview", 1);
    g_cfg.hoverDist10  = ini_int(L"Area", L"FreePlacementDistance", 40);
    g_cfg.panelScale   = ini_int(L"Interface", L"PanelScale", 100);
    if (g_cfg.panelScale < 50) g_cfg.panelScale = 50;
    if (g_cfg.panelScale > 300) g_cfg.panelScale = 300;
    g_cfg.panelBottom  = ini_int(L"Interface", L"PanelBottom", 0);
    g_cfg.panelTimeout = ini_int(L"Interface", L"PanelTimeout", 5);
    g_cfg.panelKeys    = ini_int(L"Interface", L"PanelKeys", 1);
    if (g_cfg.maxShapes < 1) g_cfg.maxShapes = 1;
    if (g_cfg.maxShapes > 20000) g_cfg.maxShapes = 20000;
    // first start or an ini from an older version: write every option (values kept)
    if (ini_int(L"Keys", L"CycleAreaFill", -1) == -1 || ini_int(L"Interface", L"RefundPopup", -1) == -1 ||
        ini_int(L"Area", L"Preview", -1) == -1)
        write_full_ini(g_cfg);
}

// --------------------------------------------------------------- game access
// Signatures (generated and verified against the 1.0 client, 2026-10-20 build).
static const char* SIG_PLACE  = "48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 C0 FE FF FF";
static const char* SIG_REMOVE = "48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8D AC 24 00 E7 FF FF B8 00 1A 00 00";
static const char* SIG_BOUNDS = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 B1 48 81 EC 90 00 00 00";
// Caller layout: ServerCursor component pointer at [rbp-0x58], cursor copy at rbp+0xC0.
static const char* SIG_CURSOR_CALL = "4C 8B 45 A8 48 8D 8D F8 00 00 00 48 8B 55 98 E8 ?? ?? ?? ?? 0F 10 00 0F 11 85 C0 00 00 00";
static const char* SIG_PLACE_CALL  = "48 8D 85 C0 00 00 00 4C 8B CE 4C 8B C3 48 89 44 24 20 48 8D 95 30 01 00 00";
// Directly before PLACE_CALL: cost list = undo step [rbp+0x220] + 0x110 on the server
// (UndoStorage only exists there), else the stack-local list at rbp+0x430.
static const char* SIG_SERVER_CHECK =
    "48 8B BD 20 02 00 00 48 8D 85 58 04 00 00 48 89 85 30 04 00 00 48 8D 05 ?? ?? ?? ?? 48 89 85 48 04 00 00 "
    "4C 89 B5 50 04 00 00 4C 89 B5 38 04 00 00 48 C7 85 40 04 00 00 08 00 00 00 48 85 FF 74 09 48 81 C7 10 01 00 00 "
    "EB 07 48 8D BD 30 04 00 00";
// Charges (consume=1) or refunds (consume=0) every entry of a cost list.
static const char* SIG_PAY = "40 55 56 48 83 EC 78 48 8B F2 48 8B E9 0F B6 11 85 D2 0F 84 ?? ?? ?? ??";
// The real voxel writes. The PlaceShape/RemoveShape functions above are dry runs
// (they price an action); after payment the game commits it with these.
static const char* SIG_PLACE_COMMIT  = "48 8B C4 55 53 56 57 41 54 41 56 41 57 48 8D A8 88 FD FF FF 48 81 EC 40 03 00 00";
static const char* SIG_REMOVE_COMMIT = "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8D AC 24 E0 E5 FF FF";
// Sends a BuildingTearDownEvent (effects only) through the event writer at ctx+0xC8.
// Contexts without that writer would crash it; the hook skips the event then.
static const char* SIG_TEARDOWN_EVENT = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 48 8B B1 C8 00 00 00";
// Turns per-material removed volumes into refund entries (called by RemoveShape).
static const char* SIG_REFUND_BUILD = "4C 8B DC 55 53 56 57 41 55 41 57 49 8D 6B A1 48 81 EC E8 00 00 00 49 8B 58 08";
// Density check inside the voxel blit callback; patched to run src/cave.S.
static const char* SIG_BLIT_CHECK =
    "41 8B 4F 08 41 8B 47 04 41 03 CD 41 0F AF C8 03 C5 41 0F AF 44 24 10 4D 8B 04 24 03 C8 41 03 0F 03 CF 8B D1 "
    "45 0F B6 1C 48 41 80 FB 80 0F 83 ?? ?? ?? ?? 4D 3B 5A 28 0F 83 ?? ?? ?? ?? 49 8B 42 20 41 80 3C 03 00 0F 84 ?? ?? ?? ?? "
    "41 80 BA A0 00 00 00 00 0F 84 ?? ?? ?? ??";
// patch the existing-material checks (cmp r11b,80h ... je skip): 35 bytes at +0x29
static const int kBlitPatchOff = 0x29, kBlitPatchLen = 35, kBlitSkipOff = 0x104, kBlitWriteOff = 0xdb;
// Builds the 256-entry "may write over this material" table for a placement.
// Begin undo step (storage ring of 5 steps, 32 records each).
static const char* SIG_BEGIN_UNDO = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B 82 00 0A 00 00";
// Drop the oldest undo step.
static const char* SIG_POP_UNDO = "48 89 5C 24 10 48 89 6C 24 18 57 41 56 41 57 48 83 EC 30 48 8B B9 08 0A 00 00";
// HUD building update (plays the build sounds); hooked to play the mod's sounds.
static const char* SIG_HUD_UPDATE = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 88 FE FF FF 48 81 EC 78 02 00 00";
// mov rcx,[rip+uiCtx] ... movups xmm0,[rax+12B88h] (buildUndoSuccessful) ... call PlayUiSound
static const char* SIG_UI_SOUND = "48 8B 0D ?? ?? ?? ?? 44 89 B3 DC 1B 03 00 40 88 B3 D9 1B 03 00 48 8B 81 C8 00 00 00 0F 10 80 88 2B 01 00 0F 29 44 24 20 E8";
// builds the cursor query (snapping etc.) from the equipped item
static const char* SIG_CURSOR_QUERY = "48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8B EC 48 83 EC 30 4D 8B F1";
// place cursor transform from the aim ray
static const char* SIG_CURSOR_XF = "48 8B C4 55 53 56 57 41 56 48 8D 6C 24 90 48 81 EC 70 01 00 00 0F 29 70 C8";
// System dependency copy, and the dismantle system's call of it (writer at +0x180)
static const char* SIG_SYS_DEPS = "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 4C 8B 09 48 8B D9";
static const char* SIG_DISM_DEPS = "41 B8 90 01 00 00 48 8D 95 F0 01 00 00 4C 8B E9 E8";
// Client cursor hover highlight (runs every frame)
static const char* SIG_HOVER_FX = "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 70 F2 FF FF";
static const char* SIG_WRITE_TABLE = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 C7 44 24 40 00 00 00 00 49 8B D8";
// Item id of a building material (used to price a shape).
static const char* SIG_MATERIAL_ITEM = "48 83 EC 28 44 0F B6 DA 41 8D 43 FF 3C 7E 77 74 4C 8B 49 38 4D 85 C9 74 3B";

// Cost list each shape action appends to (0x18-byte entries).
struct CostList { void* data; uint64_t count; uint64_t cap; void* grow; };
struct CostEntry { uint8_t b[0x18]; };

typedef uint8_t (*PlaceFn)(CostList* list, void* ctx, void* shapeState, void* playerData, CursorXf* xf);
typedef uint8_t (*RemoveFn)(CostList* list, void* ctx, void* shapeState, Aabb* box, IntXf* xf, void* filter, uint8_t check);
typedef Aabb*   (*BoundsFn)(Aabb* out, void* shapeState, const float* pos4, const float* rot4, float scale);
typedef uint8_t (*PayFn)(uint8_t* mode, CostList* list, uint8_t consume);
// Commit record the game builds after payment: position (metres), rotation,
// player entity id, flag. Same layout for the remove commit (first 0x1c bytes).
struct Commit { float pos[3]; float rot[4]; uint32_t player; uint8_t flag; uint8_t pad[7]; };
static_assert(sizeof(Commit) == 0x28, "commit size");
typedef uint8_t  (*CommitFn)(void* ctx, Commit* c, uint32_t shape);
// Items a material costs for a volume: ceil(volume / (exchangeRate * 8)).
typedef uint32_t (*MaterialItemFn)(void* registry, uint8_t material, float volume);

static PlaceFn  g_placeOrig = nullptr;   // trampoline
static PayFn    g_payOrig = nullptr;     // trampoline
static CommitFn g_placeCommitOrig = nullptr;   // trampoline

static CommitFn g_removeCommit = nullptr;

// ---- replace blit (src/cave.S)
extern "C" uint8_t bt_cave_start[], bt_cave_end[], bt_cave_mode[], bt_cave_cont[], bt_cave_skip[], bt_cave_write[],
                   bt_cave_vols[], bt_cave_allow[], bt_cave_hits[];
static uint8_t* g_caveAllow = nullptr;
static volatile uint32_t* g_caveHits = nullptr;
// Materials Replace may repaint: those a normal removal could remove at that spot
// (from a removal dry run), so protected world blocks are never touched.
static uint8_t g_allow[256];
static volatile DWORD g_tableForceTid = 0;
typedef void (*WriteTableFn)(uint8_t* table, void* ctx, void* state);
static WriteTableFn g_writeTableOrig = nullptr;
static void write_table_hook(uint8_t* table, void* ctx, void* state) {
    g_writeTableOrig(table, ctx, state);
    if (table && g_tableForceTid == GetCurrentThreadId())
        for (int m = 1; m < 0x80; m++) if (g_allow[m]) table[m] = 1;
}
static volatile uint8_t* g_caveMode = nullptr;   // in the runtime copy
static uint32_t* g_caveVols = nullptr;
enum { STYLE_OVERLAP = 1, STYLE_FULL = 2 };
static volatile LONG g_replaceStyle = STYLE_FULL;
typedef void (*TearDownFn)(void* ctx, void* a, void* b, uint32_t c);
static TearDownFn g_tearDownOrig = nullptr;
static void teardown_hook(void* ctx, void* a, void* b, uint32_t c) {
    if (!ctx || !*(void**)((uint8_t*)ctx + 0xc8)) return;   // no event writer: skip the effect event
    g_tearDownOrig(ctx, a, b, c);
}
static MaterialItemFn g_materialItem = nullptr;
static RemoveFn g_remove = nullptr;      // trampoline once hooked
static volatile DWORD g_ownRemoveTid = 0;  // set while the mod itself calls RemoveShape

// Fields of the dismantle system's action context the placement context lacks.
// RemoveCommit needs them (+0x120 is the ColliderSystem, used to clear props in
// the removed volume). Captured from the game's own dismantle dry run, per world.
static void flash(const char* msg);
// Removed volume of the mod's own removal dry runs (summed over all materials).
struct VolList { float* data; uint64_t count; };
typedef void (*RefundBuildFn)(void* list, void* ctx, VolList* vols);
static RefundBuildFn g_refundBuildOrig = nullptr;
static float g_removedVolume = 0.f;
static float g_remVols[256];
static void refund_build_hook(void* list, void* ctx, VolList* vols) {
    if (g_ownRemoveTid == GetCurrentThreadId()) {
        // the mod's removal dry run: note what is removable, create no refund entries
        if (vols && vols->data)
            for (uint64_t i = 1; i < vols->count && i < 0x100; i++) {
                float v = vols->data[i]; v = v < 0 ? -v : v;
                g_remVols[i] += v; g_removedVolume += v;
            }
        return;
    }
    g_refundBuildOrig(list, ctx, vols);
}


static BoundsFn g_bounds = nullptr;
static bool     g_cursorLayoutOk = false;
static bool     g_worldLayoutOk = false;
static uint8_t* g_base = nullptr;

// Stand-in for the player's TerraformingLootMemory (0x808 bytes), which the
// removal code expects at ctx+0x40 but the placement system does not provide.
alignas(16) static uint8_t g_lootMemory[0x808];

// Refunds owed for blocks removed by Replace, paid back right after the game
// charges the placement (same payment function, consume=0).
static const int kMaxRefund = 8192;
static CostEntry g_refund[kMaxRefund];
static volatile LONG g_refundN = 0;
static CostList* volatile g_refundList = nullptr;
static volatile LONG g_refundDropped = 0;

// ------------------------------------------------------------------- state
enum { MODE_PREFAB = 0, MODE_AREA = 1 };
static volatile LONG g_mode = MODE_PREFAB;
static volatile LONG g_replace = 0;
static volatile LONG g_corners = 0;      // corners captured so far (0..2)
static volatile LONG g_cancelReq = 0;
static volatile LONG g_active = 0;       // hooks installed
static volatile DWORD g_flashUntil = 0;  // overlay transient message
static char g_flash[128];

static void flash(const char* msg) {
    lstrcpynA(g_flash, msg, sizeof(g_flash));
    g_flashUntil = GetTickCount() + 2500;
}
// Sounds: the game's own UI sounds (FbUiBundle.sounds), played on the HUD thread.
// Sound=1 game sounds, 2 = Windows sounds, 0 = off.
enum { SND_CORNER = 0, SND_DONE, SND_FAIL, SND_TOGGLE, SND_COUNT };
static uint8_t** g_uiCtxSlot = nullptr;           // global UI context pointer
typedef void (*PlayUiSoundFn)(void* ctx, const void* soundRef);
static PlayUiSoundFn g_playUiSound = nullptr;
static volatile LONG g_sndQueue[SND_COUNT];
static volatile DWORD g_hudTick = 0;      // pending plays per sound
static volatile DWORD g_sndTick[SND_COUNT];
// offsets inside FbUiBundle (sounds @0x12060: menu @+0x1e0, hud @+0x9e0)
static const uint32_t kSndOffset[SND_COUNT] = {
    0x12320,    // menu.highlightChange
    0x12330,    // menu.entryTrigger
    0x12b70,    // hud.buildActionFailed
    0x12310,    // menu.tabChange
};
static void beep(UINT type) {
    if (!g_cfg.sound) return;
    int k = type == MB_ICONHAND ? SND_FAIL : type == MB_ICONASTERISK ? SND_DONE : type == MB_ICONQUESTION ? SND_TOGGLE : SND_CORNER;
    if (g_cfg.sound == 1 && g_playUiSound && GetTickCount() - g_hudTick < 2000) { g_sndTick[k] = GetTickCount(); InterlockedExchange(&g_sndQueue[k], 1); return; }
    MessageBeep(type);
}
// called on the HUD thread
static void drain_sounds() {
    if (!g_playUiSound || !g_uiCtxSlot) return;
    for (int k = 0; k < SND_COUNT; k++) {
        if (!g_sndQueue[k] || !InterlockedExchange(&g_sndQueue[k], 0)) continue;
        if (GetTickCount() - g_sndTick[k] > 1000) continue;            // stale
        uint8_t* ctx = *g_uiCtxSlot;
        if (!ctx) continue;
        uint8_t* bundle = *(uint8_t**)(ctx + 0xc8);
        if (!bundle) continue;
        alignas(16) uint8_t ref[16];
        memcpy(ref, bundle + kSndOffset[k], 16);
        g_playUiSound(ctx, ref);
    }
}
// ---- free placement
// 28bc80(out, sys, query, ray) turns the aim ray into the place cursor transform
// {int64 pos[3], float rot[4] @0x18, bool valid @0x28}. With free placement on and a
// building shape selected (query mode 4) the cursor hovers in front of the camera at
// g_hoverDist instead of attaching to the surface hit by the ray, snapped to the
// 0.5 m building grid. sys+0x640 = camera position (fixed point), sys+0x658 = camera
// rotation quaternion (forward = third matrix row, see 42880).
static volatile LONG g_hover = 0;
static volatile LONG g_fill = 0;              // core::FILL_*
static float g_hoverDist = 4.f;
static float g_lastRot[4] = { 0.f, 0.f, 0.f, 1.f };
static volatile LONG g_cxLogged = 0;
typedef uint8_t* (*CursorXfFn)(uint8_t* out, uint8_t* sys, uint8_t* q, uint8_t* ray);
static CursorXfFn g_cxOrig = nullptr;
static int64_t round_grid(double v, double grid) {           // v, grid in fixed point
    double k = v / grid;
    double r = (double)(int64_t)(k < 0 ? k - 0.5 : k + 0.5);
    return (int64_t)(r * grid);
}
static uint8_t* cursor_xf_hook(uint8_t* out, uint8_t* sys, uint8_t* q, uint8_t* ray) {
    uint8_t* r = g_cxOrig(out, sys, q, ray);
    if (!out || !sys || !q || q[3] != 4) return r;
    float* rot = (float*)(out + 0x18);
    bool valid = out[0x28] != 0;
    if (valid) {
        float n = rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3];
        if (n > 0.9f && n < 1.1f) for (int k = 0; k < 4; k++) g_lastRot[k] = rot[k];
    }
    if (!g_hover) return r;
    const int64_t* cam = (const int64_t*)(sys + 0x640);
    const float* cq = (const float*)(sys + 0x658);
    float x = cq[0], y = cq[1], z = cq[2], w = cq[3];
    float f[3] = { 2.f * (x * z + w * y), 2.f * (y * z - w * x), 1.f - 2.f * (x * x + y * y) };
    if (g_cfg.log && InterlockedCompareExchange(&g_cxLogged, 1, 0) == 0)
        LOG(.str("free placement: cam=(").fix(cam[0]).c(',').fix(cam[1]).c(',').fix(cam[2]).str(") fwd=(")
            .f(f[0]).c(',').f(f[1]).c(',').f(f[2]).str(") sys678=").f(*(float*)(sys + 0x678))
            .str(" vanilla valid=").i(valid).str(" ray type=").u(ray ? ray[0] : 255));
    const double M = 4294967296.0, grid = 0.5 * M;
    int64_t* pos = (int64_t*)out;
    for (int a = 0; a < 3; a++) pos[a] = round_grid((double)cam[a] + (double)f[a] * g_hoverDist * M, grid);
    for (int k = 0; k < 4; k++) rot[k] = g_lastRot[k];
    out[0x28] = 1;
    return r;
}

typedef void (*HudUpdateFn)(void* a, void* b, void* c, void* d);
static HudUpdateFn g_hudOrig = nullptr;
static void hud_hook(void* a, void* b, void* c, void* d) { g_hudTick = GetTickCount(); drain_sounds(); g_hudOrig(a, b, c, d); }

// ---------------------------------------------------------------- utilities
static bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof(mi))) return false;
    if (mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const uint8_t*)p + n <= (const uint8_t*)mi.BaseAddress + mi.RegionSize;
}

static uint8_t* g_textBase; static size_t g_textSize;

static void find_text() {
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    auto* nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            g_textBase = g_base + sec[i].VirtualAddress;
            g_textSize = sec[i].Misc.VirtualSize;
        }
    }
}

static uint8_t* find_unique(const char* name, const char* sig) {
    const uint8_t* hits[4];
    int n = core::scan(g_textBase, g_textSize, sig, hits, 4);
    if (n >= 1) LOG(.str("signature ").str(name).str(": ").i(n).str(n == 1 ? " match at rva " : " matches, first at rva ").x((uint64_t)(hits[0] - g_base)));
    else LOG(.str("signature ").str(name).str(": not found"));
    return n == 1 ? (uint8_t*)hits[0] : nullptr;
}

// A function this mod hooks may already be hooked by another mod (its first bytes
// replaced by a jump), so its signature no longer matches. Retry with the first
// `stolen` bytes as wildcards and accept a match whose start is a jump (E9 rel32 or
// FF 25 [rip]); hook() then chains to the other mod's hook.
static uint8_t* find_hookable(const char* name, const char* sig, int stolen) {
    uint8_t* p = find_unique(name, sig);
    if (p) return p;
    char pat[512]; int o = 0, tok = 0;
    for (const char* q = sig; *q && o < (int)sizeof(pat) - 4; ) {
        while (*q == ' ') q++;
        if (!*q) break;
        const char* e = q; while (*e && *e != ' ') e++;
        if (o) pat[o++] = ' ';
        if (tok < stolen) { pat[o++] = '?'; pat[o++] = '?'; }
        else for (const char* c = q; c < e && o < (int)sizeof(pat) - 2; c++) pat[o++] = *c;
        tok++; q = e;
    }
    pat[o] = 0;
    const uint8_t* hits[8];
    int n = core::scan(g_textBase, g_textSize, pat, hits, 8), k = 0;
    uint8_t* found = nullptr;
    for (int i = 0; i < n && i < 8; i++) {
        const uint8_t* h = hits[i];
        if (h[0] == 0xE9 || (h[0] == 0xFF && h[1] == 0x25)) { found = (uint8_t*)h; k++; }
    }
    if (k == 1) {
        LOG(.str("  ").str(name).str(": found at rva ").x((uint64_t)(found - g_base))
            .str(" but already hooked by another program (starts with a jump); chaining to it"));
        return found;
    }
    return nullptr;
}

// ------------------------------------------------------------------ hooking
static uint8_t* alloc_near(uint8_t* target, size_t size) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t t = (uintptr_t)target;
    for (uintptr_t off = gran; off < 0x70000000; off += gran) {
        for (int dir = -1; dir <= 1; dir += 2) {
            uintptr_t a = dir < 0 ? (t > off ? t - off : 0) : t + off;
            if (!a) continue;
            a &= ~(gran - 1);
            void* p = VirtualAlloc((void*)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return (uint8_t*)p;
        }
    }
    return nullptr;
}

static void put_absjmp(uint8_t* at, void* to) {
    at[0] = 0xFF; at[1] = 0x25; at[2] = at[3] = at[4] = at[5] = 0;
    memcpy(at + 6, &to, 8);
}

// Hooks a function whose first `stolen` bytes are position-independent whole
// instructions (verified by the signature). Returns trampoline to the original.
static void* hook(uint8_t* target, int stolen, void* detour) {
    uint8_t* mem = alloc_near(target, 64);
    if (!mem) { LOG(.str("hook: no memory near target")); return nullptr; }
    uint8_t* relay = mem;            // jmp [rip] -> detour
    uint8_t* tramp = mem + 16;       // stolen bytes + jmp back
    put_absjmp(relay, detour);
    if (target[0] == 0xE9) {
        // already hooked by another program: our "original" is its jump target
        uint8_t* to = target + 5 + *(int32_t*)(target + 1);
        put_absjmp(tramp, to);
        stolen = 5;
    } else if (target[0] == 0xFF && target[1] == 0x25) {
        uint8_t* to = *(uint8_t**)(target + 6 + *(int32_t*)(target + 2));
        put_absjmp(tramp, to);
        stolen = 6;
    } else {
        memcpy(tramp, target, stolen);
        put_absjmp(tramp + stolen, target + stolen);
    }
    DWORD old;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old)) { LOG(.str("hook: VirtualProtect failed")); return nullptr; }
    uint8_t patch[16];
    int32_t rel = (int32_t)(relay - (target + 5));
    patch[0] = 0xE9; memcpy(patch + 1, &rel, 4);
    for (int k = 5; k < stolen; k++) patch[k] = 0x90;
    memcpy(target, patch, stolen);
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    return tramp;
}

// ------------------------------------------------------------ building logic
static const float kInv = (float)(1.0 / 4294967296.0);

// Bounding box of the current shape at a cursor transform, as the game computes it.
static void shape_bounds(void* state, const CursorXf* xf, Aabb* box) {
    float pos[4] = { (float)xf->pos[0] * kInv, (float)xf->pos[1] * kInv, (float)xf->pos[2] * kInv, 0.f };
    float rot[4] = { xf->rot[0], xf->rot[1], xf->rot[2], xf->rot[3] };
    memset(box, 0, sizeof(*box));
    g_bounds(box, state, pos, rot, xf->scale);
}
// World centre of a shape box (fixed point), snapped to 1/16 m against float noise.
// The cursor position is not the shape's centre and where it sits inside the shape
// depends on the rotation (aimed surface), so area corners are stored as centres.
static I3 box_centre(const Aabb& b) {
    int64_t r[3];
    for (int a = 0; a < 3; a++) {
        double c = ((double)b.mn[a] + (double)b.mx[a]) * 0.5 * 16.0;
        double k = (double)(int64_t)(c < 0 ? c - 0.5 : c + 0.5);
        r[a] = (int64_t)(k * (4294967296.0 / 16.0));
    }
    return I3{ r[0], r[1], r[2] };
}

// How the game builds a shape action (all in the server world):
//   1. PlaceShape / RemoveShape: dry runs that measure the shape and append cost
//      (or refund) entries to the click's cost list. Nothing is written.
//   2. Payment of the cost list.
//   3. PlaceCommit (once, at the click position): the real write, which also
//      records undo and replicates the action to the player's client.
// The mod prices every shape of an action in step 1 and, in step 3, turns the
// single commit into one RemoveCommit + PlaceCommit per shape.

static CostEntry* list_push(CostList* list) {
    if (list->count + 1 > list->cap) {
        typedef uint8_t (*GrowFn)(CostList*, int64_t, uint64_t);
        GrowFn grow = (GrowFn)list->grow;
        if (!grow || !grow(list, -(int64_t)(list->count + 1), 0)) return nullptr;
    }
    return (CostEntry*)list->data + list->count++;
}

// Cost entry exactly as PlaceShape appends it after a successful dry run.
// Needed when Replace targets full voxels: the dry run then measures nothing.
static bool add_place_cost(CostList* list, void* ctx, uint8_t* pdata, float volume) {
    if (!g_materialItem || !pdata || volume <= 0.f) return false;
    uint32_t item = g_materialItem(*(void**)((uint8_t*)ctx + 0xf8), pdata[0x454], volume);
    if (!item) return true;            // too small to cost anything
    CostEntry* e = list_push(list);
    if (!e) return false;
    memcpy(e->b, pdata + 4, 16);
    memcpy(e->b + 0x10, pdata, 4);
    memcpy(e->b + 0x14, &item, 4);
    return true;
}

// Dry-run removal at `xf`: moves the refund entries into g_refund (paid back in
// pay_hook once the action is paid), so the cost list only charges the new material.
// Returns the removed volume.
static float price_removal(CostList* list, void* ctx, void* state, const CursorXf* xf) {
    if (!g_remove || !g_bounds || !list || !ctx) return 0.f;
    Aabb box; shape_bounds(state, xf, &box);
    IntXf ix; memset(&ix, 0, sizeof(ix));
    ix.pos[0] = core::centre_fixed(box.mn[0], box.mx[0]);
    ix.pos[1] = core::centre_fixed(box.mn[1], box.mx[1]);
    ix.pos[2] = core::centre_fixed(box.mn[2], box.mx[2]);
    for (int k = 0; k < 4; k++) ix.rot[k] = xf->rot[k];
    ix.scale[0] = ix.scale[1] = ix.scale[2] = 1.f;
    void** lootSlot = (void**)((uint8_t*)ctx + 0x40);
    void* savedLoot = *lootSlot;
    if (!savedLoot) *lootSlot = g_lootMemory;
    uint64_t before = list->count;
    g_ownRemoveTid = GetCurrentThreadId();
    g_removedVolume = 0.f;
    g_remove(list, ctx, state, &box, &ix, nullptr, 0);
    float removed = g_removedVolume;
    g_ownRemoveTid = 0;
    *lootSlot = savedLoot;
    if (list->count > before) {
        CostEntry* e = (CostEntry*)list->data;
        for (uint64_t k = before; k < list->count; k++) {
            if (g_refundN < kMaxRefund) g_refund[g_refundN++] = e[k];
            else g_refundDropped++;
        }
        g_refundList = list;
        list->count = before;
    }
    return removed;
}

// Wraps the game's payment step. When it charges a list we removed blocks for,
// the removed materials are refunded through the same function.
static volatile LONG g_payDiag = 0;
// ---- undo of Replace
// The game's undo of a placement step refunds the step's cost list and restores the
// old voxels, but knows nothing about the old material the mod gave back. The mod
// remembers, per undo step, what it refunded for the shapes that undo covers and
// takes exactly that back (charge first; if it is not in the inventory the undo is
// refused like any undo the game cannot pay for).
static const int kMaxPaid = 64;
static CostEntry g_paid[kMaxPaid]; static int g_paidN = 0; static DWORD g_paidTick = 0;
struct UndoRefund { CostList* list; void* data; uint64_t count; int n; CostEntry e[kMaxPaid]; };
static UndoRefund g_undoRefunds[16]; static int g_undoNext = 0;
static void undo_refund_add(CostList* list, const CostEntry* e, int n) {
    if (!list || n <= 0) return;
    for (auto& r : g_undoRefunds) if (r.list == list) r.list = nullptr;       // slot reused
    UndoRefund& r = g_undoRefunds[g_undoNext++ & 15];
    r.list = list; r.data = list->data; r.count = list->count;
    r.n = n < kMaxPaid ? n : kMaxPaid; memcpy(r.e, e, (size_t)r.n * sizeof(CostEntry));
}
static UndoRefund* undo_refund_find(CostList* list) {
    for (auto& r : g_undoRefunds)
        if (r.list && r.list == list && r.data == list->data && r.count == list->count) return &r;
    return nullptr;
}
// ---- PickupEvent writer for refund pop-ups
// The placement system has no PickupEvent writer; the dismantle system gets one as a
// system dependency (block copied by 9bc1a0 to rbp+0x1f0, writer at +0x180). The mod
// records it per world (*system context) every time the dismantle system runs.
struct WriterRec { void* world; void* writer; };
static WriterRec g_pickWriters[8];
static uint8_t* g_dismDepsRet = nullptr;     // return address of the dismantle's 9bc1a0 call
typedef uint64_t (*DepsFn)(void* sys, uint8_t* out, uint32_t size);
static DepsFn g_depsOrig = nullptr;
static uint64_t deps_hook(void* sys, uint8_t* out, uint32_t size) {
    uint64_t r = g_depsOrig(sys, out, size);
    if ((uint8_t*)__builtin_return_address(0) == g_dismDepsRet && sys && out && size >= 0x188) {
        void* world = *(void**)sys, *w = *(void**)(out + 0x180);
        if (w) {
            int k = 0;
            for (; k < 8; k++) if (g_pickWriters[k].world == world || !g_pickWriters[k].world) break;
            if (k < 8) { g_pickWriters[k].world = world; g_pickWriters[k].writer = w; }
        }
    }
    return r;
}
static void* pickup_writer_for(uint8_t* inv) {
    void* sys = *(void**)inv;
    if (!sys || !readable(sys, 8)) return nullptr;
    void* world = *(void**)sys;
    for (int k = 0; k < 8; k++) if (g_pickWriters[k].world == world) return g_pickWriters[k].writer;
    return nullptr;
}
static uint8_t pay_hook(uint8_t* mode, CostList* list, uint8_t consume) {
    if (!consume && list && list != g_refundList) {
        if (UndoRefund* u = undo_refund_find(list)) {           // undo of a Replace step
            CostList take = { u->e, (uint64_t)u->n, (uint64_t)u->n, nullptr };
            u->list = nullptr;
            if (!g_payOrig(mode, &take, 1)) {
                LOG(.str("undo of replace refused: the old material it gave back is not in the inventory"));
                flash("Undo needs the replaced material back in your inventory");
                beep(MB_ICONHAND);
                return 0;
            }
            uint8_t r = g_payOrig(mode, list, consume);
            if (!r) g_payOrig(mode, &take, 0);                    // undo failed: give it back again
            LOG(.str("undo of replace: took back ").i(u->n).str(" refund entries, game refund ").i(r));
            return r;
        }
    }
    if (!consume && mode && mode[0] == 0 && list != g_refundList && InterlockedIncrement(&g_payDiag) <= 3) {
        uint8_t* inv = *(uint8_t**)(mode + 8);
        if (inv && readable(inv, 0x100))
            LOG(.str("game refund transaction: flags ").x(inv[0xb0]).str(" writer ").x(*(uint64_t*)(inv + 0xb8)));
    }
    uint8_t r = g_payOrig(mode, list, consume);
    if (list && list == g_refundList) {
        LONG n = g_refundN;
        if (r && consume && mode && mode[0] == 0 && n > 0) {
            CostList tmp = { g_refund, (uint64_t)n, (uint64_t)n, nullptr };
            // Inventory transaction of this action: flag bit 0 at +0xb0 makes the game
            // send PickupEvents (the "+N item" pop-up) for added items when the
            // transaction is committed; the placement system leaves it off.
            uint8_t* inv = *(uint8_t**)(mode + 8);
            if (inv && readable(inv, 0x100)) {
                LOG(.str("  refund transaction: flags ").x(inv[0xb0]).str(" writer ").x(*(uint64_t*)(inv + 0xb8))
                    .str(" pending ").u(*(uint64_t*)(inv + 0xf8)));
                if (g_cfg.refundPopup) {
                    if (!*(void**)(inv + 0xb8)) *(void**)(inv + 0xb8) = pickup_writer_for(inv);
                    if (*(void**)(inv + 0xb8)) inv[0xb0] |= 1;
                    else LOG(.str("  no pick-up writer known for this world yet"));
                }
            }
            uint8_t rr = g_payOrig(mode, &tmp, 0);
            if (rr) { g_paidN = n < kMaxPaid ? n : kMaxPaid; memcpy(g_paid, g_refund, (size_t)g_paidN * sizeof(CostEntry)); g_paidTick = GetTickCount(); }
            if (inv && readable(inv, 0x100)) LOG(.str("  after refund: pending ").u(*(uint64_t*)(inv + 0xf8)));
            LOG(.str("refund: ").i(n).str(" entries -> ").str(rr ? "ok" : "not all refunded (inventory full?)")
                .str(g_refundDropped ? " (some dropped: over limit)" : ""));
        } else if (n > 0) {
            LOG(.str("refund skipped: charge result=").i(r).str(" consume=").i(consume));
        }
        g_refundN = 0; g_refundDropped = 0; g_refundList = nullptr;
    }
    return r;
}

// The game's live ServerCursor component, derived from the caller's frame.
static uint8_t* cursor_component(CursorXf* xf) {
    if (!g_cursorLayoutOk) return nullptr;
    uint8_t** slot = (uint8_t**)((uint8_t*)xf - 0x118);
    if (!readable(slot, 8)) return nullptr;
    uint8_t* cc = *slot;
    if (!cc || !readable(cc + 0x300, 0x70)) return nullptr;
    // must be where the caller copied the transform from
    if (memcmp(cc + 0x300, xf, sizeof(CursorXf)) != 0 && memcmp(cc + 0x338, xf, sizeof(CursorXf)) != 0)
        return nullptr;
    return cc;
}

static void log_xf(const char* what, const CursorXf* x) {
    LOG(.str(what).str(" pos=(").fix(x->pos[0]).c(',').fix(x->pos[1]).c(',').fix(x->pos[2])
        .str(") rot=(").f(x->rot[0]).c(',').f(x->rot[1]).c(',').f(x->rot[2]).c(',').f(x->rot[3])
        .str(") scale=").f(x->scale));
}

// ---- pending action: priced in place_hook, written in place_commit_hook
static const int kMaxPending = 20000;
static int64_t g_pendPos[kMaxPending][3];
struct Pending { void* ctx; DWORD tid; DWORD tick; int n; bool replace; LONG style; float rot[4]; uint32_t shape; CostList* list; };
static uint32_t g_pendCost[kMaxPending + 1];     // cost list count before shape i (n = end)
static const int kVolSnaps = 40;                 // replace: old-material voxel counts after shape i
static uint32_t g_volSnap[kVolSnaps][256];

// ---- undo budget
// Each committed shape adds one undo record to the action's undo step: at most 32
// per step, taken from a small shared record pool (+0xa10) and snapshot memory
// (+0xa50). When those are full the game drops the oldest undo steps (492480); if
// the only step left is the one being written, the pool is corrupted. So the mod
// records undo only while the current step's own records clearly fit, commits the
// rest without undo, and leaves the step only the cost entries of the recorded
// shapes (undo refunds exactly what it takes back).
struct UndoSplit {
    uint8_t* ctx; uint8_t* storage; uint8_t* step0; CostList* list; const uint32_t* cost; int n;
    bool costsOk, stop, lost;
    int recorded; uint64_t bytes, maxRec, headBefore;
};
static UndoSplit* volatile g_activeSplit = nullptr;
static volatile DWORD g_activeSplitTid = 0;
static CostEntry g_tmpEntries[8192];
typedef void (*PopUndoFn)(uint8_t* storage);
static PopUndoFn g_popUndoOrig = nullptr;

static void pop_undo_hook(uint8_t* storage) {
    UndoSplit* u = g_activeSplit;
    if (u && g_activeSplitTid == GetCurrentThreadId() && storage == u->storage) {
        uint64_t head = *(uint64_t*)(storage + 0xa00), tail = *(uint64_t*)(storage + 0xa08);
        if (head != tail && storage + (tail % 5) * 0x200 == u->step0) { u->lost = true; u->stop = true; }
    }
    g_popUndoOrig(storage);
}

static void undo_begin(UndoSplit& u, void* ctx, CostList* list, const uint32_t* cost, int n) {
    memset(&u, 0, sizeof(u));
    u.ctx = (uint8_t*)ctx; u.n = n; u.cost = cost; u.list = list;
    u.step0 = *(uint8_t**)(u.ctx + 0xf0);
    u.storage = *(uint8_t**)(u.ctx + 0xe8);
    u.costsOk = u.step0 && list && (uint8_t*)list == u.step0 + 0x110 && list->data && list->count >= cost[n] &&
                cost[0] <= cost[n] && list->count <= 8192;
    if (n > 1 && u.step0 && u.storage && readable(u.storage + 0xa00, 0x80) && g_popUndoOrig) {
        g_activeSplitTid = GetCurrentThreadId(); g_activeSplit = &u;
    } else if (n > 1) u.stop = true;               // cannot check the budget: first shape only
}

// before committing shape i: record undo for it only if it surely fits
static void undo_before(UndoSplit& u, int i) {
    if (!u.step0) return;
    if (u.stop && i > 0) { *(uint8_t**)(u.ctx + 0xf0) = nullptr; return; }
    if (i > 0) {
        uint64_t stepRecs = *(uint64_t*)(u.step0 + 0x100) - *(uint64_t*)(u.step0 + 0x108);
        uint32_t poolUsed = *(uint32_t*)(u.storage + 0xa28), poolCap = *(uint32_t*)(u.storage + 0xa38);
        uint64_t memCap = *(uint64_t*)(u.storage + 0xa60);
        (void)poolUsed;
        bool fits = stepRecs < 30 && (uint64_t)u.recorded + 2 < poolCap &&
                    u.bytes + 2 * u.maxRec + 64 < memCap / 2;
        if (!fits) { u.stop = true; *(uint8_t**)(u.ctx + 0xf0) = nullptr; return; }
    }
    u.headBefore = *(uint64_t*)(u.storage + 0xa68);
    u.recorded++;
}
static void undo_after(UndoSplit& u) {
    if (!u.step0 || !u.storage || *(uint8_t**)(u.ctx + 0xf0) == nullptr) return;
    uint64_t d = *(uint64_t*)(u.storage + 0xa68) - u.headBefore;
    if (d < (1ull << 32)) { u.bytes += d; if (d > u.maxRec) u.maxRec = d; }
}

// after the last commit: restore the context, keep only the recorded shapes' costs
static void undo_end(UndoSplit& u) {
    g_activeSplit = nullptr; g_activeSplitTid = 0;
    if (!u.step0) return;
    *(uint8_t**)(u.ctx + 0xf0) = u.step0;
    int kept = -1;
    if (u.recorded < u.n && u.costsOk && !u.lost && u.list->data && u.list->count >= u.cost[u.n]) {
        CostEntry* e = (CostEntry*)u.list->data;
        uint64_t total = u.list->count;
        kept = 0;
        auto keep = [&](const CostEntry& x) { if (kept < 8192) g_tmpEntries[kept++] = x; };
        for (uint32_t q = 0; q < u.cost[u.recorded]; q++) keep(e[q]);
        for (uint64_t q = u.cost[u.n]; q < total; q++) keep(e[q]);
        memcpy(e, g_tmpEntries, (size_t)kept * sizeof(CostEntry));
        u.list->count = (uint64_t)kept;
    }
    if (u.n > 1)
        LOG(.str("  undo: ").i(u.recorded).str(" of ").i(u.n).str(" shapes recorded, bytes ").u(u.bytes)
            .str(" max ").u(u.maxRec).str(" pool cap ").u(u.storage ? *(uint32_t*)(u.storage + 0xa38) : 0)
            .str(" mem cap ").u(u.storage ? *(uint64_t*)(u.storage + 0xa60) : 0)
            .str(u.lost ? " STEP DROPPED BY GAME" : "").str(kept >= 0 ? " (step costs trimmed)" : ""));
}

static Pending g_pend;

static void pend_begin(void* ctx, const CursorXf& tmpl, bool replace, uint32_t shape) {
    g_pend.ctx = ctx; g_pend.tid = GetCurrentThreadId(); g_pend.tick = GetTickCount();
    g_pend.n = 0; g_pend.replace = replace; g_pend.shape = shape; g_pend.style = g_replaceStyle;
    memset(g_allow, 0, sizeof(g_allow));
    if (g_caveAllow) memset(g_caveAllow, 0, 256);
    if (g_caveHits) { g_caveHits[0] = 0; g_caveHits[1] = 0; }
    for (int k = 0; k < 4; k++) g_pend.rot[k] = tmpl.rot[k];
}
static void pend_add(const int64_t* pos) {
    if (g_pend.n < kMaxPending) { memcpy(g_pendPos[g_pend.n], pos, 24); g_pend.n++; }
}

// Prices one shape of the action at `xf` and queues it for the commit.
// With Replace, the dry run runs with the replace blit, so the game measures (and
// prices) exactly what will be written, and the old material of every repainted
// voxel is tallied in the cave's volume table for the refund.
static bool price_shape(CostList* list, void* ctx, void* state, void* pdata, CursorXf* xf, bool replace) {
    if (replace) {
        memset(g_remVols, 0, sizeof(g_remVols));
        price_removal(list, ctx, state, xf);                         // what is removable here
        for (int m = 1; m < 0x100; m++) if (g_remVols[m] > 0.f) { g_allow[m] = 1; g_caveAllow[m] = 1; }
        *g_caveMode = (uint8_t)g_pend.style;
        g_tableForceTid = GetCurrentThreadId();
    }
    if (g_pend.n < kMaxPending) g_pendCost[g_pend.n] = (uint32_t)list->count;
    g_pend.list = list;
    uint8_t r = g_placeOrig(list, ctx, state, pdata, xf);          // dry run
    if (replace) { *g_caveMode = 0; g_tableForceTid = 0; }
    pend_add(xf->pos);
    if (replace && g_pend.n <= kVolSnaps) memcpy(g_volSnap[g_pend.n - 1], g_caveVols, 256 * 4);
    return r != 0;
}

static void refund_reset() { memset(g_caveVols, 0, 256 * 4); }

// refund entries for given voxel counts (the game's refund builder into a local list)
static int build_refund_entries(void* ctx, const uint32_t* cnt, CostEntry* out, int max) {
    alignas(16) float vols[256]; float total = 0.f;
    for (int m = 0; m < 256; m++) { vols[m] = (float)cnt[m]; total += vols[m]; }
    vols[0] = 0.f;
    if (total <= 0.f || !g_refundBuildOrig) return 0;
    static CostEntry buf[256];
    CostList l = { buf, 0, 256, nullptr };
    VolList vl = { vols, 0x100 };
    void** lootSlot = (void**)((uint8_t*)ctx + 0x40);
    void* saved = *lootSlot; if (!saved) *lootSlot = g_lootMemory;
    g_refundBuildOrig(&l, ctx, &vl);
    *lootSlot = saved;
    int n = (int)(l.count < (uint64_t)max ? l.count : (uint64_t)max);
    memcpy(out, buf, (size_t)n * sizeof(CostEntry));
    return n;
}

// Turns the tallied old-material volumes into refund entries (the game's own
// refund builder) and moves them to g_refund, paid back once the action is paid.
static void refund_build(CostList* list, void* ctx) {
    alignas(16) float vols[256];
    float total = 0.f;
    for (int m = 0; m < 256; m++) { vols[m] = (float)g_caveVols[m]; total += vols[m]; }   // 1.0 per voxel
    vols[0] = 0.f;
    if (total <= 0.f || !g_refundBuildOrig) return;
    VolList vl = { vols, 0x100 };
    void** lootSlot = (void**)((uint8_t*)ctx + 0x40);
    void* savedLoot = *lootSlot;
    if (!savedLoot) *lootSlot = g_lootMemory;
    uint64_t before = list->count;
    g_refundBuildOrig(list, ctx, &vl);
    *lootSlot = savedLoot;
    if (list->count > before) {
        CostEntry* e = (CostEntry*)list->data;
        for (uint64_t k = before; k < list->count; k++) {
            if (g_refundN < kMaxRefund) g_refund[g_refundN++] = e[k];
            else g_refundDropped++;
        }
        g_refundList = list;
        list->count = before;
    }
    LOG(.str("  repainted volume ").f(total).str(" -> refund entries ").i(g_refundN));
}

static uint8_t place_commit_hook(void* ctx, Commit* c, uint32_t shape) {
    bool mine = g_pend.n > 0 && g_pend.ctx == ctx && g_pend.tid == GetCurrentThreadId() &&
                GetTickCount() - g_pend.tick < 3000;
    if (!mine) return g_placeCommitOrig(ctx, c, shape);
    Pending pd = g_pend;
    g_pend.n = 0;
    uint8_t any = 0;
    int placed = 0;
    uint32_t h0 = g_caveHits[0], h1 = g_caveHits[1];
    if (pd.replace) { *g_caveMode = (uint8_t)pd.style; g_tableForceTid = GetCurrentThreadId(); }
    UndoSplit us; undo_begin(us, ctx, pd.list, g_pendCost, pd.n);
    for (int i = 0; i < pd.n; i++) {
        undo_before(us, i);
        Commit pc = *c;
        for (int a = 0; a < 3; a++) pc.pos[a] = (float)g_pendPos[i][a] * kInv;
        for (int k = 0; k < 4; k++) pc.rot[k] = pd.rot[k];
        if (g_placeCommitOrig(ctx, &pc, shape)) { any = 1; placed++; }
        undo_after(us);
    }
    undo_end(us);
    *g_caveMode = 0; g_tableForceTid = 0;
    if (pd.replace && us.step0 && !us.lost && us.recorded > 0 && GetTickCount() - g_paidTick < 3000 && g_paidN > 0) {
        CostEntry e[kMaxPaid]; int ne = 0;
        if (us.recorded >= pd.n) { ne = g_paidN; memcpy(e, g_paid, (size_t)ne * sizeof(CostEntry)); }
        else if (us.recorded <= kVolSnaps) ne = build_refund_entries(ctx, g_volSnap[us.recorded - 1], e, kMaxPaid);
        undo_refund_add((CostList*)(us.step0 + 0x110), e, ne);
        LOG(.str("  undo will take back ").i(ne).str(" refund entries (").i(us.recorded).str(" of ").i(pd.n).str(" shapes)"));
        g_paidN = 0;
    }
    if (pd.replace) LOG(.str("  commit blit: voxels ").u(g_caveHits[0] - h0).str(" solid ").u(g_caveHits[1] - h1));
    LOG(.str("commit: ").i(pd.n).str(" shapes, written ").i(placed).str(pd.replace ? (pd.style == STYLE_OVERLAP ? " (replace overlap)" : " (replace full)") : ""));
    if (pd.n > 1) { Buf b; b.str("Placed ").i(placed).str(" shapes");
        if (us.recorded < pd.n) b.str("  |  undo covers ").i(us.recorded);
        flash(b.z()); }
    return any;
}

// ---- structure fill (server): price every shape, queue them all
static uint8_t structure_fill(CostList* list, void* ctx, void* state, void* pdata, const CursorXf& tmpl,
                              const I3* corners, bool replace) {
    Aabb box; shape_bounds(state, &tmpl, &box);
    float step[3] = { box.mx[0] - box.mn[0], box.mx[1] - box.mn[1], box.mx[2] - box.mn[2] };
    const int nc = g_cfg.corners;
    I3 c[3] = { corners[0], corners[1], nc == 3 ? core::lock_third(corners[0], corners[1], corners[2], (int64_t)(0.1 * core::kFix)) : corners[1] };
    core::Plan pl = {};
    const int fm = g_fill;
    bool ok = core::plan_fill(c, nc, step, 50000000, &pl);
    if (ok) { pl.total = core::fill_count(pl, fm); ok = pl.total <= g_cfg.maxShapes; }
    LOG(.str("fill: mode ").i(fm).str(" last corner (").fix(corners[2].x).c(',').fix(corners[2].y).c(',').fix(corners[2].z)
        .str(") locked to (").fix(c[2].x).c(',').fix(c[2].y).c(',').fix(c[2].z)
        .str(") step=(").f(step[0]).c(',').f(step[1]).c(',').f(step[2])
        .str(") counts=").i(pl.count[0]).c('x').i(pl.count[1]).c('x').i(pl.count[2]).str(" total=").i(pl.total)
        .str(ok ? "" : " REFUSED (over MaxShapes or invalid)"));
    if (!ok) {
        Buf b; b.str("Too big: ").i(pl.total).str(" shapes (max ").i(g_cfg.maxShapes).c(')');
        flash(b.z()); beep(MB_ICONHAND);
        return 0;
    }
    pend_begin(ctx, tmpl, replace, state ? *(uint32_t*)state : 0);
    if (replace) refund_reset();
    CursorXf xf = tmpl;
    const I3 tc = box_centre(box);                       // planned positions are centres
    const int64_t off[3] = { tmpl.pos[0] - tc.x, tmpl.pos[1] - tc.y, tmpl.pos[2] - tc.z };
    long priced = 0;
    for (int jj = 0; jj < pl.count[1]; jj++)          // bottom layer first
        for (int i = 0; i < pl.count[0]; i++)
            for (int k = 0; k < pl.count[2]; k++) {
                int j = pl.stepFix[1] >= 0 ? jj : pl.count[1] - 1 - jj;
                if (!core::fill_keep(pl, i, j, k, fm)) continue;
                I3 p = core::plan_pos(pl, i, j, k);
                xf.pos[0] = p.x + off[0]; xf.pos[1] = p.y + off[1]; xf.pos[2] = p.z + off[2];
                if (price_shape(list, ctx, state, pdata, &xf, replace)) priced++;
            }
    g_pendCost[g_pend.n] = (uint32_t)list->count;
    if (replace) LOG(.str("  pricing blit: voxels ").u(g_caveHits[0]).str(" solid ").u(g_caveHits[1]));
    if (replace && priced) refund_build(list, ctx);
    LOG(.str("fill: priced ").i(priced).str(" of ").i(pl.total).str(" cost entries=").u(list->count)
        .str(" refund entries=").i(g_refundN));
    if (!priced) { g_pend.n = 0; return 0; }
    return g_pend.n > 0 ? 1 : 0;
}

// The placement system runs once per game world: the authoritative server world
// (single player runs one in-process) and one or more client prediction copies.
enum { WORLD_UNKNOWN = 0, WORLD_SERVER = 1, WORLD_CLIENT = 2 };

static int world_of(CostList* list, CursorXf* xf) {
    if (!g_worldLayoutOk) return WORLD_UNKNOWN;
    uint8_t* frame = (uint8_t*)xf - 0xc0;          // caller's rbp
    if (!readable(frame + 0x50, 8) || !readable(frame + 0x220, 8)) return WORLD_UNKNOWN;
    uint8_t* undoStep = *(uint8_t**)(frame + 0x220);
    if (undoStep && (uint8_t*)list == undoStep + 0x110) return WORLD_SERVER;
    if ((uint8_t*)list == frame + 0x430) return WORLD_CLIENT;
    return WORLD_UNKNOWN;
}

// ---- corner state (server only)
static CRITICAL_SECTION g_stateLock;
static LONG g_sc = 0;            // corners set (0..2)
static I3   g_scPos[3];
enum { OP_BUILD = 0, OP_REMOVE = 1 };
static volatile LONG g_scOp = OP_BUILD;   // what the corners being collected are for
static void reset_corners() { g_sc = 0; g_corners = 0; }
// live size readout: shape step and which cursor the corners follow
static float g_scStep[3] = { 1.f, 1.f, 1.f };
static volatile LONG g_scCursorOff = 0x300;
static int64_t g_scOff[3];       // corner 1: shape centre - cursor position (live size readout)

// ---- aim hand-off
// Only client worlds hold the live "aimed at" cursor (+0x338); the server copy is
// the previous action. The server reads the client's live cursor component when
// that client's place cursor equals its own, else a per-click record.
static uint8_t* volatile g_clientCC = nullptr;
struct AimRec { int64_t place[3]; uint32_t shape; CursorXf aim; DWORD tick; };
static AimRec g_aim[16];
static int g_aimNext = 0;

static bool zero_pos(const CursorXf& x) { return !x.pos[0] && !x.pos[1] && !x.pos[2]; }

static void aim_record(const CursorXf* place, uint32_t shape, const CursorXf& aim) {
    AimRec& r = g_aim[g_aimNext++ & 15];
    memcpy(r.place, place->pos, sizeof(r.place)); r.shape = shape; r.aim = aim; r.tick = GetTickCount();
}

static bool aim_lookup(const CursorXf* place, uint32_t shape, CursorXf* out, const char** how) {
    uint8_t* c = g_clientCC;
    if (c && readable(c + 0x300, 0x70) && !memcmp(c + 0x300, place->pos, 0x18) &&
        !zero_pos(*(CursorXf*)(c + 0x338))) {
        *out = *(CursorXf*)(c + 0x338); *how = "client cursor"; return true;
    }
    DWORD now = GetTickCount();
    for (int k = 0; k < 16; k++) {
        const AimRec& r = g_aim[(g_aimNext - 1 - k) & 15];
        if (now - r.tick > 1500) continue;
        if (r.shape == shape && !memcmp(r.place, place->pos, sizeof(r.place))) { *out = r.aim; *how = "click record"; return true; }
    }
    // The client skips its own placement prediction when its ghost is blocked (e.g. a
    // big shape that overlaps blocks next to the aimed surface), so neither match above
    // exists. The client cursor component still holds the live aim: accept it when it
    // is close to the server's place cursor (the ghost sits next to the aimed shape).
    if (c && readable(c + 0x300, 0x70) && !zero_pos(*(CursorXf*)(c + 0x338))) {
        const CursorXf* a = (const CursorXf*)(c + 0x338);
        int64_t lim = (int64_t)6 << 32;                       // 6 m
        bool close = true;
        for (int k = 0; k < 3; k++) { int64_t d = a->pos[k] - place->pos[k]; if (d < -lim || d > lim) close = false; }
        if (close) { *out = *a; *how = "client cursor (near)"; return true; }
    }
    if (c && readable(c + 0x300, 0x70)) {
        const CursorXf* cp = (const CursorXf*)(c + 0x300); const CursorXf* ca = (const CursorXf*)(c + 0x338);
        LOG(.str("  aim lookup failed: client place (").fix(cp->pos[0]).c(',').fix(cp->pos[1]).c(',').fix(cp->pos[2])
            .str(") client aim (").fix(ca->pos[0]).c(',').fix(ca->pos[1]).c(',').fix(ca->pos[2]).c(')'));
    } else LOG(.str("  aim lookup failed: no client cursor known yet"));
    return false;
}

// ---- area removal (right click in Area mode)
// The dismantle system dry-runs RemoveShape at the aimed shape, pays the refund
// list and commits with RemoveCommit, all in its own context. In Area mode the
// dry run becomes a corner click (and the commit is suppressed) or, on the third
// corner, dry runs for every shape of the area plus one RemoveCommit per shape.
static CommitFn g_removeCommitOrig = nullptr;     // trampoline
struct RemovePending { void* ctx; DWORD tid, tick; int n; int64_t click[3]; bool suppress; CostList* list; };
static uint32_t g_rpendCost[kMaxPending + 1];
static RemovePending g_rpend;
static int64_t g_rpendPos[kMaxPending][3];

static bool is_server_ctx(void* ctx) { return ctx && *(void**)((uint8_t*)ctx + 0xf0) != nullptr; }

static uint8_t remove_hook(void* list, void* ctx, void* state, void* box, void* xfp, void* filter, uint8_t check) {
    if (g_ownRemoveTid == GetCurrentThreadId() || g_mode != MODE_AREA || !is_server_ctx(ctx) || !box || !xfp)
        return g_remove((CostList*)list, ctx, state, (Aabb*)box, (IntXf*)xfp, filter, check);
    IntXf* xf = (IntXf*)xfp;
    Aabb* bx = (Aabb*)box;
    EnterCriticalSection(&g_stateLock);
    if (InterlockedExchange(&g_cancelReq, 0)) reset_corners();
    if (g_scOp != OP_REMOVE) { reset_corners(); g_scOp = OP_REMOVE; }
    g_rpend.ctx = ctx; g_rpend.tid = GetCurrentThreadId(); g_rpend.tick = GetTickCount(); g_rpend.n = 0;
    LONG n = g_sc;
    if (n < 0 || n > g_cfg.corners - 1) n = 0;
    g_scPos[n] = I3{ xf->pos[0], xf->pos[1], xf->pos[2] };      // removal transforms are shape centres
    LOG(.str("remove click: corner ").i(n + 1).str(" at (").fix(xf->pos[0]).c(',').fix(xf->pos[1]).c(',').fix(xf->pos[2]).c(')'));
    if (n == 0) {
        for (int a = 0; a < 3; a++) g_scStep[a] = bx->mx[a] - bx->mn[a];
        g_scCursorOff = 0x338;
        uint8_t* cc = g_clientCC;
        if (cc && readable(cc + 0x338, 0x18)) for (int a = 0; a < 3; a++) g_scOff[a] = xf->pos[a] - ((int64_t*)(cc + 0x338))[a];
        else for (int a = 0; a < 3; a++) g_scOff[a] = 0;
    }
    if (n < g_cfg.corners - 1) {
        g_sc = n + 1; g_corners = g_sc;
        g_rpend.suppress = true;                       // the dismantle would still commit
        Buf b; b.str("Remove corner ").i(n + 1).str(" of ").i(g_cfg.corners).str(" set");
        flash(b.z()); beep(MB_OK);
        LeaveCriticalSection(&g_stateLock);
        return 0;
    }
    g_sc = 0; g_corners = 0;
    const int nc = g_cfg.corners;
    I3 c[3] = { g_scPos[0], g_scPos[1], nc == 3 ? core::lock_third(g_scPos[0], g_scPos[1], g_scPos[2], (int64_t)(0.1 * core::kFix)) : g_scPos[1] };
    float step[3] = { bx->mx[0] - bx->mn[0], bx->mx[1] - bx->mn[1], bx->mx[2] - bx->mn[2] };
    core::Plan pl = {};
    const int fm = g_fill;
    bool ok = core::plan_fill(c, nc, step, 50000000, &pl);
    if (ok) { pl.total = core::fill_count(pl, fm); ok = pl.total <= g_cfg.maxShapes; }
    LOG(.str("remove fill: mode ").i(fm).str(" step=(").f(step[0]).c(',').f(step[1]).c(',').f(step[2]).str(") counts=")
        .i(pl.count[0]).c('x').i(pl.count[1]).c('x').i(pl.count[2]).str(" total=").i(pl.total).str(ok ? "" : " REFUSED"));
    if (!ok) {
        g_rpend.suppress = true;
        Buf b; b.str("Too big: ").i(pl.total).str(" shapes (max ").i(g_cfg.maxShapes).c(')');
        flash(b.z()); beep(MB_ICONHAND);
        LeaveCriticalSection(&g_stateLock);
        return 0;
    }
    g_rpend.suppress = false;
    memcpy(g_rpend.click, xf->pos, sizeof(g_rpend.click));
    uint8_t any = 0;
    for (int jj = 0; jj < pl.count[1]; jj++)          // top layer first
        for (int i = 0; i < pl.count[0]; i++)
            for (int k = 0; k < pl.count[2]; k++) {
                int j = pl.stepFix[1] < 0 ? jj : pl.count[1] - 1 - jj;
                if (!core::fill_keep(pl, i, j, k, fm)) continue;
                I3 q = core::plan_pos(pl, i, j, k);
                IntXf ix = *xf; Aabb b2 = *bx;
                int64_t d[3] = { q.x - xf->pos[0], q.y - xf->pos[1], q.z - xf->pos[2] };
                for (int a = 0; a < 3; a++) {
                    ix.pos[a] += d[a];
                    float df = (float)d[a] * kInv; b2.mn[a] += df; b2.mx[a] += df;
                }
                if (g_rpend.n >= kMaxPending) continue;
                g_rpendCost[g_rpend.n] = list ? (uint32_t)((CostList*)list)->count : 0;
                if (g_remove((CostList*)list, ctx, state, &b2, &ix, filter, check)) any = 1;
                memcpy(g_rpendPos[g_rpend.n], ix.pos, 24); g_rpend.n++;
            }
    g_rpendCost[g_rpend.n] = list ? (uint32_t)((CostList*)list)->count : 0;
    g_rpend.list = (CostList*)list;
    LOG(.str("remove fill: priced ").i(g_rpend.n).str(" shapes, refund entries ").u(list ? ((CostList*)list)->count : 0));
    LeaveCriticalSection(&g_stateLock);
    beep(MB_ICONASTERISK);
    return any;
}

static uint8_t remove_commit_hook(void* ctx, Commit* c, uint32_t shape) {
    bool mine = g_rpend.ctx == ctx && g_rpend.tid == GetCurrentThreadId() && GetTickCount() - g_rpend.tick < 3000;
    if (!mine) return g_removeCommitOrig(ctx, c, shape);
    RemovePending rp = g_rpend;
    g_rpend.ctx = nullptr;
    if (rp.suppress) { LOG(.str("  corner click: dismantle commit skipped")); return 0; }
    uint8_t any = 0; int done = 0;
    UndoSplit us; undo_begin(us, ctx, rp.list, g_rpendCost, rp.n);
    for (int i = 0; i < rp.n; i++) {
        undo_before(us, i);
        Commit rc = *c;
        for (int a = 0; a < 3; a++) rc.pos[a] += (float)(g_rpendPos[i][a] - rp.click[a]) * kInv;
        if (g_removeCommitOrig(ctx, &rc, shape)) { any = 1; done++; }
        undo_after(us);
    }
    undo_end(us);
    LOG(.str("remove commit: ").i(rp.n).str(" shapes, removed ").i(done));
    Buf b; b.str("Removed ").i(done).str(" shapes");
    if (us.recorded < rp.n) b.str("  |  undo covers ").i(us.recorded);
    flash(b.z());
    return any;
}

static uint8_t place_hook(CostList* list, void* ctx, void* state, void* pdata, CursorXf* xf) {
    const bool replace = g_replace != 0;
    const LONG mode = g_mode;
    const bool modActive = replace || mode == MODE_AREA;
    if (!modActive) return g_placeOrig(list, ctx, state, pdata, xf);

    const int world = world_of(list, xf);
    if (world == WORLD_UNKNOWN) {
        LOG(.str("click in unrecognised world; running vanilla"));
        return g_placeOrig(list, ctx, state, pdata, xf);
    }
    const uint32_t shape = state ? *(uint32_t*)state : 0;
    uint8_t* cc = cursor_component(xf);

    if (world == WORLD_CLIENT) {
        // Clients only lend their live aim. Their prediction is skipped: the
        // server's writes replicate to the client as building actions.
        if (cc && !zero_pos(*(CursorXf*)(cc + 0x338))) {
            EnterCriticalSection(&g_stateLock);
            aim_record(xf, shape, *(CursorXf*)(cc + 0x338));
            g_clientCC = cc;
            LeaveCriticalSection(&g_stateLock);
        }
        return 0;
    }

    // ---- server world
    EnterCriticalSection(&g_stateLock);
    if (InterlockedExchange(&g_cancelReq, 0)) reset_corners();
    if (g_refundList && g_refundList != list) { g_refundN = 0; g_refundList = nullptr; }   // stale
    g_pend.n = 0;

    // Place cursor: where the ghost sits (next to the surface for small blocks).
    // Aimed cursor: the shape overlapping the block you look at. Replace uses it.
    CursorXf aim; const char* how = "";
    bool haveAim = aim_lookup(xf, shape, &aim, &how);
    if (g_cfg.log) {
        LOG(.str("click: mode=").str(mode == MODE_AREA ? "area" : "prefab")
            .str(" replace=").i(replace).str(" shape=").x(shape).str(" corners=").i(g_sc)
            .str(" aim=").str(haveAim ? how : "none"));
        log_xf("  place ", xf);
        if (haveAim) log_xf("  aim   ", &aim);
    }
    CursorXf target = *xf;
    if (replace) {
        if (!haveAim) {
            LOG(.str("  no live aim for this click; nothing done"));
            flash("Replace: could not read the aimed block, try again"); beep(MB_ICONHAND);
            LeaveCriticalSection(&g_stateLock);
            return 0;
        }
        target = aim;
    }

    uint8_t r = 0;
    if (mode == MODE_PREFAB) {
        pend_begin(ctx, target, true, shape);
        refund_reset();
        r = price_shape(list, ctx, state, pdata, &target, true) ? 1 : 0;
        { int na = 0; for (int m = 1; m < 0x100; m++) na += g_allow[m];
          LOG(.str("  pricing blit: voxels ").u(g_caveHits[0]).str(" solid ").u(g_caveHits[1])
              .str(" removable volume ").f(g_removedVolume).str(" materials allowed ").i(na)); }
        if (r) refund_build(list, ctx);
        if (!r) { flash("Nothing to replace there"); g_pend.n = 0; }
        LOG(.str("  replace priced=").i(r).str(" refund entries=").i(g_refundN));
        LeaveCriticalSection(&g_stateLock);
        return r;
    }

    // ---- area mode (build / replace)
    if (g_scOp != OP_BUILD) { reset_corners(); g_scOp = OP_BUILD; }
    LONG n = g_sc;
    if (n < 0 || n > g_cfg.corners - 1) n = 0;
    {
        Aabb box; shape_bounds(state, &target, &box);
        g_scPos[n] = box_centre(box);
        if (n == 0) {
            for (int a = 0; a < 3; a++) g_scStep[a] = box.mx[a] - box.mn[a];
            g_scCursorOff = replace ? 0x338 : 0x300;
            g_scOff[0] = g_scPos[0].x - target.pos[0]; g_scOff[1] = g_scPos[0].y - target.pos[1]; g_scOff[2] = g_scPos[0].z - target.pos[2];
        }
        LOG(.str("  corner centre (").fix(g_scPos[n].x).c(',').fix(g_scPos[n].y).c(',').fix(g_scPos[n].z).c(')'));
    }
    if (n < g_cfg.corners - 1) {
        g_sc = n + 1; g_corners = g_sc;
        Buf b; b.str(replace ? "Replace" : "Build").str(" corner ").i(n + 1).str(" of ").i(g_cfg.corners).str(" set");
        flash(b.z()); beep(MB_OK);
        LOG(.str("  corner ").i(n + 1).str(" set"));
        LeaveCriticalSection(&g_stateLock);
        return 0;
    }
    g_sc = 0; g_corners = 0;
    I3 corners[3] = { g_scPos[0], g_scPos[1], g_scPos[2] };
    r = structure_fill(list, ctx, state, pdata, target, corners, replace);
    LeaveCriticalSection(&g_stateLock);
    beep(MB_ICONASTERISK);
    return r;
}

// ------------------------------------------------------------------- overlay
static HWND g_overlay;
static char g_overlayText[320];
static volatile LONG g_fontPx = 24;      // panel font height, scaled to the window

static HWND game_window() {
    struct S { DWORD pid; HWND best; LONG area; } s = { GetCurrentProcessId(), nullptr, 0 };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* s = (S*)lp; DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid != s->pid || h == g_overlay || !IsWindowVisible(h)) return TRUE;
        RECT r; GetWindowRect(h, &r); LONG a = (r.right - r.left) * (r.bottom - r.top);
        if (a > s->area) { s->area = a; s->best = h; }
        return TRUE;
    }, (LPARAM)&s);
    return s.best;
}

static bool game_focused() {
    HWND f = GetForegroundWindow(); DWORD pid = 0;
    if (f) GetWindowThreadProcessId(f, &pid);
    return pid == GetCurrentProcessId();
}

static LRESULT CALLBACK overlay_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        HBRUSH bg = CreateSolidBrush(RGB(14, 16, 20));
        FillRect(dc, &r, bg); DeleteObject(bg);
        RECT bar = { r.left, r.bottom - 3, r.right, r.bottom };     // accent along the bottom
        HBRUSH fr = CreateSolidBrush(g_replace ? RGB(214, 150, 72) : RGB(196, 178, 126));
        FillRect(dc, &bar, fr); DeleteObject(fr);
        int px = g_fontPx;
        HFONT font = CreateFontW(px, 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(232, 226, 210));
        RECT tr = { r.left + px / 2, r.top + px / 4, r.right - px / 2, r.bottom - px / 4 };
        DrawTextA(dc, g_overlayText, -1, &tr, DT_CENTER | DT_TOP | DT_NOPREFIX);
        SelectObject(dc, of); DeleteObject(font);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// Size the area would have if the next corner were clicked where the cursor is now.
// The area the next corner click would make, from the corners set so far and the
// live client cursor. false when no area is being set up.
static bool area_plan_live(core::Plan* pl, float step[3], bool* ok) {
    LONG nc = g_corners;
    uint8_t* cc = g_clientCC;
    if (g_mode != MODE_AREA || nc < 1 || nc > g_cfg.corners - 1 || !cc) return false;
    LONG off = g_scCursorOff;
    if (!readable(cc + off, 0x18)) return false;
    int64_t* p = (int64_t*)(cc + off);
    if (!p[0] && !p[1] && !p[2]) return false;
    I3 live{ p[0] + g_scOff[0], p[1] + g_scOff[1], p[2] + g_scOff[2] };
    I3 c[3];
    const int64_t tol = (int64_t)(0.1 * core::kFix);
    if (nc == 1) { c[0] = g_scPos[0]; c[1] = live; c[2] = live; }   // 2-corner mode: the final size
    else { c[0] = g_scPos[0]; c[1] = g_scPos[1]; c[2] = core::lock_third(g_scPos[0], g_scPos[1], live, tol); }
    for (int a = 0; a < 3; a++) step[a] = g_scStep[a];
    if (step[0] <= 0.f || step[1] <= 0.f || step[2] <= 0.f) return false;
    *pl = core::Plan{};
    *ok = core::plan_fill(c, 3, step, 50000000, pl);
    return true;
}

static void live_size(Buf& b) {
    core::Plan pl; float step[3]; bool ok = false;
    if (!area_plan_live(&pl, step, &ok)) return;
    long n = ok ? core::fill_count(pl, g_fill) : 0;
    if (!ok || n > g_cfg.maxShapes) { b.str("  |  TOO BIG (max ").i(g_cfg.maxShapes).c(')'); return; }
    b.str("  |  ").i(pl.count[0]).str("x").i(pl.count[2]).str(" h").i(pl.count[1]).str(" = ").i(n);
}

// ---- in-game area preview
// The client cursor's hover function (28e2f0) runs every frame. When the hammer
// hovers an existing block it highlights that block's voxels with one looped VFX
// each (guid b0718612-..., started by 29d3d0, stopped by 29d420, time from 9b9490).
// While an area is being set up, the mod runs its own set of those highlights along
// the area's 12 edges, from the same function and thread. The highlight is drawn on
// surfaces (block faces), so edges in open air are not visible; tested alternatives
// (the hammer's cursor VFX) were worse. Voxel coordinates: floor(world metres * 2); VFX at the voxel centre.
typedef void (*HoverFxFn)(uint8_t* ctx, void* b, void* c);
typedef uint64_t* (*GameTimeFn)(void* world, void* out);
typedef uint32_t (*VfxStartFn)(void* sys, uint64_t time, const void* guid, const float* pos, const float* rot,
                               const void* global, void* params);
typedef void (*VfxStopFn)(void* sys, uint32_t* handle, uint64_t time, const void* extra);
static HoverFxFn g_hoverFxOrig = nullptr;
static GameTimeFn g_gameTime = nullptr;
static VfxStartFn g_vfxStart = nullptr;
static VfxStopFn g_vfxStop = nullptr;
static const void* g_vfxGlobal = nullptr;
static const uint32_t kHoverVfxGuid[4] = { 0xb0718612, 0x4e1327b1, 0xca143cbf, 0x9a608f41 };
static const int kPreviewMax = 256;
struct PreviewSlot { uint32_t v[3]; uint32_t handle; };
static PreviewSlot g_pv[kPreviewMax];
static volatile LONG g_previewCount = 0;
static volatile LONG g_pvStarted = 0, g_pvActiveFrames = 0;

// voxels along the 12 edges of the area box (thinned evenly when they do not fit)
static int preview_voxels(uint32_t out[][3], int cap) {
    core::Plan pl; float step[3]; bool ok = false;
    if (!g_cfg.preview || !area_plan_live(&pl, step, &ok) || !ok) return 0;
    if (core::fill_count(pl, g_fill) > g_cfg.maxShapes) return 0;
    int64_t v0[3], v1[3];
    for (int a = 0; a < 3; a++) {
        int64_t e = pl.origin[a] + pl.stepFix[a] * (pl.count[a] - 1);
        int64_t lo = e < pl.origin[a] ? e : pl.origin[a], hi = e < pl.origin[a] ? pl.origin[a] : e;
        int64_t half = (int64_t)(step[a] * 0.5 * core::kFix);
        lo -= half; hi += half;
        v0[a] = lo >> 31;                                   // floor(m * 2)
        v1[a] = (hi - 1) >> 31;                             // last voxel inside
        if (v1[a] < v0[a]) v1[a] = v0[a];
        if (v0[a] < 0) return 0;
    }
    int64_t d[3] = { v1[0] - v0[0] + 1, v1[1] - v0[1] + 1, v1[2] - v0[2] + 1 };
    int64_t inner = (d[0] > 2 ? d[0] - 2 : 0) * (d[1] > 2 ? d[1] - 2 : 0) * (d[2] > 2 ? d[2] - 2 : 0);
    int64_t surface = d[0] * d[1] * d[2] - inner;
    int n = 0;
    auto put = [&](int64_t x, int64_t y, int64_t z) {
        if (n >= cap) return;
        out[n][0] = (uint32_t)x; out[n][1] = (uint32_t)y; out[n][2] = (uint32_t)z; n++;
    };
    (void)surface;
    {
        int64_t edges = 4 * (d[0] + d[1] + d[2]);
        int64_t stride = (edges + cap - 9) / (cap - 8);
        if (stride < 1) stride = 1;
        for (int a = 0; a < 3; a++) {
            int b1 = (a + 1) % 3, b2 = (a + 2) % 3;
            for (int k = 0; k < 4; k++) {
                int64_t q1 = (k & 1) ? v1[b1] : v0[b1], q2 = (k & 2) ? v1[b2] : v0[b2];
                for (int64_t t = v0[a]; t <= v1[a]; t += stride) {
                    int64_t pp[3]; pp[a] = t; pp[b1] = q1; pp[b2] = q2; put(pp[0], pp[1], pp[2]);
                }
                int64_t pp[3]; pp[a] = v1[a]; pp[b1] = q1; pp[b2] = q2; put(pp[0], pp[1], pp[2]);   // corner
            }
        }
    }
    return n;
}

static void hover_fx_hook(uint8_t* ctx, void* b, void* c) {
    g_hoverFxOrig(ctx, b, c);
    if (!ctx || !g_vfxStart || !g_vfxStop || !g_gameTime) return;
    void* world = *(void**)ctx;
    void* sys = *(void**)(ctx + 0x168);
    if (!world || !sys) return;
    static uint32_t want[kPreviewMax][3];
    int n = preview_voxels(want, kPreviewMax);
    static int have = 0;
    if (n == 0 && have == 0) return;
    uint64_t tbuf[4] = {};
    uint64_t* tp = g_gameTime(world, tbuf);
    uint64_t now = tp ? *tp : 0;
    alignas(16) uint8_t extra[16] = {};
    alignas(16) uint8_t params[0x80] = {};
    alignas(16) static const float rot[4] = { 0.f, 0.f, 0.f, 1.f };
    int top = n > have ? n : have;
    for (int i = 0; i < top; i++) {
        PreviewSlot& s = g_pv[i];
        if (i < n && s.handle && !memcmp(s.v, want[i], 12)) continue;          // unchanged
        if (s.handle) g_vfxStop(sys, &s.handle, now, extra);
        s.handle = 0;
        if (i < n) {
            memcpy(s.v, want[i], 12);
            alignas(16) float pos[4] = { want[i][0] * 0.5f + 0.25f, want[i][1] * 0.5f + 0.25f, want[i][2] * 0.5f + 0.25f, 0.f };
            memset(params, 0, sizeof(params));
            s.handle = g_vfxStart(sys, now, kHoverVfxGuid, pos, rot, g_vfxGlobal, params);
            InterlockedIncrement(&g_pvStarted);
        }
    }
    have = n;
    g_previewCount = n;
    if (n) InterlockedIncrement(&g_pvActiveFrames);
}

static void preview_diag() {
    static DWORD next = 0; static LONG last = -1;
    if (!g_cfg.log || GetTickCount() < next) return;
    next = GetTickCount() + 3000;
    LONG f = g_pvActiveFrames;
    if (f == last) return;
    last = f;
    LOG(.str("preview: ").i(g_previewCount).str(" highlights, started ").i(g_pvStarted).str(", active frames ").i(f));
}

static void vk_name(int vk, char* out) {
    if (vk >= VK_F1 && vk <= VK_F24) { Buf b; b.c('F').i(vk - VK_F1 + 1); lstrcpynA(out, b.z(), 16); return; }
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (!sc || !GetKeyNameTextA((LONG)(sc << 16), out, 16)) { Buf b; b.str("key ").i(vk); lstrcpynA(out, b.z(), 16); }
}

// Info panel text, one item per line (shown bottom centre).
static void compose_overlay(char* out, int cap) {
    Buf b;
    bool vanilla = g_mode == MODE_PREFAB && !g_replace && !g_hover;
    bool flashing = GetTickCount() < g_flashUntil;
    if (vanilla) { if (flashing) b.str(g_flash); lstrcpynA(out, b.z(), cap); return; }
    // line 1: state (or the latest message)
    if (flashing) b.str(g_flash);
    else {
        if (g_mode == MODE_AREA) {
            static const char* fills[3] = { "", " hollow", " walls" };
            b.str("Area").str(fills[g_fill % 3]).c(' ').str(g_scOp == OP_REMOVE ? "remove" : (g_replace ? "replace" : "build"))
             .str("  ").i(g_corners).c('/').i(g_cfg.corners);
            live_size(b);
        } else b.str(g_replace ? "Prefab replace" : "Prefab");
        if (g_replace) b.str(g_replaceStyle == STYLE_FULL ? "  |  full shape" : "  |  overlap");
        if (g_hover) { int t = (int)(g_hoverDist * 10.f + 0.5f); b.str("  |  free ").i(t / 10).c('.').i(t % 10).str(" m"); }
    }
    // line 2: keys
    if (g_cfg.panelKeys) {
        char k[6][16];
        vk_name(g_cfg.keyStructure, k[0]); vk_name(g_cfg.keyReplace, k[1]); vk_name(g_cfg.keyCancel, k[2]);
        vk_name(g_cfg.keyStyle, k[3]); vk_name(g_cfg.keyHover, k[4]); vk_name(g_cfg.keyFill, k[5]);
        b.str("\n").str(k[0]).str(" area  ").str(k[1]).str(" replace  ").str(k[2]).str(" clear  ")
         .str(k[3]).str(" style  ").str(k[4]).str(" free  ").str(k[5]).str(" fill");
        if (g_hover) b.str("  Q+wheel dist");
    }
    lstrcpynA(out, b.z(), cap);
}

static DWORD WINAPI overlay_thread(void*) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = overlay_proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EnshroudedBuildingToolsOverlay"; wc.hCursor = nullptr;
    RegisterClassExW(&wc);
    g_overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                wc.lpszClassName, L"", WS_POPUP, 0, 0, 420, 36, nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(g_overlay, 0, 205, LWA_ALPHA);

    bool shown = false;
    char last[320] = "", changed[320] = "";
    DWORD changedAt = GetTickCount();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        compose_overlay(g_overlayText, sizeof(g_overlayText));
        preview_diag();
        DWORD now = GetTickCount();
        if (lstrcmpA(changed, g_overlayText) != 0) { lstrcpyA(changed, g_overlayText); changedAt = now; }
        bool idle = g_cfg.panelTimeout > 0 && now - changedAt > (DWORD)g_cfg.panelTimeout * 1000 && now >= g_flashUntil;
        bool want = g_cfg.overlay && g_overlayText[0] && game_focused() && !idle;
        if (want) {
            HWND gw = game_window();
            RECT r = { 0, 0, 1920, 1080 };
            if (gw) {
                RECT c; POINT tl = { 0, 0 };
                if (GetClientRect(gw, &c) && ClientToScreen(gw, &tl))
                    r = { tl.x, tl.y, tl.x + c.right, tl.y + c.bottom };
                else GetWindowRect(gw, &r);
            }
            HDC dc = GetDC(g_overlay);
            float sc = (float)(r.bottom - r.top) / 1080.f * (float)g_cfg.panelScale / 100.f;
            if (sc < 0.5f) sc = 0.5f;
            int px = (int)(17.f * sc + 0.5f);
            if (px != g_fontPx) { g_fontPx = px; last[0] = 0; }
            HFONT font = CreateFontW(px, 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HGDIOBJ of = SelectObject(dc, font);
            RECT tr = { 0, 0, 4000, 0 };
            DrawTextA(dc, g_overlayText, -1, &tr, DT_CALCRECT | DT_NOPREFIX);
            SelectObject(dc, of); DeleteObject(font); ReleaseDC(g_overlay, dc);
            int w = (tr.right - tr.left) + px * 2, h = (tr.bottom - tr.top) + px / 2 + 3;
            int x = r.left + ((r.right - r.left) - w) / 2, y = r.bottom - (int)(g_cfg.panelBottom * sc) - h;
            if (x < r.left) x = r.left; if (y < r.top) y = r.top;
            SetWindowPos(g_overlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (!shown || lstrcmpA(last, g_overlayText) != 0) { InvalidateRect(g_overlay, nullptr, FALSE); lstrcpyA(last, g_overlayText); }
            shown = true;
        } else if (shown) {
            ShowWindow(g_overlay, SW_HIDE); shown = false;
        }
        Sleep(50);
    }
}

// ------------------------------------------------- free placement distance (Q + wheel)
static void hover_step(float d) {
    float v = g_hoverDist + d;
    if (v < 1.f) v = 1.f; if (v > 20.f) v = 20.f;
    g_hoverDist = v;
    int t = (int)(v * 10.f + 0.5f);
    Buf b; b.str("Free placement distance ").i(t / 10).c('.').i(t % 10).str(" m"); flash(b.z());
}
static LRESULT CALLBACK wheel_proc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && w == WM_MOUSEWHEEL && g_active && g_hover && game_focused() &&
        g_cfg.wheelKey > 0 && (GetAsyncKeyState(g_cfg.wheelKey) & 0x8000)) {
        short delta = (short)HIWORD(((MSLLHOOKSTRUCT*)l)->mouseData);
        hover_step(delta > 0 ? 0.5f : -0.5f);
        return 1;                                    // the game does not see this wheel turn
    }
    return CallNextHookEx(nullptr, code, w, l);
}
static DWORD WINAPI wheel_thread(void*) {
    HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, wheel_proc, GetModuleHandleW(nullptr), 0);
    LOG(.str("mouse wheel hook: ").str(h ? "ok" : "failed"));
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

// ------------------------------------------------------------------ hotkeys
static DWORD WINAPI input_thread(void*) {
    bool prev[8] = {};
    for (;;) {
        Sleep(15);
        if (!g_active) continue;
        bool focus = game_focused();
        int keys[8] = { g_cfg.keyStructure, g_cfg.keyReplace, g_cfg.keyCancel, g_cfg.keyStyle, g_cfg.keyHover,
                        g_hover ? g_cfg.keyFarther : 0, g_hover ? g_cfg.keyCloser : 0, g_cfg.keyFill };
        for (int k = 0; k < 8; k++) {
            bool down = focus && keys[k] > 0 && (GetAsyncKeyState(keys[k]) & 0x8000);
            if (down && !prev[k]) {
                if (k == 0) {
                    LONG m = g_mode == MODE_PREFAB ? MODE_AREA : MODE_PREFAB;
                    g_mode = m; g_corners = 0; g_cancelReq = 1;
                    flash(m == MODE_AREA ? "Area mode" : "Prefab mode (vanilla)");
                    LOG(.str("key: mode -> ").str(m == MODE_AREA ? "area" : "prefab"));
                } else if (k == 1) {
                    LONG r = !g_replace; g_replace = r;
                    flash(r ? "Replace ON" : "Replace OFF");
                    LOG(.str("key: replace -> ").i(r));
                } else if (k == 3) {
                    LONG st = g_replaceStyle == STYLE_FULL ? STYLE_OVERLAP : STYLE_FULL; g_replaceStyle = st;
                    flash(st == STYLE_FULL ? "Replace style: full shape" : "Replace style: overlap only");
                    LOG(.str("key: replace style -> ").str(st == STYLE_FULL ? "full" : "overlap"));
                } else if (k == 7) {
                    LONG f = (g_fill + 1) % core::FILL_COUNT; g_fill = f;
                    flash(f == core::FILL_SOLID ? "Area fill: solid" : f == core::FILL_HOLLOW ? "Area fill: hollow room (walls, floor, ceiling)" : "Area fill: walls only");
                    LOG(.str("key: fill -> ").i(f));
                } else if (k == 5 || k == 6) {
                    hover_step(k == 5 ? 0.5f : -0.5f);
                } else if (k == 4) {
                    LONG h = !g_hover; g_hover = h; g_cxLogged = 0;
                    flash(h ? "Free placement ON (no surface snapping)" : "Free placement OFF");
                    LOG(.str("key: free placement -> ").i(h));
                } else {
                    g_cancelReq = 1; g_corners = 0;
                    flash("Corners cleared");
                    LOG(.str("key: corners cleared"));
                }
                if (k < 5 || k >= 7) beep(MB_ICONQUESTION);
            }
            prev[k] = down;
        }
    }
}

// Copies src/cave.S next to the game image and points the blit's density check at it.
static bool install_cave(uint8_t* sig) {
    size_t size = (size_t)(bt_cave_end - bt_cave_start);
    uint8_t* site = sig + kBlitPatchOff;
    uint8_t* mem = alloc_near(site, size + 64);
    if (!mem) return false;
    memcpy(mem, bt_cave_start, size);
    uint64_t cont = (uint64_t)(site + kBlitPatchLen), skip = (uint64_t)(sig + kBlitSkipOff);
    memcpy(mem + (bt_cave_cont - bt_cave_start), &cont, 8);
    memcpy(mem + (bt_cave_skip - bt_cave_start), &skip, 8);
    uint64_t write = (uint64_t)(sig + kBlitWriteOff);
    memcpy(mem + (bt_cave_write - bt_cave_start), &write, 8);
    g_caveAllow = mem + (bt_cave_allow - bt_cave_start);
    g_caveMode = mem + (bt_cave_mode - bt_cave_start);
    g_caveVols = (uint32_t*)(mem + (bt_cave_vols - bt_cave_start));
    g_caveHits = (uint32_t*)(mem + (bt_cave_hits - bt_cave_start));
    *g_caveMode = 0;
    FlushInstructionCache(GetCurrentProcess(), mem, size);
    DWORD old;
    if (!VirtualProtect(site, kBlitPatchLen, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t patch[16];
    int32_t rel = (int32_t)(mem - (site + 5));
    patch[0] = 0xE9; memcpy(patch + 1, &rel, 4);
    for (int k = 5; k < kBlitPatchLen; k++) patch[k] = 0x90;
    memcpy(site, patch, kBlitPatchLen);
    VirtualProtect(site, kBlitPatchLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, kBlitPatchLen);
    { Buf b; b.str("replace blit installed (").u(size).str(" bytes) at site game+").x((uint64_t)(site - g_base)).str(":");
      for (int k = 0; k < kBlitPatchLen; k++) b.c(' ').u(site[k], 16, 2);
      log_line(b); }
    return true;
}

// ------------------------------------------------------------- crash report
// Logs where a fatal exception happened (as module offsets), then lets the game
// handle it as usual. First-chance exceptions the game handles itself are rare;
// only the first few are logged.
static uint8_t* g_self; static size_t g_selfSize;
static volatile LONG g_crashLogs = 0;

static void addr_desc(Buf& b, uint64_t a) {
    uint64_t gb = (uint64_t)g_base, ge = gb + 0x2a1e000;
    uint64_t sb = (uint64_t)g_self, se = sb + g_selfSize;
    if (a >= gb && a < ge) b.str("game+").x(a - gb);
    else if (a >= sb && a < se) b.str("mod+").x(a - sb);
    else b.x(a);
}

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code < 0xC0000000 || code == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;   // not fatal / C++ throw
    if (InterlockedIncrement(&g_crashLogs) > 4) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    { Buf b; b.str("EXCEPTION ").x(code).str(" at "); addr_desc(b, c->Rip);
      if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
          b.str(ep->ExceptionRecord->ExceptionInformation[0] ? " writing " : " reading ").x(ep->ExceptionRecord->ExceptionInformation[1]);
      b.str(" thread ").u(GetCurrentThreadId());
      log_line(b); }
    { Buf b; b.str("  rax=").x(c->Rax).str(" rcx=").x(c->Rcx).str(" rdx=").x(c->Rdx).str(" r8=").x(c->R8)
          .str(" r9=").x(c->R9).str(" rbx=").x(c->Rbx).str(" rsi=").x(c->Rsi).str(" rdi=").x(c->Rdi); log_line(b); }
    Buf b; b.str("  stack:");
    uint64_t* sp = (uint64_t*)c->Rsp; int n = 0;
    for (int k = 0; k < 512 && n < 14; k++) {
        if (!readable(sp + k, 8)) break;
        uint64_t v = sp[k];
        uint64_t gb = (uint64_t)g_base;
        bool inGame = v >= gb + 0x1000 && v < gb + 0x13b2000;
        bool inMod = v >= (uint64_t)g_self && v < (uint64_t)g_self + g_selfSize;
        if (inGame || inMod) { b.c(' '); addr_desc(b, v); n++; }
    }
    log_line(b);
    return EXCEPTION_CONTINUE_SEARCH;
}

// --------------------------------------------------------------------- init
static DWORD WINAPI init_thread(void*) {
    Sleep(4000);   // let the Steam stub and game start-up finish
    find_text();
    g_hoverDist = g_cfg.hoverDist10 / 10.f;
    if (g_hoverDist < 1.f) g_hoverDist = 1.f; if (g_hoverDist > 20.f) g_hoverDist = 20.f;
    LOG(.str(MOD_NAME " " MOD_VERSION " starting, image base ").x((uint64_t)g_base)
        .str(" .text size ").x(g_textSize));
    uint8_t* place  = find_hookable("PlaceShape", SIG_PLACE, 10);
    uint8_t* remove = find_hookable("RemoveShape", SIG_REMOVE, 10);
    uint8_t* bounds = find_unique("ShapeBounds", SIG_BOUNDS);
    uint8_t* cc     = find_unique("CursorCall", SIG_CURSOR_CALL);
    uint8_t* pc     = find_unique("PlaceCall", SIG_PLACE_CALL);
    uint8_t* sc     = find_unique("ServerCheck", SIG_SERVER_CHECK);
    uint8_t* pay    = find_hookable("PayCost", SIG_PAY, 7);
    uint8_t* pcommit = find_hookable("PlaceCommit", SIG_PLACE_COMMIT, 5);
    uint8_t* rcommit = find_hookable("RemoveCommit", SIG_REMOVE_COMMIT, 5);
    uint8_t* matItem = find_unique("MaterialItem", SIG_MATERIAL_ITEM);
    uint8_t* tearDown = find_unique("TearDownEvent", SIG_TEARDOWN_EVENT);
    uint8_t* refundBuild = find_hookable("RefundBuild", SIG_REFUND_BUILD, 5);
    uint8_t* blit = find_unique("BlitCheck", SIG_BLIT_CHECK);
    uint8_t* wtable = find_unique("WriteTable", SIG_WRITE_TABLE);
    uint8_t* popUndo = find_hookable("PopUndo", SIG_POP_UNDO, 5);
    uint8_t* hudUpd = find_hookable("HudUpdate", SIG_HUD_UPDATE, 5);
    uint8_t* uiSnd = find_unique("UiSound", SIG_UI_SOUND);
    uint8_t* cquery = find_unique("CursorQuery", SIG_CURSOR_QUERY);
    g_removeCommit = (CommitFn)rcommit;
    g_materialItem = (MaterialItemFn)matItem;
    g_remove = nullptr;
    g_bounds = (BoundsFn)bounds;
    // both call-site patterns must live in the same function, close together
    g_cursorLayoutOk = cc && pc && pc > cc && pc - cc < 0x400;
    // the server/client branch must sit directly before the placement call
    g_worldLayoutOk = g_cursorLayoutOk && sc && sc + 81 == pc;
    if (!place || !bounds || !remove || !pay || !pcommit || !rcommit || !matItem || !refundBuild || !blit || !wtable || !g_worldLayoutOk) {
        LOG(.str("required code not found; mod disabled (game version not supported, or another mod changed this code). place=").i(place != nullptr)
            .str(" bounds=").i(bounds != nullptr).str(" remove=").i(remove != nullptr).str(" pay=").i(pay != nullptr)
            .str(" layout=").i(g_worldLayoutOk));
        flash("Building Tools: game version not supported");
        return 0;
    }
    g_payOrig = (PayFn)hook(pay, 7, (void*)&pay_hook);
    if (!g_payOrig) { LOG(.str("pay hook failed; mod disabled")); return 0; }
    (void)tearDown;
    g_remove = (RemoveFn)hook(remove, 10, (void*)&remove_hook);
    if (!g_remove) { LOG(.str("remove hook failed; mod disabled")); return 0; }
    g_removeCommitOrig = (CommitFn)hook(rcommit, 5, (void*)&remove_commit_hook);
    if (!g_removeCommitOrig) { LOG(.str("remove commit hook failed; mod disabled")); return 0; }
    g_refundBuildOrig = (RefundBuildFn)hook(refundBuild, 5, (void*)&refund_build_hook);
    if (!g_refundBuildOrig) { LOG(.str("refund hook failed; mod disabled")); return 0; }
    (void)wtable;     // the replace blit applies the allow list itself
    if (!install_cave(blit)) { LOG(.str("blit patch failed; mod disabled")); return 0; }
    if (popUndo) g_popUndoOrig = (PopUndoFn)hook(popUndo, 5, (void*)&pop_undo_hook);
    if (!g_popUndoOrig) LOG(.str("undo pop hook not installed: area actions keep undo for their first shape only"));
    if (hudUpd && uiSnd) {
        g_uiCtxSlot = (uint8_t**)(uiSnd + 7 + *(int32_t*)(uiSnd + 3));
        g_playUiSound = (PlayUiSoundFn)(uiSnd + 0x2d + *(int32_t*)(uiSnd + 0x29));
        g_hudOrig = (HudUpdateFn)hook(hudUpd, 5, (void*)&hud_hook);
        if (!g_hudOrig) g_playUiSound = nullptr;
    }
    LOG(.str("game sounds: ").str(g_playUiSound ? "yes" : "no (Windows sounds)"));
    (void)cquery;
    uint8_t* sdeps = find_hookable("SystemDeps", SIG_SYS_DEPS, 5);
    uint8_t* ddeps = find_unique("DismantleDeps", SIG_DISM_DEPS);
    if (sdeps && ddeps && ddeps + 0x15 + *(int32_t*)(ddeps + 0x11) == sdeps) {
        g_dismDepsRet = ddeps + 0x15;
        g_depsOrig = (DepsFn)hook(sdeps, 5, (void*)&deps_hook);
    }
    LOG(.str("refund pop-up: ").str(g_depsOrig ? "available" : "not available"));
    uint8_t* hfx = find_hookable("HoverFx", SIG_HOVER_FX, 5);
    if (hfx) {
        // the hover function's own calls: game time (+0x4fd), stop (+0x51d), start (+0x607),
        // lea of the spawn defaults (+0x5e7) and the highlight VFX guid (+0x432)
        auto rel = [](uint8_t* at) -> uint8_t* { return at + 5 + *(int32_t*)(at + 1); };
        bool layout = hfx[0x4fd] == 0xE8 && hfx[0x51d] == 0xE8 && hfx[0x607] == 0xE8 &&
                      hfx[0x5e7] == 0x48 && hfx[0x5e8] == 0x8D && hfx[0x5e9] == 0x0D &&
                      *(uint32_t*)(hfx + 0x432 + 4) == kHoverVfxGuid[0];
        if (layout) {
            g_gameTime = (GameTimeFn)rel(hfx + 0x4fd);
            g_vfxStop = (VfxStopFn)rel(hfx + 0x51d);
            g_vfxStart = (VfxStartFn)rel(hfx + 0x607);
            g_vfxGlobal = hfx + 0x5e7 + 7 + *(int32_t*)(hfx + 0x5e7 + 3);
            g_hoverFxOrig = (HoverFxFn)hook(hfx, 5, (void*)&hover_fx_hook);
        }
    }
    LOG(.str("area preview: ").str(g_hoverFxOrig ? "available" : "not available"));
    uint8_t* cxf = find_hookable("CursorTransform", SIG_CURSOR_XF, 5);
    if (cxf) g_cxOrig = (CursorXfFn)hook(cxf, 5, (void*)&cursor_xf_hook);
    if (!g_cxOrig) LOG(.str("free placement not available"));
    g_placeCommitOrig = (CommitFn)hook(pcommit, 5, (void*)&place_commit_hook);
    if (!g_placeCommitOrig) { LOG(.str("commit hook failed; mod disabled")); return 0; }
    g_placeOrig = (PlaceFn)hook(place, 10, (void*)&place_hook);
    if (!g_placeOrig) { LOG(.str("hook failed; mod disabled")); return 0; }
    g_active = 1;
    LOG(.str("ready. keys: area=").i(g_cfg.keyStructure).str(" replace=").i(g_cfg.keyReplace).str(" style=").i(g_cfg.keyStyle)
        .str(" cancel=").i(g_cfg.keyCancel).str(" maxShapes=").i(g_cfg.maxShapes));
    flash("Building Tools " MOD_VERSION " ready");
    return 0;
}

// ------------------------------------------------------------- dinput8 proxy
typedef HRESULT (WINAPI *DI8CreateFn)(HINSTANCE, DWORD, const void*, void**, void*);
static DI8CreateFn g_realCreate;

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, const void* riid, void** out, void* outer) {
    if (!g_realCreate) {
        wchar_t p[MAX_PATH];
        UINT n = GetSystemDirectoryW(p, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16) return E_FAIL;
        lstrcatW(p, L"\\dinput8.dll");
        HMODULE m = LoadLibraryW(p);
        if (m) g_realCreate = (DI8CreateFn)GetProcAddress(m, "DirectInput8Create");
        if (!g_realCreate) return E_FAIL;
    }
    return g_realCreate(inst, ver, riid, out, outer);
}

extern "C" BOOL WINAPI DllMainCRTStartup(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    g_base = (uint8_t*)GetModuleHandleW(nullptr);
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    for (int i = lstrlenW(g_dir); i > 0; i--) if (g_dir[i - 1] == L'\\') { g_dir[i] = 0; break; }
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_stateLock);
    load_config();
    g_logOn = g_cfg.log != 0;
    open_log();
    g_self = (uint8_t*)self;
    { auto* dos = (IMAGE_DOS_HEADER*)self; auto* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)self + dos->e_lfanew);
      g_selfSize = nt->OptionalHeader.SizeOfImage; }
    AddVectoredExceptionHandler(0, crash_handler);
    CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, input_thread, nullptr, 0, nullptr);
    if (g_cfg.overlay) CreateThread(nullptr, 0, overlay_thread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, wheel_thread, nullptr, 0, nullptr);
    return TRUE;
}
