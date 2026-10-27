// Enshrouded Building Tools: dinput8.dll proxy mod (single player).
//
//   Prefab mode    - vanilla placement, one shape per click.
//   Area mode      - two (or three) corner clicks; the box they span is built,
//                    replaced or removed with the selected shape (solid, hollow, walls).
//   Replace        - repaints existing blocks with the current material: full shape,
//                    overlap only, or connected (fill bucket). Refunds the old material.
//                    Connected also deletes: right click removes the touching material.
//   Free placement - the ghost hovers in front of the camera on the 0.5 m grid.
//   Options        - creative building, magic chests of all bases, off switch (F5).
// How it works: docs/HOW-IT-WORKS.md. Addresses and research: docs/reverse-engineering-notes.md.
//
// Built freestanding with clang (mingw target), no C runtime.

#include <windows.h>
#include "core.h"

using core::CursorXf;
using core::IntXf;
using core::Aabb;
using core::I3;

#define MOD_NAME    "Building Tools"
#define MOD_VERSION "0.20.1"

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
    Buf& i(int64_t v) { if (v < 0) { c('-'); return u(0 - (uint64_t)v); } return u((uint64_t)v); }
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
    // A (translated) text with {0}..{3} replaced by the arguments.
    Buf& fmt(const char* t, const char* a0 = "", const char* a1 = "", const char* a2 = "", const char* a3 = "") {
        const char* a[4] = { a0, a1, a2, a3 };
        while (*t) {
            if (t[0] == '{' && t[1] >= '0' && t[1] <= '3' && t[2] == '}') { str(a[t[1] - '0']); t += 3; }
            else c(*t++);
        }
        return *this;
    }
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
    int keyMenu      = VK_HOME;    // in-game menu (native game UI, experimental)
    int refundPopup  = 1;          // show the game's item pop-up for refunds
    int preview      = 1;          // show the area with the game's ghost highlight
    int hoverDist10  = 40;         // free placement distance in tenths of a metre
    int panelScale   = 100;        // info panel size in percent
    int panelBottom  = 0;      // panel distance from the bottom edge (pixels at 1080p)
    int panelTimeout = 5;      // seconds without change before the panel hides (0 = never)
    int panelKeys    = 1;      // key hint line
    int hudRows      = 1;      // mod state and keys in the game's building HUD box
    int creative     = 0;      // building is free: no charge, no refund, no missing materials
    int keyOff       = VK_F5;  // the whole mod off / on (vanilla game while off)
    int fillLimit    = 500;    // most shapes one connected replace (fill bucket) may repaint
    int globalChests = 0;      // building and crafting use the magic chests of every base
    char language[16] = "auto";  // text language: auto (as the game), or a file code (pl, de, ...)
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
    IniText& i(int v) { char b[16]; int k = 0; bool neg = v < 0; unsigned u = neg ? 0u - (unsigned)v : (unsigned)v;
        do { b[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 15);
        if (neg) t("-"); while (k) { char c[2] = { b[--k], 0 }; t(c); } return *this; }
};
static void write_full_ini(const Config& c) {
    static IniText o; o.n = 0;
    o.t("; Enshrouded Building Tools " MOD_VERSION "\r\n")
     .t("; Close the game before editing. Keys are Windows virtual-key codes (decimal):\r\n")
     .t(";   F1..F12 = 112..123   A..Z = 65..90   0..9 = 48..57   Page Up 33  Page Down 34\r\n")
     .t(";   Insert 45  Delete 46  Home 36  End 35  Numpad 0..9 = 96..105  (0 = key off)\r\n")
     .t(";   Add 4096 for Shift, 8192 for Ctrl, 16384 for Alt (Shift+K = 4096+75 = 4171).\r\n")
     .t(";   Easier: change keys in the game with the Building Tools menu.\r\n")
     .t("[Keys]\r\n")
     .t("; Prefab mode <-> Area mode\r\nToggleAreaMode=").i(c.keyStructure).t("\r\n")
     .t("; Replace on/off\r\nToggleReplace=").i(c.keyReplace).t("\r\n")
     .t("; Clear area corners\r\nCancelCorners=").i(c.keyCancel).t("\r\n")
     .t("; Replace style: full shape / overlap only / connected (fill bucket)\r\nToggleReplaceStyle=").i(c.keyStyle).t("\r\n")
     .t("; Area fill: solid / hollow room / walls only\r\nCycleAreaFill=").i(c.keyFill).t("\r\n")
     .t("; Whole mod off / on (off = vanilla game; all other mod keys do nothing)\r\nToggleMod=").i(c.keyOff).t("\r\n")
     .t("; Free placement on/off\r\nToggleFreePlacement=").i(c.keyHover).t("\r\n")
     .t("; Free placement distance: hold this key + mouse wheel (81 = Q), or the two keys below\r\nFreePlacementWheelKey=").i(c.wheelKey).t("\r\n")
     .t("FreePlacementFarther=").i(c.keyFarther).t("\r\n")
     .t("FreePlacementCloser=").i(c.keyCloser).t("\r\n")
     .t("; Building Tools menu in the game's own UI (experimental)\r\nMenu=").i(c.keyMenu).t("\r\n")
     .t("\r\n[Creative]\r\n")
     .t("; 1 = creative building: blocks cost nothing, removing or undoing gives nothing back,\r\n")
     .t(";     so your inventory is never touched (props, crafting and chests stay normal)\r\n")
     .t("CreativeBuilding=").i(c.creative).t("\r\n")
     .t("\r\n[Chests]\r\n")
     .t("; 1 = building and crafting use the magic chests of all your bases (flame altar\r\n")
     .t(";     areas), not only the one you stand in, also outside a base. The material picker\r\n")
     .t(";     and crafting menus count them. Loot deposit stays local. Off by default.\r\n")
     .t("GlobalMagicChests=").i(c.globalChests).t("\r\n")
     .t("\r\n[Area]\r\n")
     .t("; 2 = two opposite corners, 3 = two base corners then the height\r\nCorners=").i(c.corners).t("\r\n")
     .t("; Free placement start distance in tenths of a metre (40 = 4 m)\r\nFreePlacementDistance=").i(c.hoverDist10).t("\r\n")
     .t("; Show the area being set up with the game's ghost highlight (1 = on)\r\nPreview=").i(c.preview).t("\r\n")
     .t("\r\n[Limits]\r\n")
     .t("; Most shapes one area action may build, replace or remove (max 20000)\r\nMaxShapes=").i(c.maxShapes).t("\r\n")
     .t("; Most shapes one connected replace (fill bucket) repaints (max = MaxShapes)\r\nFillLimit=").i(c.fillLimit).t("\r\n")
     .t("\r\n[Interface]\r\n")
     .t("; Info panel at the bottom of the screen (0 = off)\r\nOverlay=").i(c.overlay).t("\r\n")
     .t("; Panel size in percent, lift from the bottom edge (pixels at 1080p)\r\nPanelScale=").i(c.panelScale).t("\r\n")
     .t("PanelBottom=").i(c.panelBottom).t("\r\n")
     .t("; Seconds without change before the panel hides (0 = always shown), key hint line\r\nPanelTimeout=").i(c.panelTimeout).t("\r\n")
     .t("PanelKeys=").i(c.panelKeys).t("\r\n")
     .t("; Mod state and keys as rows of the game's building box, bottom right (1 = on)\r\nHudRows=").i(c.hudRows).t("\r\n")
     .t("; Sound: 1 = game sounds, 2 = Windows sounds, 0 = off\r\nSound=").i(c.sound).t("\r\n")
     .t("; Item pop-up when Replace gives materials back (1 = on)\r\nRefundPopup=").i(c.refundPopup).t("\r\n")
     .t("; Text language: auto = as the game, or en, de, fr, it, ja, ko, pl, pt-BR, ru, es, th, tr,\r\n")
     .t(";     uk, zh-CN, zh-TW (files in the BuildingTools-Languages folder)\r\nLanguage=").t(c.language).t("\r\n")
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
    g_cfg.keyMenu      = ini_int(L"Keys", L"Menu", VK_HOME);
    g_cfg.refundPopup  = ini_int(L"Interface", L"RefundPopup", 1);
    g_cfg.preview      = ini_int(L"Area", L"Preview", 1);
    g_cfg.hoverDist10  = ini_int(L"Area", L"FreePlacementDistance", 40);
    g_cfg.panelScale   = ini_int(L"Interface", L"PanelScale", 100);
    if (g_cfg.panelScale < 50) g_cfg.panelScale = 50;
    if (g_cfg.panelScale > 300) g_cfg.panelScale = 300;
    g_cfg.panelBottom  = ini_int(L"Interface", L"PanelBottom", 0);
    g_cfg.panelTimeout = ini_int(L"Interface", L"PanelTimeout", 5);
    g_cfg.panelKeys    = ini_int(L"Interface", L"PanelKeys", 1);
    g_cfg.hudRows      = ini_int(L"Interface", L"HudRows", 1);
    g_cfg.creative     = ini_int(L"Creative", L"CreativeBuilding", 0) != 0;
    g_cfg.keyOff       = ini_int(L"Keys", L"ToggleMod", VK_F5);
    g_cfg.fillLimit    = ini_int(L"Limits", L"FillLimit", 500);
    g_cfg.globalChests = ini_int(L"Chests", L"GlobalMagicChests", 0) != 0;
    bool noLanguage;
    {
        wchar_t p[MAX_PATH], w[16];
        lstrcpyW(p, g_dir); lstrcatW(p, L"BuildingTools.ini");
        GetPrivateProfileStringW(L"Interface", L"Language", L"?", w, 16, p);
        noLanguage = w[0] == L'?' && !w[1];
        if (noLanguage) lstrcpyW(w, L"auto");
        int k = 0;
        for (; w[k] && k < 15; k++) g_cfg.language[k] = w[k] < 128 ? (char)w[k] : '?';
        g_cfg.language[k] = 0;
    }
    if (g_cfg.maxShapes < 1) g_cfg.maxShapes = 1;
    if (g_cfg.maxShapes > 20000) g_cfg.maxShapes = 20000;
    if (g_cfg.fillLimit < 1) g_cfg.fillLimit = 1;
    if (g_cfg.fillLimit > g_cfg.maxShapes) g_cfg.fillLimit = g_cfg.maxShapes;
    // first start or an ini from an older version: write every option (values kept)
    if (ini_int(L"Keys", L"CycleAreaFill", -1) == -1 || ini_int(L"Interface", L"RefundPopup", -1) == -1 ||
        ini_int(L"Area", L"Preview", -1) == -1 || ini_int(L"Creative", L"CreativeBuilding", -1) == -1 ||
        ini_int(L"Keys", L"ToggleMod", -1) == -1 || ini_int(L"Limits", L"FillLimit", -1) == -1 ||
        ini_int(L"Chests", L"GlobalMagicChests", -1) == -1 || ini_int(L"Keys", L"Menu", -1) == -1 || noLanguage)
        write_full_ini(g_cfg);
}

// -------------------------------------------------------------- localization
// The texts follow the game's language: its in-game override (user_settings.json in the
// Steam cloud folder), else the Steam language, else English. Language= in the ini
// picks one by hand. English is built in; the others are read from
// BuildingTools-Languages\<code>.txt (UTF-8, "key = text" lines) when the language is set.
#include "strings.h"

static const char* const kStrKey[S_COUNT] = {
#define BT_STR_KEY(id, key, en) key,
    BT_STRINGS(BT_STR_KEY)
#undef BT_STR_KEY
};
static const char* const kStrEn[S_COUNT] = {
#define BT_STR_EN(id, key, en) en,
    BT_STRINGS(BT_STR_EN)
#undef BT_STR_EN
};
static const char* volatile g_str[S_COUNT];   // the current language (null = English)
static volatile LONG g_lang = 0;               // index in kLanguages, -1 = the test language
static volatile LONG g_keysVersion = 0;        // bumped when a key is rebound or the language changes

static const char* T(Str s) { const char* t = g_str[s]; return t ? t : kStrEn[s]; }

// A number, or tenths as "4.0" with the language's decimal mark, for Buf::fmt.
struct Num { char s[24]; };
static Num num(int64_t v) { Num n; Buf b; b.i(v); lstrcpynA(n.s, b.z(), sizeof n.s); return n; }
static Num tenths(int t) { Num n; Buf b; b.i(t / 10).str(T(S_DECIMAL)).i(t % 10); lstrcpynA(n.s, b.z(), sizeof n.s); return n; }

// Copies UTF-8 text, never cutting a character in two.
static void copy_text(char* d, const char* s, int cap) {
    int n = 0;
    while (s[n] && n < cap - 1) n++;
    if (s[n]) while (n > 0 && ((uint8_t)s[n] & 0xc0) == 0x80) n--;
    memcpy(d, s, n); d[n] = 0;
}

static bool same_ascii(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

// Whole file, zero terminated (never freed: the texts point into it).
static char* read_file(const wchar_t* path, DWORD* size = nullptr) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    DWORD n = GetFileSize(h, nullptr), got = 0;
    char* d = n < (8u << 20) ? (char*)HeapAlloc(GetProcessHeap(), 0, n + 1) : nullptr;
    if (d && (!ReadFile(h, d, n, &got, nullptr) || got != n)) { HeapFree(GetProcessHeap(), 0, d); d = nullptr; }
    CloseHandle(h);
    if (d) { d[n] = 0; if (size) *size = n; }
    return d;
}

static int lang_by_code(const char* code) {
    for (int k = 0; k < kLanguageCount; k++) if (same_ascii(code, kLanguages[k].code)) return k;
    return same_ascii(code, "test") ? -1 : -2;
}

// The game's own language choice (Settings > Gameplay): "languageIdOverride" in
// <Steam>\userdata\<account>\1203620\remote\user_settings.json, "None" = the Steam language.
static int lang_from_game() {
    wchar_t steam[MAX_PATH] = L"";
    if (HMODULE sc = GetModuleHandleW(L"steamclient64.dll")) GetModuleFileNameW(sc, steam, MAX_PATH);
    int cut = 0;
    if (steam[0]) cut = 1;
    else { lstrcpyW(steam, g_dir); cut = 4; }    // <Steam>\steamapps\common\Enshrouded\ (default library)
    for (int i = lstrlenW(steam); i > 0 && cut; i--)
        if (steam[i - 1] == L'\\') { steam[i] = 0; if (--cut) steam[i - 1] = 0; }
    wchar_t pat[MAX_PATH], best[MAX_PATH] = L"", f[MAX_PATH];
    lstrcpyW(pat, steam); lstrcatW(pat, L"userdata\\*");
    WIN32_FIND_DATAW fd; FILETIME newest = {};
    HANDLE fh = FindFirstFileW(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) return -2;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (lstrlenW(steam) + lstrlenW(fd.cFileName) > MAX_PATH - 60) continue;
        lstrcpyW(f, steam); lstrcatW(f, L"userdata\\"); lstrcatW(f, fd.cFileName); lstrcatW(f, L"\\1203620\\remote\\user_settings.json");
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(f, GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &newest) > 0) {
            newest = a.ftLastWriteTime; lstrcpyW(best, f);
        }
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    if (!best[0]) return -2;
    char* js = read_file(best);
    if (!js) return -2;
    int r = -2;
    static const char key[] = "\"languageIdOverride\"";
    for (char* q = js; *q; q++) {
        if (memcmp(q, key, sizeof key - 1)) continue;
        q += sizeof key - 1;
        while (*q == ' ' || *q == '\t' || *q == ':') q++;
        if (*q++ != '"') break;
        char v[16]; int n = 0;
        while (*q && *q != '"' && n < 15) v[n++] = *q++;
        v[n] = 0;
        for (int k = 0; k < kLanguageCount && r < 0; k++)
            for (const char* g : kLanguages[k].game) if (g && same_ascii(v, g)) r = k;
        LOG(.str("language: game setting ").str(v));
        break;
    }
    HeapFree(GetProcessHeap(), 0, js);
    return r;
}

static int lang_from_steam() {
    HMODULE m = GetModuleHandleW(L"steam_api64.dll");
    if (!m) return -2;
    typedef void* (*AppsFn)();
    typedef const char* (*LangFn)(void*);
    AppsFn apps = (AppsFn)GetProcAddress(m, "SteamAPI_SteamApps_v009");
    if (!apps) apps = (AppsFn)GetProcAddress(m, "SteamAPI_SteamApps_v008");
    LangFn lang = (LangFn)GetProcAddress(m, "SteamAPI_ISteamApps_GetCurrentGameLanguage");
    void* a = apps ? apps() : nullptr;
    const char* v = a && lang ? lang(a) : nullptr;
    if (!v) return -2;
    LOG(.str("language: Steam ").str(v));
    for (int k = 0; k < kLanguageCount; k++)
        for (const char* n : kLanguages[k].steam) if (n && same_ascii(v, n)) return k;
    return -2;
}

// Loads a language's texts. Keys missing from its file stay English.
static void lang_load(int k) {
    const char* next[S_COUNT] = {};
    int found = 0;
    if (k == -1) {                               // test language: longer, marked texts, to spot overflow and missed texts
        static char buf[S_COUNT * 40 + 16384];
        int n = 0;
        for (int s = 0; s < S_COUNT; s++) {
            const char* e = kStrEn[s];
            int len = lstrlenA(e);
            if (s == S_DECIMAL || !len) { next[s] = e; continue; }
            if (n + len + len / 2 + 8 > (int)sizeof buf) break;
            next[s] = buf + n;
            buf[n++] = '[';
            memcpy(buf + n, e, len); n += len;
            for (int x = 0; x < len * 4 / 10 + 1; x++) buf[n++] = '~';
            buf[n++] = ']'; buf[n++] = 0;
            found++;
        }
    } else if (k > 0) {
        wchar_t p[MAX_PATH], code[16];
        int c = 0;
        for (; kLanguages[k].code[c]; c++) code[c] = (wchar_t)kLanguages[k].code[c];
        code[c] = 0;
        lstrcpyW(p, g_dir); lstrcatW(p, L"BuildingTools-Languages\\"); lstrcatW(p, code); lstrcatW(p, L".txt");
        DWORD size = 0;
        char* d = read_file(p, &size);
        if (!d) LOG(.str("language: ").str(kLanguages[k].code).str(".txt not found in BuildingTools-Languages, using English"));
        char* q = d;
        if (q && (uint8_t)q[0] == 0xef && (uint8_t)q[1] == 0xbb && (uint8_t)q[2] == 0xbf) q += 3;   // UTF-8 BOM
        while (q && *q) {
            char* line = q;
            while (*q && *q != '\n') q++;
            if (*q) *q++ = 0;
            char* end = line + lstrlenA(line);
            while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
            while (*line == ' ' || *line == '\t') line++;
            if (!*line || *line == '#' || *line == ';') continue;
            char* eq = line;
            while (*eq && *eq != '=') eq++;
            if (!*eq) continue;
            char* ke = eq;
            while (ke > line && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            *ke = 0;
            char* v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            char* w = v;                         // "\n" in a text is a line break
            for (char* r = v; *r; r++) { if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r++; } else *w++ = *r; }
            *w = 0;
            for (int s = 0; s < S_COUNT; s++) if (!lstrcmpA(line, kStrKey[s])) { next[s] = v; found++; break; }
        }
    }
    for (int s = 0; s < S_COUNT; s++) g_str[s] = next[s];
    g_lang = k;
    InterlockedIncrement(&g_keysVersion);       // key names ("Ctrl+") again
    LOG(.str("language: ").str(k >= 0 ? kLanguages[k].code : "test").c(' ').i(found).c('/').i(S_COUNT).str(" texts"));
}

// The language to use now: the ini choice, else the game's.
static int lang_wanted() {
    if (!same_ascii(g_cfg.language, "auto")) {
        int k = lang_by_code(g_cfg.language);
        if (k >= -1) return k;
        LOG(.str("language: unknown Language=").str(g_cfg.language).str(" in the ini, following the game"));
    }
    int k = lang_from_game();
    if (k < 0) k = lang_from_steam();
    return k < 0 ? 0 : k;
}
static volatile LONG g_langSet = 0;
// At start, and again when the menu opens (the game's language may have changed).
static void lang_update() {
    int k = lang_wanted();
    if (!g_langSet || k != g_lang) { g_langSet = 1; lang_load(k); }
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
// The "may remove" material table (48f1c0) as RemoveShape and RemoveCommit build it
// (connected delete narrows it to the clicked material).
static const char* SIG_REMOVE_TABLE_DRY = "E8 ?? ?? ?? ?? 48 8B 85 D0 00 00 00 4C 8D 4C 24 50 4C 8B 46 30 33 D2 48 8B 8E 30 01 00 00";
static const char* SIG_REMOVE_TABLE_COMMIT = "E8 ?? ?? ?? ?? 66 0F 6F 05 ?? ?? ?? ?? 48 8D 85 90 00 00 00 4C 8B 47 30 4C 8D 4C 24 50";
// Turns per-material removed volumes into refund entries (called by RemoveShape).
static const char* SIG_REFUND_BUILD = "4C 8B DC 55 53 56 57 41 55 41 57 49 8D 6B A1 48 81 EC E8 00 00 00 49 8B 58 08";
// Density check inside the voxel blit callback; patched to run src/cave.S.
static const char* SIG_BLIT_CHECK =
    "41 8B 4F 08 41 8B 47 04 41 03 CD 41 0F AF C8 03 C5 41 0F AF 44 24 10 4D 8B 04 24 03 C8 41 03 0F 03 CF 8B D1 "
    "45 0F B6 1C 48 41 80 FB 80 0F 83 ?? ?? ?? ?? 4D 3B 5A 28 0F 83 ?? ?? ?? ?? 49 8B 42 20 41 80 3C 03 00 0F 84 ?? ?? ?? ?? "
    "41 80 BA A0 00 00 00 00 0F 84 ?? ?? ?? ??";
// patch the existing-material checks (cmp r11b,80h ... je skip): 35 bytes at +0x29
static const int kBlitPatchOff = 0x29, kBlitPatchLen = 35, kBlitSkipOff = 0x104;
// Drop the oldest undo step.
static const char* SIG_POP_UNDO = "48 89 5C 24 10 48 89 6C 24 18 57 41 56 41 57 48 83 EC 30 48 8B B9 08 0A 00 00";
// HUD building update (plays the build sounds); hooked to play the mod's sounds.
// The play-state UI ("doPlayStateUi()"), built every frame. Checks the offsets the menu
// uses: hud = [[[ui ctx]+0xd0]]+0x38, current HudScreenType byte at hud+0x1cd8.
static const char* SIG_PLAY_UI =
    "4C 8B DC 55 49 8D AB E8 FD FF FF 48 81 EC 10 03 00 00 48 8B 05 ?? ?? ?? ?? 48 8B 88 D0 00 00 00 48 8B 01 "
    "48 85 C0 0F 84 ?? ?? ?? ?? 4D 89 73 C8 4C 8B 70 38 4C 89 B5 30 02 00 00 4D 85 F6 0F 84 ?? ?? ?? ?? "
    "4D 89 63 D8 4D 8D A6 D8 1C 00 00 41 0F B6 04 24";
// Its only caller: E8 <play ui>, then three more UI calls.
static const char* SIG_PLAY_UI_CALL = "EB 05 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 4C 24 60 E8";
// Screen dispatch inside it: screen 52 (TechGallery, a developer test screen) calls
// the function at the je target, screen 53 (YcPlayground) the next one.
static const char* SIG_SCREEN_DISPATCH = "48 8B 45 80 0F B6 08 83 E9 34 0F 84 ?? ?? ?? ?? 83 F9 01 0F 84";
// toggleHudScreen(ui ctx, screen): queues a screen change ("[GameUi] toggleHudScreen to
// %k" in the game log); the game then switches input and cursor like for its own menus.
static const char* SIG_TOGGLE_SCREEN =
    "48 89 5C 24 10 48 89 74 24 18 57 48 81 EC 80 00 00 00 48 8B 81 D0 00 00 00 0F B6 F2 48 8B 98 C0 3C 03 00";
// beginWindow(window, name, fullscreen, layer, inputMask, flags). Flags bit 0 makes the
// window modal: windows on lower layers (the game world among them) get no input.
static const char* SIG_BEGIN_WINDOW = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 45 8B D9";
// The game's immediate-mode UI building blocks the menu draws with (a box is 0x28 bytes,
// its layout node at +8; a text 0x150).
static const char* SIG_UI_WINDOW_CTX   = "48 8B 41 10 C3 CC CC CC CC CC CC CC CC CC CC CC 48 8B 41 10 48 83 C0 20";
static const char* SIG_UI_BEGIN_ROOT   = "40 53 48 83 EC 20 48 8B D9 48 C7 41 10 00 00 00 00 48 C7 01 00 00 00 00";
static const char* SIG_UI_BEGIN_BOX    = "40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B D9 45 33 C0 48 8B 48 18 48 89 0B";
static const char* SIG_UI_END_BOX      = "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 60 80 79 20 00 48 8B D9 74 6A";
static const char* SIG_UI_END_WINDOW   = "40 53 48 83 EC 20 48 89 74 24 38 48 8B D9 48 8B 35 ?? ?? ?? ?? 48 89 7C 24 40";
static const char* SIG_UI_BACKGROUND   = "48 89 5C 24 08 57 48 83 EC 60 48 8B 1A 48 8B F9 48 8B 49 08 0F 29 74 24 50";
static const char* SIG_UI_PADDING      = "0F 10 02 0F 11 81 A8 00 00 00 C3 CC CC CC CC CC 0F 57 C0 F3 0F 5F C1 F3 0F 11 81 84 00 00 00";
static const char* SIG_UI_SPACING      = "C7 81 B8 00 00 00 02 00 00 00 44 0F B6 89 BC 00 00 00 41 0F B6 C1 41 80 C9 02";
static const char* SIG_UI_SELF_ALIGN   = "0F B7 02 66 89 41 6C C3 CC CC CC CC CC CC CC CC 0F 10 02 0F 11 81 98 00 00 00";
static const char* SIG_UI_LAYOUT       = "0F B7 02 66 89 41 6E C3 CC CC CC CC CC CC CC CC 0F B7 02 66 89 41 6C C3";
static const char* SIG_UI_STRETCH      = "48 89 91 90 00 00 00 C3 CC CC CC CC CC CC CC CC 88 51 6D C3 CC CC CC CC";
static const char* SIG_UI_WIDTH        = "0F 57 C0 C7 81 90 00 00 00 00 00 00 00 F3 0F 5F C8 F3 0F 11 49 78 F3 0F 11 89 88 00 00 00";
static const char* SIG_UI_FANCY_BOX    = "48 8B C4 55 41 56 48 8D A8 B8 FE FF FF 48 81 EC 38 02 00 00 48 89 58 20";
static const char* SIG_UI_BEGIN_TEXT   = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 50 48 8B DA 41 0F B6 F9";
static const char* SIG_UI_END_TEXT     = "40 55 56 57 48 8D 6C 24 B9 48 81 EC F0 00 00 00 0F 29 BC 24 B0 00 00 00";
// settingOption(params, u8* value): a settings row with arrows, as in the game's options.
// Its start is not unique (an identical copy exists), so it is found by a call: E8 at +7.
static const char* SIG_UI_SETTING_OPTION_CALL = "48 8D 8D 20 04 00 00 E8 ?? ?? ?? ?? 0F B6 8D B0 66 00 00 44 0A E0 8B C1";
// settingHeader(TextString* title, float gap): section title of the game's options.
static const char* SIG_UI_SETTING_HEADER = "48 81 EC A8 01 00 00 48 83 79 08 00 0F 29 B4 24 90 01 00 00 0F 28 F1 0F 84 ?? ?? ?? ??";
// doButton(params 0xa8): the game's button; true when clicked.
static const char* SIG_UI_BUTTON       = "48 8B C4 48 81 EC C8 00 00 00 0F 10 41 78 48 8D 54 24 20 F2 0F 10 89 88 00 00 00";
// setSpacingX: children side by side (setSpacing stacks them vertically).
static const char* SIG_UI_SPACING_X    = "45 0F B6 D0 C7 81 B8 00 00 00 01 00 00 00 44 0F B6 89 BC 00 00 00 41 0F B6 C1";
// blurBehind(ui ctx, rect, color): the blur behind the game's full-screen menus;
// fillRect(rect, color): a flat colour over a rect (their dark tint).
static const char* SIG_UI_BLUR         = "48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 81 EC E0 00 00 00 49 8B F8";
static const char* SIG_UI_FILL_RECT    = "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";
// menuTabBar(menu, flag): the tab bar at the top of the game's full-screen menus (Q/E).
// It gets its tabs from menuTabList (call at +0x19) and draws each with a tab widget
// (call at +0xe); the mod hooks both calls to show its own tabs and take their clicks.
static const char* SIG_UI_TAB_BAR       = "48 8B C4 48 89 58 08 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 38 F8 FF FF";
static const char* SIG_UI_TAB_LIST_CALL = "41 0F B6 CE 48 89 85 D8 01 00 00 48 89 BD E0 01 00 00 48 89 BD C8 01 00 00";
static const char* SIG_UI_TAB_DRAW_CALL = "48 8D 54 24 30 48 8D 4D 90 0F 11 44 24 30 E8 ?? ?? ?? ?? 84 C0 74 43 40 38 7D C0";
// makeHint(hint 0x48, UiActionId, enabled) and drawHints({hints, count}, alignment,
// padding, 0, 0): the key-hint bar at the bottom of the game's menus.
static const char* SIG_UI_MAKE_HINT    = "40 53 48 83 EC 40 45 33 D2 66 C7 01 5B 00 4C 89 51 08 44 0F B6 CA 44 88 51 14";
static const char* SIG_UI_DRAW_HINTS   = "48 8B C4 48 89 58 10 48 89 70 18 55 57 41 56 48 8D A8 A8 FA FF FF 48 81 EC 40 06 00 00";
// Building HUD box (b5d4e0): its two key rows (E8 at +0xf of each signature) call the row
// function (bc4e20: action binding + label), the box is finished by the E8 at +0x68 of
// the second signature. bindingHint (b2f8b0) draws one key icon with a label.
// The small on/off mark of the bottom-left HUD slots (c28190): image context getter (E8 at
// +0x40), resolveImage (E8 at +0x52), drawImage (E8 at +0x62); textures fancyToggleSmallOn
// bundle+0xfed4 (colour white), fancyToggleSmallOff bundle+0xfec4 (colour bundle+0xd4d8).
static const char* SIG_UI_SMALL_TOGGLE = "48 81 C3 D4 FE 00 00 C7 85 B0 00 00 00 FF FF FF FF C7 85 B4 00 00 00 00 00 80 3F EB 23 48 8B 88 C8 00 00 00 "
    "48 8D B5 B8 00 00 00 48 8B 81 D8 D4 00 00 48 8D 99 C4 FE 00 00 48 89 85 B8 00 00 00 E8 ?? ?? ?? ?? 4C 8B C3 48 8D 95 A8 00 00 00 "
    "48 8B C8 E8 ?? ?? ?? ?? 48 8B D0 48 8D 4C 24 50 4C 8B C6 E8";
// The building box's toggle row (ccff30, "first person view"): after the key row a
// stretcher (b1af90(box, {1,1}), E8 at +0xf) and the switch (ccfd00(bool on), E8 at +0x18).
static const char* SIG_HUD_TOGGLE_ROW = "0F 28 C7 48 8D 4D B0 0F 14 C7 66 48 0F 7E C2 E8 ?? ?? ?? ?? 40 0F B6 CE E8 ?? ?? ?? ?? 48 8D 4D B0 E8";
// The snapping hint's soft backdrop (cab59c): draw pass check, blend mode, then a nine-slice
// (118bc50(list, rect {x,y,w,h}, image, borders, corner radii, colour)) of the texture at
// bundle+0xd3b0 with borders bundle+0xd3c0. E8s: pass getter +0x7, pass id +0xf (4 = draw),
// draw list +0x28, blend +0x32, nine-slice +0xf4.
static const char* SIG_UI_SOFT_BACKDROP =
    "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 83 F8 04 75 1E 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? "
    "?? 48 8B C8 E8 ?? ?? ?? ?? B2 03 48 8B C8 E8 ?? ?? ?? ?? F3 0F 10 45 98 F3 0F 5D 45 9C 48 8B 0D ?? ?? ?? ?? "
    "C7 85 28 02 00 00 FF FF FF FF C7 85 2C 02 00 00 00 00 80 3F 48 8B 81 C8 00 00 00 F3 0F 59 C6 0F C6 C0 00 0F "
    "11 44 24 40 0F 10 88 B0 D3 00 00 0F 10 90 C0 D3 00 00 0F 29 4C 24 60 0F 29 54 24 70 66 0F 7F 44 24 40 E8 ?? "
    "?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 83 F8 04 75 59 E8 ?? ?? ?? ?? 4C 8D 44 24 60 48 8B C8 48 8D 95 20 02 00 00 "
    "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 8B 18 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 8D 28 02 00 00 4C 8D "
    "4C 24 70 48 89 4C 24 28 48 8D 55 90 48 8D 4C 24 40 44 8B C3 48 89 4C 24 20 48 8B C8 E8";
static const char* SIG_UI_SIZE = "48 89 54 24 08 0F 57 C0 F3 0F 10 54 24 08 33 C0";
static const char* SIG_HUD_ROW_CALL1 = "0F 29 85 B0 00 00 00 F2 0F 11 8D C0 00 00 00 E8 ?? ?? ?? ?? 48 8B 3D ?? ?? ?? ?? E9";
static const char* SIG_HUD_ROW_CALL2 = "F2 0F 11 8D C0 00 00 00 0F 29 85 B0 00 00 00 E8 ?? ?? ?? ?? 80 7D 36 00";
static const char* SIG_BINDING_HINT  = "40 55 56 57 48 8D AC 24 E0 FC FF FF 48 81 EC 20 04 00 00 44 8B 0A 48 8B FA 4C 8B C1";
static const char* SIG_HUD_UPDATE = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 88 FE FF FF 48 81 EC 78 02 00 00";
// mov rcx,[rip+uiCtx] ... movups xmm0,[rax+12B88h] (buildUndoSuccessful) ... call PlayUiSound
static const char* SIG_UI_SOUND = "48 8B 0D ?? ?? ?? ?? 44 89 B3 DC 1B 03 00 40 88 B3 D9 1B 03 00 48 8B 81 C8 00 00 00 0F 10 80 88 2B 01 00 0F 29 44 24 20 E8";
// place cursor transform from the aim ray
static const char* SIG_CURSOR_XF = "48 8B C4 55 53 56 57 41 56 48 8D 6C 24 90 48 81 EC 70 01 00 00 0F 29 70 C8";
// System dependency copy, and the dismantle system's call of it (writer at +0x180)
static const char* SIG_SYS_DEPS = "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 4C 8B 09 48 8B D9";
// Dismantle system: 9b83c0 call right before the aimed-entity check (piece or blocks)
static const char* SIG_PIECE_CHECK_CALL = "48 8B 95 F0 02 00 00 49 8B CD E8 ?? ?? ?? ?? 8B D8 85 C0 0F 85 ?? ?? ?? ?? 48 8B 85 F8 01 00 00";
static const char* SIG_DISM_DEPS = "41 B8 90 01 00 00 48 8D 95 F0 01 00 00 4C 8B E9 E8";
// Client cursor hover highlight (runs every frame)
static const char* SIG_HOVER_FX = "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 70 F2 FF FF";
// Creative building: the PayCost calls of the building systems (blueprints = place,
// dismantle = remove a shape / a prefab, undo) and the preview's missing-materials check.
static const char* SIG_PAY_PLACE   = "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B 4D F8 48 8B 45 B8 48 89 85 98 01 00 00";
static const char* SIG_PAY_REMOVE  = "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 44 8B 06 48 8D 54 24 70 0F 57 C0";
static const char* SIG_PAY_REMOVE_PREFAB = "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 44 8B 07 48 8D 54 24 70 48 8D 4D 30";
static const char* SIG_PAY_UNDO    = "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B 8D 40 01 00 00 E8 ?? ?? ?? ??";
// Magic chests of all bases. ChestList (468660): chest ids of the base at a position;
// ClientStock (4680b0): the client stock cache's chest iterator; DepositChests: the
// loot deposit's ChestList call; StockCount (469610): a stock map count; BaseAt
// (469060): index of the base whose area holds a position.
static const char* SIG_CHEST_LIST   = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 40 49 8B F9 49 8B E8";
static const char* SIG_CLIENT_STOCK = "48 89 4C 24 08 53 55 56 57 48 83 EC 58 8B 82 D4 16 00 00 49 8B D8 2B 82 D0 16 00 00";
static const char* SIG_DEPOSIT_CHESTS = "E8 ?? ?? ?? ?? 45 32 E4 48 8D 4D 10 44 88 A5 30 01 00 00 E8 ?? ?? ?? ??";
static const char* SIG_STOCK_COUNT  = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 41 8B D8 48 8B FA 4C 8B D1";
static const char* SIG_BASE_AT      = "48 89 5C 24 08 57 48 83 EC 10 8B 7A 24 45 33 DB 48 8B DA 4C 8B D1 48 85 FF";
// The material picker's item lists (g38_items::updateInventoryStocks)
static const char* SIG_STOCKS = "48 89 5C 24 08 44 88 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56";
// The picker's "selected material is not in the list" fallback, called by
// client_cycle_inventory_stock (it sends the chosen material like a scroll)
static const char* SIG_PICK_CALL = "E8 ?? ?? ?? ?? 80 7D 00 00 75 59 48 8B CE E8 ?? ?? ?? ?? 48 8B 4D C0 44 8B F0";
// The build preview's missing-materials check, called from the cursor systems
static const char* SIG_MISSING_CALL = "E8 ?? ?? ?? ?? 8B 13 8B CA 0F BA F1 0C 0F BA EA 0C 84 C0 0F 44 D1 89 13";
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

// ---- replace blit (src/cave.S)
extern "C" uint8_t bt_cave_start[], bt_cave_end[], bt_cave_mode[], bt_cave_cont[], bt_cave_skip[],
                   bt_cave_vols[], bt_cave_allow[], bt_cave_hits[];
static uint8_t* g_caveAllow = nullptr;
static volatile uint32_t* g_caveHits = nullptr;
// Materials Replace may repaint: those a normal removal could remove at that spot
// (from a removal dry run), so protected world blocks are never touched.
static uint8_t g_allow[256];
static volatile uint8_t* g_caveMode = nullptr;   // in the runtime copy
static uint32_t* g_caveVols = nullptr;
enum { STYLE_OVERLAP = 1, STYLE_FULL = 2, STYLE_CONNECTED = 3 };   // cave modes 1 and 2; connected = overlap
static volatile LONG g_modOff = 0;           // whole mod off (ToggleMod): vanilla game
static bool creative_on() { return g_cfg.creative && !g_modOff; }
static bool global_chests_on() { return g_cfg.globalChests && !g_modOff; }
static volatile LONG g_replaceStyle = STYLE_FULL;
static MaterialItemFn g_materialItem = nullptr;
static RemoveFn g_remove = nullptr;      // trampoline once hooked
static volatile DWORD g_ownRemoveTid = 0;  // set while the mod itself calls RemoveShape

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
static char g_flash[512];

static void flash(const char* msg) {
    copy_text(g_flash, msg, sizeof(g_flash));
    g_flashUntil = GetTickCount() + 2500;
}
// Sounds: the game's own UI sounds (FbUiBundle.sounds), played on the HUD thread.
// Sound=1 game sounds, 2 = Windows sounds, 0 = off.
enum { SND_CORNER = 0, SND_DONE, SND_FAIL, SND_TOGGLE, SND_COUNT };
static uint8_t** g_uiCtxSlot = nullptr;           // global UI context pointer
typedef void (*PlayUiSoundFn)(void* ctx, const void* soundRef);
static PlayUiSoundFn g_playUiSound = nullptr;
static volatile LONG g_sndQueue[SND_COUNT];      // pending plays per sound
static volatile DWORD g_sndTick[SND_COUNT];      // when each was queued
static volatile DWORD g_hudTick = 0;             // last HUD update (the game sounds play there)
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
static volatile DWORD g_buildTick = 0;   // last frame the hammer's place cursor was computed
static uint8_t* cursor_xf_hook(uint8_t* out, uint8_t* sys, uint8_t* q, uint8_t* ray) {
    uint8_t* r = g_cxOrig(out, sys, q, ray);
    if (!out || !sys || !q || q[3] != 4) return r;
    g_buildTick = GetTickCount();
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
// Executable memory within +-2 GB of `target` (for rel32 jumps). Small requests are
// carved out of one shared block (relays, trampolines, the replace blit), which saves
// one 64 KB allocation and a long VirtualAlloc search per hook.
static uint8_t* alloc_near_raw(uint8_t* target, size_t size) {
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
static uint8_t* alloc_near(uint8_t* target, size_t size) {
    static uint8_t* pool = nullptr; static size_t used = 0;
    const size_t kPool = 0x10000;
    size = (size + 15) & ~(size_t)15;
    auto in_range = [&](uint8_t* p) { int64_t d = p - target; return d > -0x70000000ll && d < 0x70000000ll; };
    if (size <= kPool / 4) {          // called from the init thread only
        if (!pool || used + size > kPool || !in_range(pool) || !in_range(pool + kPool)) {
            uint8_t* p = alloc_near_raw(target, kPool);
            if (!p) return nullptr;
            pool = p; used = 0;
        }
        uint8_t* r = pool + used; used += size;
        return r;
    }
    return alloc_near_raw(target, size);
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

// Redirects direct calls (E8 rel32) to `target` instead of patching its first bytes.
// Used for functions other mods hook or scan by their prologue (Creative Mode looks
// for PayCost and 9bc1a0 by their first bytes): the function itself stays untouched,
// so the order in which the mods start does not matter. `sites` lists the call
// instructions; with sites == nullptr every E8 whose target is `target` in .text is
// redirected (on the 1.0 client this byte scan finds exactly the real calls).
// Returns the number of calls redirected.
static int hook_calls(uint8_t* target, void* detour, uint8_t* const* sites, int nSites) {
    if (!target || !g_textBase) return 0;
    uint8_t* relay = alloc_near(target, 16);
    if (!relay) { LOG(.str("hook_calls: no memory near target")); return 0; }
    put_absjmp(relay, detour);
    auto patch = [&](uint8_t* at) -> bool {
        if (at < g_textBase || at + 5 > g_textBase + g_textSize || at[0] != 0xE8) return false;
        if (at + 5 + *(int32_t*)(at + 1) != target) return false;
        int64_t d = relay - (at + 5);
        if (d != (int32_t)d) return false;
        DWORD old;
        if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
        int32_t rel = (int32_t)d;
        memcpy(at + 1, &rel, 4);
        VirtualProtect(at, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, 5);
        return true;
    };
    int n = 0;
    if (sites) {
        for (int k = 0; k < nSites; k++) n += patch(sites[k]);
    } else {
        uint8_t* end = g_textBase + g_textSize - 5;
        for (uint8_t* p = g_textBase; p < end; p++)
            if (p[0] == 0xE8 && p + 5 + *(int32_t*)(p + 1) == target) n += patch(p);
    }
    return n;
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
static void piece_target_restore();
static uint64_t deps_hook(void* sys, uint8_t* out, uint32_t size) {
    uint64_t r = g_depsOrig(sys, out, size);
    if ((uint8_t*)__builtin_return_address(0) == g_dismDepsRet && sys && out && size >= 0x188) {
        piece_target_restore();
        void* world = *(void**)sys, *w = *(void**)(out + 0x180);
        if (w) {
            int k = 0;
            for (; k < 8; k++) if (g_pickWriters[k].world == world || !g_pickWriters[k].world) break;
            if (k < 8) { g_pickWriters[k].world = world; g_pickWriters[k].writer = w; }
        }
    }
    return r;
}
// ---- what the hammer holds
// The shape state the building functions get is the selected ItemInfo; its
// EquipmentSetup (+0x104) holds voxelData.isBuildingVoxel (+0x440), voxelObject
// (+0x458), terrainVoxelObject (+0x468) and voxelBlueprintMaterialPool (+0x4a4).
// Small shapes (first row of the shape picker) are building voxels; bigger shapes and
// building pieces (foundations, stairs...) are voxel objects placed as one entity.
// Bigger shapes take their material from a material pool, pieces have their own.
// The dismantle's block path takes terrain or building-voxel shapes only.
enum { ITEM_OTHER = 0, ITEM_PIECE = 1, ITEM_SHAPE = 2, ITEM_TERRAIN = 3, ITEM_BIG_SHAPE = 4 };
static int item_kind(const void* item) {
    const uint8_t* it = (const uint8_t*)item;
    if (!it || !readable(it + 0x440, 0x78)) return ITEM_OTHER;
    const bool vobj = (*(const uint64_t*)(it + 0x458) | *(const uint64_t*)(it + 0x460)) != 0;
    const bool terr = (*(const uint64_t*)(it + 0x468) | *(const uint64_t*)(it + 0x470)) != 0;
    const bool pool = (*(const uint64_t*)(it + 0x4a4) | *(const uint64_t*)(it + 0x4ac)) != 0;
    if (terr) return ITEM_TERRAIN;
    if (vobj) return it[0x440] ? ITEM_SHAPE : pool ? ITEM_BIG_SHAPE : ITEM_PIECE;
    return ITEM_OTHER;
}
static void log_item(const char* what, const void* item) {
    const uint8_t* it = (const uint8_t*)item;
    if (!it || !readable(it, 0x4b8)) { LOG(.str(what).str(": item not readable")); return; }
    static const char* kinds[] = { "other", "piece", "small shape", "terrain", "big shape" };
    LOG(.str(what).str(": item ").x(*(const uint32_t*)it).str(" kind ").str(kinds[item_kind(it)])
        .str(" building voxel ").i(it[0x440]).str(" object ").x(*(const uint64_t*)(it + 0x458))
        .str(" blueprint ").x(*(const uint64_t*)(it + 0x494)).str(" pool ").x(*(const uint64_t*)(it + 0x4a4))
        .str(" override ").i((int8_t)it[0x4b4]));
}
// Small shapes (the first row of the shape picker, one 0.5 m block) are the ones the
// fill bucket works with; bigger shapes replace and delete as a whole.
static const float kSmallShape = 0.51f;
static bool small_box(const Aabb* b) {
    for (int a = 0; a < 3; a++) if (b->mx[a] - b->mn[a] > kSmallShape) return false;
    return true;
}
static bool small_item(void* item) {
    if (!g_bounds || !item) return true;
    const float pos[4] = { 0.f, 0.f, 0.f, 0.f }, rot[4] = { 0.f, 0.f, 0.f, 1.f };
    Aabb b; memset(&b, 0, sizeof(b));
    g_bounds(&b, item, pos, rot, 1.f);
    return small_box(&b);
}

// ---- building pieces as blocks (Area remove, connected delete)
// The dismantle system removes a building piece (foundation, stairs...) as one entity
// when the aimed entity flag of its state at [rbp+0x1f8] is set (+0x80 == 1, entity at
// +0x78); otherwise it takes the block path (RemoveShape, RemoveCommit with the hammer's
// shape). In Area mode and connected delete the mod clears the flag between the
// dismantle's 9b83c0 call and its read at 2d43de, so pieces are cut like blocks. The
// redirected call goes through piece_check_thunk, which passes the dismantle's rbp. The
// flag is put back at the mod's next RemoveShape call or the next dismantle run.
typedef uint64_t (*PieceCheckFn)(void*, void*, uint64_t, void*);
static PieceCheckFn g_pieceCheckOrig = nullptr;
static uint8_t* volatile g_pieceFlag = nullptr;     // cleared flag byte to put back
static void piece_target_restore() {
    uint8_t* f = (uint8_t*)InterlockedExchangePointer((void* volatile*)&g_pieceFlag, nullptr);
    if (f && *f == 0) *f = 1;
}
extern "C" uint64_t bt_piece_check(void* a, void* b, uint64_t c, void* d, uint8_t* frame, void* item) {
    uint64_t r = g_pieceCheckOrig(a, b, c, d);
    if ((uint32_t)r || g_modOff) return r;
    const bool connected = g_mode == MODE_PREFAB && g_replace && g_replaceStyle == STYLE_CONNECTED;
    if (g_mode != MODE_AREA && !connected) return r;
    // a piece in the hammer, or a big shape with the fill bucket: the game's own removal
    const int kind = item_kind(item);
    log_item("dismantle", item);
    if (kind != ITEM_SHAPE && kind != ITEM_TERRAIN) return r;
    if (g_mode != MODE_AREA && !small_item(item)) return r;
    uint8_t* st = frame && readable(frame + 0x1f8, 8) ? *(uint8_t**)(frame + 0x1f8) : nullptr;
    if (!st || !readable(st, 0x88)) { LOG(.str("dismantle: aim state not readable")); return r; }
    if (st[0x80] != 1 || !*(uint64_t*)(st + 0x78)) return r;
    st[0x80] = 0;
    g_pieceFlag = st + 0x80;
    LOG(.str("dismantle: building piece aimed, removed as blocks (").str(connected ? "connected delete" : "area").c(')'));
    return r;
}
// rcx, rdx, r8, r9 pass through; the caller's rbp and rsi (the selected item) become
// the 5th and 6th arguments
extern "C" uint8_t piece_check_thunk[];
asm(".text\n"
    ".globl piece_check_thunk\n"
    "piece_check_thunk:\n"
    "    push %rbp\n"
    "    sub $0x30, %rsp\n"
    "    mov %rbp, 0x20(%rsp)\n"
    "    mov %rsi, 0x28(%rsp)\n"
    "    call bt_piece_check\n"
    "    add $0x30, %rsp\n"
    "    pop %rbp\n"
    "    ret\n");
static void* pickup_writer_for(uint8_t* inv) {
    void* sys = *(void**)inv;
    if (!sys || !readable(sys, 8)) return nullptr;
    void* world = *(void**)sys;
    for (int k = 0; k < 8; k++) if (g_pickWriters[k].world == world) return g_pickWriters[k].writer;
    return nullptr;
}
// ---- magic chests of all bases (GlobalMagicChests)
// The game's PlayerBases system (one in the server world, one in the client world) keeps
// the number of bases at +0x24 and up to 16 base records of 0x130 bytes from +0x30.
// A record starts with the base's magic chest entity ids {u32* ids, u64 count}; its
// area box is at +0xf0 (min xyz, max xyz at +0x100) and a mask of sub-boxes at +0x110.
// Three places use "the chests of the base the player stands in":
// 1. Server payment and crafting: inventory transactions get the chest list from
//    ChestList (468660). The mod returns the chests of every base (the current base
//    first), also outside every base. Not for the loot deposit, and not for client
//    worlds, whose UI checks read the client stock map below instead.
// 2. Client stock map (picker, ghost, crafting menu): cache_crafting_stock walks the
//    current base's chests through ClientStock (4680b0), 40 per frame. For the length of
//    each call the mod points that base's chest list at a list of every base's chests.
// 3. Server cursor check (server_cursor_no_prediction -> 2865e0 -> StockCount 469610):
//    the server's per-base stock maps do not hold what the chests contain, so for a
//    material only found in another base the server dropped the click before anything
//    was paid. The server check passes; the payment (1) is what refuses a placement
//    when there is not enough anywhere.
// Chests whose entity is not loaded are skipped by the game itself.
static const int kMaxBases = 16;
static const int kMaxChests = 16384;
static uint8_t* base_rec(uint8_t* sys, int b) { return sys + 0x30 + (size_t)b * 0x130; }
static int base_count(uint8_t* sys) {
    uint32_t n = *(uint32_t*)(sys + 0x24);
    return n > (uint32_t)kMaxBases ? 0 : (int)n;
}
// Every base's chest ids into buf, base `first` first (-1 = none). Returns the count.
static int collect_chests(uint8_t* sys, int first, uint32_t* buf, int cap) {
    int nb = base_count(sys), n = 0;
    if (!nb || !readable(base_rec(sys, 0), (size_t)nb * 0x130)) return 0;
    for (int k = -1; k < nb; k++) {
        int b = k < 0 ? first : k;
        if (b < 0 || (k >= 0 && b == first)) continue;
        uint8_t* rec = base_rec(sys, b);
        uint32_t* ids = *(uint32_t**)rec; uint64_t c = *(uint64_t*)(rec + 8);
        if (!ids || !c || c > (uint64_t)kMaxChests || !readable(ids, (size_t)c * 4)) continue;
        for (uint64_t i = 0; i < c && n < cap; i++) if (ids[i]) buf[n++] = ids[i];
    }
    return n;
}

// -- 2. client stock map (also registers the client worlds' systems for 1 and 3)
typedef uint8_t* (*ClientStockFn)(uint8_t* out, uint8_t* sys, const float* pos, uint32_t player);
typedef uint8_t* (*BaseAtFn)(uint8_t* out, uint8_t* sys, const float* pos);
static ClientStockFn g_clientStockOrig = nullptr;
static BaseAtFn g_baseAt = nullptr;          // 469060: index of the base whose area holds a point
struct ClientChests {
    uint8_t* sys;
    int here;        // base the player stood in when the list was built (-1 = none)
    int base;        // base whose chest list is swapped (-1 = none found)
    float at[3];     // a point in `base`, used while the player is outside every base
    int n; uint32_t buf[kMaxChests];
};
static ClientChests* volatile g_clientChests[4];   // one per client world
static bool is_client_sys(uint8_t* sys) {
    for (ClientChests* c : g_clientChests) if (c && c->sys == sys) return true;
    return false;
}
static ClientChests* client_chests(uint8_t* sys) {
    for (auto& s : g_clientChests) {
        ClientChests* c = s;
        if (c && c->sys == sys) return c;
        if (c) continue;
        c = (ClientChests*)VirtualAlloc(nullptr, sizeof(ClientChests), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!c) return nullptr;
        c->sys = sys; c->here = -2; c->base = -1;
        if (InterlockedCompareExchangePointer((void* volatile*)&s, c, nullptr) == nullptr) return c;
        VirtualFree(c, 0, MEM_RELEASE);
        if (s->sys == sys) return s;
    }
    return nullptr;
}
static int base_at(uint8_t* sys, const float* pos) {
    uint8_t r[16]; memset(r, 0, sizeof(r));
    g_baseAt(r, sys, pos);
    return r[0] == 0 ? r[8] : -1;
}
// A point the game places inside base b: the centre of its box, or of one of its sub-boxes.
static bool point_in_base(uint8_t* sys, int b, float* p) {
    uint8_t* rec = base_rec(sys, b);
    const float* box = (const float*)(rec + 0xf0);
    uint32_t mask = *(uint32_t*)(rec + 0x110);
    for (int a = 0; a < 3; a++) p[a] = (box[a] + box[a + 4]) * 0.5f;
    if (base_at(sys, p) == b) return true;
    uint8_t* sub = *(uint8_t**)(sys + 0x1330);
    if (!sub || !readable(sub, 0x200)) return false;
    for (int j = 0; j < 16; j++) {
        if (!(mask >> j & 1)) continue;
        const float* s = (const float*)(sub + j * 0x20);
        for (int a = 0; a < 3; a++) p[a] = (s[a] + s[a + 4]) * 0.5f;
        if (base_at(sys, p) == b) return true;
    }
    return false;
}
static uint8_t* client_stock_hook(uint8_t* out, uint8_t* sys, const float* pos, uint32_t player) {
    if (!global_chests_on() || !sys || !pos) return g_clientStockOrig(out, sys, pos, player);
    ClientChests* cc = client_chests(sys);
    int nb = cc ? base_count(sys) : 0;
    if (!nb) return g_clientStockOrig(out, sys, pos, player);
    // A pass over the chests starts with both chunk counters at 0. The list is rebuilt
    // only then (or when the player changes base), so it stays the same while the game
    // walks through it over several frames.
    bool fresh = *(uint32_t*)(sys + 0x16c8) == 0 && *(uint32_t*)(sys + 0x16cc) == 0;
    int here = base_at(sys, pos);
    if (fresh || here != cc->here || cc->base < 0) {
        int b = here, was = cc->n, wasBase = cc->base;
        // outside every base: gather for the first base the game can place a point in
        if (b < 0) for (int k = 0; k < nb && b < 0; k++) if (point_in_base(sys, k, cc->at)) b = k;
        cc->here = here; cc->base = b;
        cc->n = b >= 0 ? collect_chests(sys, b, cc->buf, kMaxChests) : 0;
        if (b >= 0 && (b != wasBase || cc->n != was))
            LOG(.str("global chests: picker counts ").i(cc->n).str(" chests in ").i(nb).str(" bases")
                .str(here >= 0 ? "" : " (outside every base)"));
    }
    int b = cc->base;
    if (b < 0) return g_clientStockOrig(out, sys, pos, player);
    const float* at = here >= 0 ? pos : cc->at;
    volatile uint64_t* slot = (volatile uint64_t*)base_rec(sys, b);
    uint64_t ids0 = slot[0], n0 = slot[1];
    if ((uint64_t)cc->n <= n0 && at == pos) return g_clientStockOrig(out, sys, pos, player);
    // The list starts with base b's own chests, so another reader that sees the new
    // pointer with the old count still reads valid ids: pointer first in, count first out.
    slot[0] = (uint64_t)cc->buf; slot[1] = (uint64_t)cc->n;
    uint8_t* r = g_clientStockOrig(out, sys, at, player);
    slot[1] = n0; slot[0] = ids0;
    return r;
}

// -- 1. chest lists for payments and crafting
typedef uint8_t* (*ChestListFn)(uint8_t* out, uint64_t* list, uint8_t* sysA, uint8_t* sysB, const float* pos, uint32_t player);
static ChestListFn g_chestListOrig = nullptr;
static uint8_t* g_depositRet = nullptr;     // return address of the loot deposit's ChestList call
struct ChestTls { int building; int pad; uint32_t buf[kMaxChests]; };   // per thread
static DWORD g_chestTls = TLS_OUT_OF_INDEXES;
static ChestTls* chest_tls(bool create) {
    if (g_chestTls == TLS_OUT_OF_INDEXES) return nullptr;
    ChestTls* t = (ChestTls*)TlsGetValue(g_chestTls);
    if (!t && create) {
        t = (ChestTls*)VirtualAlloc(nullptr, sizeof(ChestTls), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (t) TlsSetValue(g_chestTls, t);
    }
    return t;
}
// Marks the building systems' payments (pay_hook). Only needed when the loot deposit's
// call site is unknown: then only these payments get every base's chests.
struct ChestScope {
    ChestTls* t; int was;
    explicit ChestScope(bool on) : t(nullptr), was(0) {
        if (on && g_chestListOrig && !g_depositRet && (t = chest_tls(true))) { was = t->building; t->building = 1; }
    }
    ~ChestScope() { if (t) t->building = was; }
};
static uint8_t* chest_list_hook(uint8_t* out, uint64_t* list, uint8_t* sysA, uint8_t* sysB, const float* pos, uint32_t player) {
    uint8_t* ret = (uint8_t*)__builtin_return_address(0);
    uint8_t* r = g_chestListOrig(out, list, sysA, sysB, pos, player);
    // cheap tests first: the UI asks thousands of times per second
    if (!global_chests_on() || !out || !list || ret == g_depositRet) return r;
    uint8_t st = out[0];
    if (st != 0 && st != 0x1f) return r;              // 0 = found, 0x1f = not in a base
    uint8_t* sys = sysA ? sysA : sysB;
    if (!sys) return r;
    if (is_client_sys(sys)) {
        // The crafting menu picks "backpack only" when the base the player is in has no
        // chests (or the player is in no base), and "backpack and chests" otherwise; its
        // counts then come from the client stock map, which covers every base. Without
        // local chests it gets one chest of another base, so the menu uses the chests.
        // The list stays short: the menu asks thousands of times per second.
        if (list[1] == 0 && list[3] == 0)
            if (ClientChests* cc = client_chests(sys))
                if (cc->n > 0) { list[0] = (uint64_t)cc->buf; list[1] = 1; }
        return r;
    }
    ChestTls* t = chest_tls(g_depositRet != nullptr);
    if (!t || (!g_depositRet && !t->building)) return r;
    int nb = base_count(sys), cur = -1;
    if (st == 0 && list[1])
        for (int b = 0; b < nb; b++)
            if (*(uint64_t*)base_rec(sys, b) == list[0]) { cur = b; break; }
    int n = collect_chests(sys, cur, t->buf, kMaxChests);
    static volatile LONG s_log = 0;
    if (InterlockedIncrement(&s_log) <= 3)
        LOG(.str("global chests: a payment takes from ").i(n).str(" chests in ").i(nb).str(" bases")
            .str(st == 0 ? "" : " (outside every base)"));
    if (n <= 0) return r;
    list[0] = (uint64_t)t->buf; list[1] = (uint64_t)n;
    if (st != 0) { list[2] = 0; list[3] = 0; out[0] = 0; *(uint32_t*)(out + 4) = 0; }
    return r;
}

// -- 3. server cursor check
typedef uint32_t (*StockCountFn)(void* map, uint8_t* sys, uint32_t item, const float* pos);
static StockCountFn g_stockCountOrig = nullptr;
static uint32_t stock_count_hook(void* map, uint8_t* sys, uint32_t item, const float* pos) {
    // a system that is not a known client world's is the server's; until the client
    // world has shown up nothing is changed
    if (!global_chests_on() || !sys || !g_clientChests[0] || is_client_sys(sys))
        return g_stockCountOrig(map, sys, item, pos);
    return 0xffff;                                    // "plenty": the payment decides
}

static uint8_t* g_creativeSites[4];          // return addresses of the building PayCost calls
static volatile LONG g_creativeLog = 0;
static bool creative_site(uint8_t* ret) {
    for (uint8_t* s : g_creativeSites) if (s && s == ret) return true;
    return false;
}
static uint8_t pay_body(uint8_t* mode, CostList* list, uint8_t consume, uint8_t* ret);
// the cost list and size of the area / connected action priced last (pay feedback)
static CostList* volatile g_actList = nullptr;
static volatile int g_actShapes = 0;
static bool shrink_to_affordable(uint8_t* trans, CostList* list);
// Every PayCost call goes through here (redirected call sites). When it charges a list
// the mod removed blocks for (Replace), the removed materials are refunded through the
// same function.
static uint8_t pay_hook(uint8_t* mode, CostList* list, uint8_t consume) {
    uint8_t* ret = (uint8_t*)__builtin_return_address(0);
    ChestScope scope(global_chests_on() && creative_site(ret));
    return pay_body(mode, list, consume, ret);
}
static uint8_t pay_body(uint8_t* mode, CostList* list, uint8_t consume, uint8_t* ret) {
    if (creative_on() && creative_site(ret)) {
        // creative building: nothing is charged or given back
        if (list && list == g_refundList) { g_refundN = 0; g_refundDropped = 0; g_refundList = nullptr; }
        g_paidN = 0;
        if (InterlockedIncrement(&g_creativeLog) <= 5)
            LOG(.str("creative: ").str(consume ? "charge" : "refund").str(" of ").u(list ? list->count : 0)
                .str(" entries skipped (call at rva ").x((uint64_t)(ret - 5 - g_base)).c(')'));
        return 1;
    }
    if (!consume && list && list != g_refundList) {
        if (UndoRefund* u = undo_refund_find(list)) {           // undo of a Replace step
            CostList take = { u->e, (uint64_t)u->n, (uint64_t)u->n, nullptr };
            u->list = nullptr;
            if (!g_payOrig(mode, &take, 1)) {
                LOG(.str("undo of replace refused: the old material it gave back is not in the inventory"));
                flash(T(S_UNDO_NEEDS));
                beep(MB_ICONHAND);
                return 0;
            }
            uint8_t r = g_payOrig(mode, list, consume);
            if (!r) g_payOrig(mode, &take, 0);                    // undo failed: give it back again
            LOG(.str("undo of replace: took back ").i(u->n).str(" refund entries, game refund ").i(r));
            return r;
        }
    }
    if (creative_on() && consume && list) {
        static volatile LONG s_other = 0;
        if (InterlockedIncrement(&s_other) <= 10)
            LOG(.str("creative: charge at another call, rva ").x((uint64_t)(ret - 5 - g_base))
                .str(", ").u(list->count).str(" entries (paid normally)"));
    }
    // An area or connected action that costs more than the inventory holds is cut down
    // to the shapes it can pay for (in the order they were planned: bottom layer first,
    // or nearest cells first), instead of failing as a whole.
    if (consume && list && mode && mode[0] == 0 && g_actShapes > 1 && list == g_actList)
        shrink_to_affordable(*(uint8_t**)(mode + 8), list);
    uint8_t r = g_payOrig(mode, list, consume);
    // A mod action the inventory cannot pay for is dropped by the game without a word
    // (nothing is built or replaced), so say why.
    if (!r && consume && list && mode && mode[0] == 0 && g_actShapes > 1 && list == g_actList) {
        uint64_t items = 0;
        for (uint64_t k = 0; k < list->count; k++) items += *(uint32_t*)(((CostEntry*)list->data)[k].b + 0x14);
        Buf b; b.fmt(T(S_NOT_ENOUGH), num(g_actShapes).s, num((int64_t)items).s);
        flash(b.z()); beep(MB_ICONHAND);
        LOG(.str("charge refused: ").i(g_actShapes).str(" shapes, ").u(list->count).str(" cost entries, ").u(items).str(" items"));
    }
    if (consume && list == g_actList && mode && mode[0] == 0) { g_actList = nullptr; g_actShapes = 0; }   // charged once
    if (list && list == g_refundList) {
        LONG n = g_refundN;
        if (r && consume && mode && mode[0] == 0 && n > 0) {
            CostList tmp = { g_refund, (uint64_t)n, (uint64_t)n, nullptr };
            // Inventory transaction of this action: flag bit 0 at +0xb0 makes the game
            // send PickupEvents (the "+N item" pop-up) for added items when the
            // transaction is committed; the placement system leaves it off.
            uint8_t* inv = *(uint8_t**)(mode + 8);
            if (inv && readable(inv, 0x100)) {
                if (g_cfg.refundPopup) {
                    if (!*(void**)(inv + 0xb8)) *(void**)(inv + 0xb8) = pickup_writer_for(inv);
                    if (*(void**)(inv + 0xb8)) inv[0xb0] |= 1;
                    else LOG(.str("  no pick-up writer known for this world yet"));
                }
            }
            uint8_t rr = g_payOrig(mode, &tmp, 0);
            if (rr) { g_paidN = n < kMaxPaid ? n : kMaxPaid; memcpy(g_paid, g_refund, (size_t)g_paidN * sizeof(CostEntry)); g_paidTick = GetTickCount(); }
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
// The build preview's material check (true = too few materials: red preview,
// "Missing Materials", Build greyed out). Creative building: shapes never miss any.
typedef uint8_t (*MissingFn)(void* cursor, uint8_t* sel, float scale);
static MissingFn g_missingOrig = nullptr;
static uint8_t missing_hook(void* cursor, uint8_t* sel, float scale) {
    uint8_t r = g_missingOrig(cursor, sel, scale);
    if (r && creative_on() && sel && readable(sel + 0x60, 8)) {
        uint8_t* bp = *(uint8_t**)(sel + 0x60);
        if (bp && readable(bp + 0x458, 0x20) &&
            (*(uint64_t*)(bp + 0x458) | *(uint64_t*)(bp + 0x460) | *(uint64_t*)(bp + 0x468) | *(uint64_t*)(bp + 0x470)))
            return 0;      // a building shape or prefab (props keep the normal check)
        static volatile LONG s_n = 0;
        if (InterlockedIncrement(&s_n) <= 8)
            LOG(.str("creative: preview still missing materials, blueprint ").x((uint64_t)bp)
                .str(bp && readable(bp + 0x5d8, 4) ? " flags " : "").x(bp && readable(bp + 0x5d8, 4) ? *(uint32_t*)(bp + 0x5d8) : 0)
                .str(" sel+0x68 ").x(readable(sel + 0x68, 8) ? *(uint64_t*)(sel + 0x68) : 0));
    }
    return r;
}

// ---- creative: the material picker lists every building material
// The game builds the picker's lists in updateInventoryStocks (4076b0): seven stock
// lists of 0x648 bytes (entries {itemId, slot, count} of 0x18 bytes), filled from the
// backpack, the magic chests and the favourites. In creative building the mod adds
// every item of the terrain, block, roof and overgrowth item groups (count 0), with
// the game's own add function, right before the lists are finalised.
struct StockList { uint8_t* data; uint64_t count; uint64_t cap; void* grow; };
typedef void (*StocksFn)(void* stocks, void* ctx, uint8_t onlyShown);
typedef void (*StockFinalFn)(void* stocks, void* a, void* b, void* c, void* d, void* e);
typedef void (*StockAddFn)(void* list, const uint32_t* item, uint32_t container, uint64_t index, uint8_t keep);
typedef uint8_t* (*RegistryFn)(void* src);
typedef uint64_t (*IterBeginFn)(void* set);
typedef uint64_t (*IterNextFn)(void* set, uint64_t at);
static StocksFn g_stocksOrig; static StockFinalFn g_stockFinalOrig; static StockAddFn g_stockAdd;
static RegistryFn g_itemGroups; static IterBeginFn g_iterBegin; static IterNextFn g_iterNext;
static uint32_t* g_noContainer; static DWORD g_stocksTls = TLS_OUT_OF_INDEXES;
static volatile LONG g_stocksLog = 0;
struct StocksCall { uint8_t* ctx; uint8_t onlyShown; };
static void stocks_hook(void* stocks, void* ctx, uint8_t onlyShown) {
    StocksCall c = { (uint8_t*)ctx, onlyShown };
    TlsSetValue(g_stocksTls, &c);
    g_stocksOrig(stocks, ctx, onlyShown);
    TlsSetValue(g_stocksTls, nullptr);
}
// The item groups never change while the game runs, so their material ids are
// collected once per registry. The lists are rebuilt every frame: missing ids are
// found with a small hash set and appended directly (the game's add searches the
// list for every item, which cost ~20 fps for a few hundred materials).
static const int kMaxCreativeIds = 2048;
static uint32_t g_cIds[4][kMaxCreativeIds]; static int g_cN[4];
static uint8_t* g_cReg = nullptr;
static void collect_materials(uint8_t* reg) {
    for (int t = 0; t < 4; t++) g_cN[t] = 0;
    uint64_t end = *(uint64_t*)(reg + 0x10);
    int guard = 0;
    for (uint64_t at = g_iterBegin(reg + 8); at != end && guard < 200000; at = g_iterNext(reg + 8, at), guard++) {
        uint8_t* g = (*(uint8_t***)(reg + 0x40))[at];
        if (!g || !readable(g, 0x10)) continue;
        int type = g[0] - 0xe;                 // item group -> stock type (as the game maps it)
        static const int8_t map[7] = { 0, 1, 6, 2, 3, 4, 5 };
        if (type < 0 || type > 6) continue;
        int st = map[type];
        if (st < 2 || st > 5) continue;        // terrain, blocks, roof, overgrowth
        int32_t off = *(int32_t*)(g + 8); uint32_t n = *(uint32_t*)(g + 0xc);
        if (!off || !n) continue;
        uint32_t* ids = (uint32_t*)(g + 8 + off);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t id = ids[k]; if (!id) continue;
            int t = st - 2; bool dup = false;
            for (int j = 0; j < g_cN[t] && !dup; j++) dup = g_cIds[t][j] == id;
            if (!dup && g_cN[t] < kMaxCreativeIds) g_cIds[t][g_cN[t]++] = id;
        }
    }
    g_cReg = reg;
    LOG(.str("creative: materials terrain ").i(g_cN[0]).str(" blocks ").i(g_cN[1]).str(" roof ").i(g_cN[2])
        .str(" overgrowth ").i(g_cN[3]));
}
static void add_all_materials(uint8_t* stocks, StocksCall* c) {
    if (!c || !c->ctx || !readable(c->ctx + 0x68, 8)) return;
    uint8_t* reg = g_itemGroups(*(void**)(c->ctx + 0x68));
    if (!reg || !readable(reg, 0x48)) return;
    if (reg != g_cReg) collect_materials(reg);
    static uint32_t set[4096];                 // ids already listed (open addressing, 0 = empty)
    for (int t = 0; t < 4; t++) {
        int st = t + 2;
        // only the list the picker shows, and only in its "backpack and magic chests"
        // order (0): favourites, last used, amount and item sets stay as they are
        if (!g_cN[t] || stocks[0x2bf8] != st || stocks[st * 0x648 + 0x638] != 0) continue;
        StockList* list = (StockList*)(stocks + st * 0x648);
        if (list->count > 2048) continue;
        memset(set, 0, sizeof(set));
        for (uint64_t k = 0; k < list->count; k++) {
            uint32_t id = *(uint32_t*)(list->data + k * 0x18);
            uint32_t h = (id * 2654435761u) & 4095;
            while (set[h] && set[h] != id) h = (h + 1) & 4095;
            set[h] = id;
        }
        for (int k = 0; k < g_cN[t]; k++) {
            uint32_t id = g_cIds[t][k];
            uint32_t h = (id * 2654435761u) & 4095;
            while (set[h] && set[h] != id) h = (h + 1) & 4095;
            if (set[h] == id) continue;
            set[h] = id;
            if (list->count == 0 || list->count + 1 > list->cap) {
                // first entry or the list must grow: the game's add (it can reallocate)
                if (list->count + 1 > list->cap && !list->grow) break;
                uint32_t item[3] = { id, 0, 0 };
                g_stockAdd(list, item, *g_noContainer, 0, 1);
                continue;
            }
            // same entry the game's add writes for an item with no slot: {id, slot, count 0};
            // the slot value is copied from an entry the game added the same way
            uint8_t* e = list->data + list->count * 0x18;
            static uint64_t noSlot = 0; static bool haveSlot = false;
            if (!haveSlot) {
                uint32_t item[3] = { id, 0, 0 };
                uint64_t before = list->count;
                g_stockAdd(list, item, *g_noContainer, 0, 1);
                if (list->count == before + 1) { noSlot = *(uint64_t*)(list->data + before * 0x18 + 4); haveSlot = true; }
                continue;
            }
            *(uint32_t*)e = id; *(uint64_t*)(e + 4) = noSlot; *(uint64_t*)(e + 0x10) = 0;
            list->count++;
        }
    }
}

// 406020: when the material chosen last is not in the list, the game picks the
// selected entry (or the first) and sends it as the new choice, but refuses an entry
// with count 0 (result 6). Creative building accepts it, so a type with nothing in the
// inventory still gets a material without scrolling first.
typedef uint8_t* (*PickFn)(uint8_t* res, uint8_t* list, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);
static PickFn g_pickOrig = nullptr;
static uint8_t* pick_hook(uint8_t* res, uint8_t* list, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    uint8_t* r = g_pickOrig(res, list, a, b, c, d, e, f);
    if (creative_on() && res && res[0] == 6 && list) {
        StockList* l = (StockList*)list;
        uint64_t k = list[0x630] && *(uint64_t*)(list + 0x628) < l->count ? *(uint64_t*)(list + 0x628) : 0;
        if (k < l->count) {
            list[0x630] = 1; *(uint64_t*)(list + 0x628) = k;
            res[0] = 0; *(uint32_t*)(res + 4) = 0; *(uint32_t*)(res + 8) = *(uint32_t*)(l->data + k * 0x18);
        }
    }
    return r;
}
// Material counts of the picker's lists (types 2..5: terrain, blocks, roof, overgrowth),
// written by the client thread each frame into the back half and published by index.
struct StockCount { uint32_t id; uint32_t count; };
static const int kStockCache = 1024;
static StockCount g_stockCache[2][kStockCache];
static volatile LONG g_stockN[2], g_stockCur = 0;
static volatile DWORD g_stockTick = 0;
static void note_stock(uint8_t* stocks) {
    LONG back = g_stockCur ^ 1; int n = 0;
    for (int t = 2; t <= 5; t++) {
        StockList* l = (StockList*)(stocks + t * 0x648);
        if (!l->data || l->count > 4096) continue;
        for (uint64_t k = 0; k < l->count && n < kStockCache; k++) {
            uint8_t* en = l->data + k * 0x18;
            uint32_t id = *(uint32_t*)en; uint64_t c = *(uint64_t*)(en + 0x10);
            if (!id) continue;
            int j = 0; while (j < n && g_stockCache[back][j].id != id) j++;     // one entry per item
            uint64_t sum = (j < n ? g_stockCache[back][j].count : 0) + c;
            if (j == n) { if (n == kStockCache) break; g_stockCache[back][n++].id = id; }
            g_stockCache[back][j].count = sum > 0xffffffffull ? 0xffffffffu : (uint32_t)sum;
        }
    }
    g_stockN[back] = n;
    MemoryBarrier();
    g_stockCur = back; g_stockTick = GetTickCount();
}
static int64_t stock_of(uint32_t item) {
    if (GetTickCount() - g_stockTick > 3000) return -1;
    LONG cur = g_stockCur; int n = g_stockN[cur];
    for (int j = 0; j < n; j++) if (g_stockCache[cur][j].id == item) return g_stockCache[cur][j].count;
    return 0;                       // listed types are complete: not there = none
}
static void stock_final_hook(void* stocks, void* a, void* b, void* cc, void* d, void* e) {
    // only while the hammer is out (the lists are rebuilt every frame; longer lists
    // cost the game time even when the picker is closed)
    bool building = !g_cxOrig || GetTickCount() - g_buildTick < 1500;
    if (creative_on() && building && g_stockAdd && g_stocksTls != TLS_OUT_OF_INDEXES)
        add_all_materials((uint8_t*)stocks, (StocksCall*)TlsGetValue(g_stocksTls));
    uint8_t* sb = (uint8_t*)stocks; int st = sb[0x2bf8];
    uint64_t before = st < 7 ? ((StockList*)(sb + st * 0x648))->count : 0;
    g_stockFinalOrig(stocks, a, b, cc, d, e);
    if (!creative_on()) note_stock((uint8_t*)stocks);
    // The game selects the entry of the material chosen last (+0x628 index, +0x630
    // valid). With none of a type in the inventory nothing is chosen, so the picker
    // cannot scroll and the hammer has no material: select the first entry instead.
    if (creative_on() && building && g_stockAdd)
        for (int t = 2; t <= 5; t++) {
            uint8_t* l = (uint8_t*)stocks + t * 0x648;
            if (((StockList*)l)->count && !l[0x630]) { *(uint64_t*)(l + 0x628) = 0; l[0x630] = 1; }
        }
    static DWORD s_diagTick = 0;
    if (creative_on() && st < 7 && GetTickCount() - s_diagTick > 10000) {
        s_diagTick = GetTickCount();
        LOG(.str("creative: picker list ").i(st).str(" sort ").i(sb[st * 0x648 + 0x638]).str(" entries ").u(before)
            .str(" -> ").u(((StockList*)(sb + st * 0x648))->count).str(building ? " (hammer out)" : ""));
    }
}

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
    g_pend.n = 0; g_pend.replace = replace; g_pend.shape = shape;
    g_pend.style = g_replaceStyle == STYLE_CONNECTED ? STYLE_OVERLAP : g_replaceStyle;   // cave mode
    memset(g_allow, 0, sizeof(g_allow));
    if (g_caveAllow) memset(g_caveAllow, 0, 256);
    if (g_caveHits) { g_caveHits[0] = 0; g_caveHits[1] = 0; g_caveHits[2] = 0; }
    for (int k = 0; k < 4; k++) g_pend.rot[k] = tmpl.rot[k];
}
static void pend_add(const int64_t* pos) {
    if (g_pend.n < kMaxPending) { memcpy(g_pendPos[g_pend.n], pos, 24); g_pend.n++; }
}

// Replace: the old-material voxel counts after each shape, as changes only (most shapes
// repaint one or two materials). A cut-down action refunds exactly its own shapes.
struct VolMark { uint32_t shape; uint32_t mat; uint32_t total; };
static const int kVolLog = 1 << 18;
static VolMark g_volLog[kVolLog];
static int g_volLogN = 0; static bool g_volLogFull = false;
static uint32_t g_volPrev[256];
static void vol_log(int shape) {
    for (int m = 1; m < 256; m++) {
        uint32_t v = g_caveVols[m];
        if (v == g_volPrev[m]) continue;
        g_volPrev[m] = v;
        if (g_volLogN < kVolLog) g_volLog[g_volLogN++] = VolMark{ (uint32_t)shape, (uint32_t)m, v };
        else g_volLogFull = true;
    }
}
// old-material voxel counts of shapes 0..k-1
static void vol_counts(int k, uint32_t* cnt) {
    memset(cnt, 0, 256 * 4);
    for (int q = 0; q < g_volLogN && (int)g_volLog[q].shape < k; q++) cnt[g_volLog[q].mat] = g_volLog[q].total;
}

// Prices one shape of the action at `xf` and queues it for the commit.
// With Replace, the dry run runs with the replace blit, so the game measures (and
// prices) exactly what will be written, and the old material of every repainted
// voxel is tallied in the cave's volume table for the refund.
static int g_onlyMaterial = 0;     // connected replace: the only material repainted
static bool price_shape(CostList* list, void* ctx, void* state, void* pdata, CursorXf* xf, bool replace) {
    if (replace) {
        memset(g_remVols, 0, sizeof(g_remVols));
        price_removal(list, ctx, state, xf);                         // what is removable here
        for (int m = 1; m < 0x100; m++) if (g_remVols[m] > 0.f) { g_allow[m] = 1; g_caveAllow[m] = 1; }
        if (g_onlyMaterial)                 // connected replace: repaint only the clicked material
            for (int m = 1; m < 0x100; m++) if (m != g_onlyMaterial) { g_allow[m] = 0; g_caveAllow[m] = 0; }
        *g_caveMode = (uint8_t)g_pend.style;
    }
    if (g_pend.n < kMaxPending) g_pendCost[g_pend.n] = (uint32_t)list->count;
    g_pend.list = list;
    uint8_t r = g_placeOrig(list, ctx, state, pdata, xf);          // dry run
    if (replace) *g_caveMode = 0;
    pend_add(xf->pos);
    if (replace && g_pend.n <= kVolSnaps) memcpy(g_volSnap[g_pend.n - 1], g_caveVols, 256 * 4);
    if (replace) vol_log(g_pend.n - 1);
    return r != 0;
}

static void refund_reset() { memset(g_caveVols, 0, 256 * 4); memset(g_volPrev, 0, sizeof(g_volPrev)); g_volLogN = 0; g_volLogFull = false; }

// The game's refund builder: appends refund entries for per-material volumes to `list`
// (building: 1.0 per voxel; terrain: density / 255, as the game's removal measures it).
// `cnt` holds the cave's tally: building voxels, terrain density sums.
// Returns the total volume (0 = nothing built).
static float refund_entries(CostList* list, void* ctx, const uint32_t* cnt) {
    alignas(16) float vols[256]; float total = 0.f;
    vols[0] = 0.f;
    for (int m = 1; m < 256; m++) { vols[m] = m < 0x80 ? (float)cnt[m] / 255.f : (float)cnt[m]; total += vols[m]; }
    if (total <= 0.f || !g_refundBuildOrig) return 0.f;
    VolList vl = { vols, 0x100 };
    void** lootSlot = (void**)((uint8_t*)ctx + 0x40);
    void* saved = *lootSlot; if (!saved) *lootSlot = g_lootMemory;
    g_refundBuildOrig(list, ctx, &vl);
    *lootSlot = saved;
    return total;
}
// refund entries for given voxel counts, into `out`
static int build_refund_entries(void* ctx, const uint32_t* cnt, CostEntry* out, int max) {
    static CostEntry buf[256];
    CostList l = { buf, 0, 256, nullptr };
    refund_entries(&l, ctx, cnt);
    int n = (int)(l.count < (uint64_t)max ? l.count : (uint64_t)max);
    memcpy(out, buf, (size_t)n * sizeof(CostEntry));
    return n;
}

// Turns the tallied old-material volumes into refund entries and moves them to
// g_refund, paid back once the action is paid.
static void refund_build(CostList* list, void* ctx) {
    uint64_t before = list->count;
    float total = refund_entries(list, ctx, g_caveVols);
    if (total <= 0.f) return;
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

// ---- partial actions
static volatile int g_partialOf = 0;          // the action was cut down from this many shapes
// How much of an item the player can build with: the material picker's count (backpack and
// magic chests, as the charge uses them), noted every frame by stock_final_hook.
// -1 = not known (no picker list with that item in the last 3 seconds).
static int64_t stock_of(uint32_t item);
static bool affordable(uint8_t*, const CostList* list, uint32_t end) {
    struct Need { uint32_t item; uint64_t count; } need[32]; int nn = 0;
    const CostEntry* e = (const CostEntry*)list->data;
    for (uint32_t q = 0; q < end; q++) {
        uint32_t item = *(const uint32_t*)(e[q].b + 0x10), count = *(const uint32_t*)(e[q].b + 0x14);
        int k = 0; while (k < nn && need[k].item != item) k++;
        if (k == nn) { if (nn == 32) return false; need[nn++] = { item, 0 }; }
        need[k].count += count;
    }
    for (int k = 0; k < nn; k++) {
        int64_t have = stock_of(need[k].item);
        if (have < 0 || (uint64_t)have < need[k].count) return false;
    }
    return true;
}
static bool stock_known(const CostList* list) {
    const CostEntry* e = (const CostEntry*)list->data;
    for (uint64_t q = 0; q < list->count; q++) if (stock_of(*(const uint32_t*)(e[q].b + 0x10)) < 0) return false;
    return true;
}
// Cuts the queued action down to the most shapes the inventory can pay for. Returns true
// when the list was changed. With nothing affordable (or a Replace whose refunds cannot be
// split) the list is left as is and the charge fails as usual.
static bool shrink_to_affordable(uint8_t* trans, CostList* list) {
    int n = g_pend.n;
    if (!list->data || n < 2 || g_pend.list != list || list->count != g_pendCost[n]) return false;
    if (!stock_known(list)) { LOG(.str("partial action: material count not known (open the material picker once)")); return false; }
    if (affordable(trans, list, (uint32_t)list->count)) return false;
    int lo = 0, hi = n - 1;                       // largest k with shapes 0..k-1 affordable
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (affordable(trans, list, g_pendCost[mid])) lo = mid; else hi = mid - 1;
    }
    int k = lo;
    LOG(.str("partial action: ").i(k).str(" of ").i(n).str(" shapes affordable"));
    if (k == 0 || (g_pend.replace && g_volLogFull)) return false;
    list->count = g_pendCost[k];
    g_pend.n = k;
    if (g_pend.replace && g_refundList == list) {
        static uint32_t cnt[256];
        vol_counts(k, cnt);
        g_refundN = build_refund_entries(g_pend.ctx, cnt, g_refund, kMaxRefund);
        g_refundDropped = 0;
    }
    g_partialOf = n;
    return true;
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
    if (pd.replace) *g_caveMode = (uint8_t)pd.style;
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
    *g_caveMode = 0;
    if (pd.replace && us.step0 && !us.lost && us.recorded > 0 && GetTickCount() - g_paidTick < 3000 && g_paidN > 0) {
        CostEntry e[kMaxPaid]; int ne = 0;
        if (us.recorded >= pd.n) { ne = g_paidN; memcpy(e, g_paid, (size_t)ne * sizeof(CostEntry)); }
        else if (us.recorded <= kVolSnaps) ne = build_refund_entries(ctx, g_volSnap[us.recorded - 1], e, kMaxPaid);
        undo_refund_add((CostList*)(us.step0 + 0x110), e, ne);
        LOG(.str("  undo will take back ").i(ne).str(" refund entries (").i(us.recorded).str(" of ").i(pd.n).str(" shapes)"));
        g_paidN = 0;
    }
    if (pd.replace) LOG(.str("  commit blit: voxels ").u(g_caveHits[0] - h0).str(" solid ").u(g_caveHits[1] - h1));
    LOG(.str("commit: ").i(pd.n).str(" shapes, written ").i(placed).str(pd.replace ? (pd.style == STYLE_OVERLAP ? " (replace overlap / connected)" : " (replace full)") : ""));
    int of = g_partialOf; g_partialOf = 0;
    if (pd.n > 1 || of) { Buf b;
        if (of > pd.n) b.fmt(T(S_PLACED_PARTIAL), num(placed).s, num(of).s); else b.fmt(T(S_PLACED), num(placed).s);
        if (us.recorded < pd.n) b.str("  |  ").fmt(T(S_UNDO_COVERS), num(us.recorded).s);
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
        Buf b; b.fmt(T(S_TOO_BIG), num(pl.total).s, num(g_cfg.maxShapes).s);
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
    g_pend.tick = GetTickCount();
    g_actList = list; g_actShapes = g_pend.n;
    return g_pend.n > 0 ? 1 : 0;
}

// Roof materials spread diagonally. The game's per-material config
// (BuildingTerraformingConfig, found by e75970(registry, id); building material m has
// id m - 0x80) has isRoofBlock at +0x6c; the removal table builder 48f070 reads it the same way.
typedef uint8_t* (*MatConfigFn)(void* registry, uint8_t id);
static MatConfigFn g_matConfig = nullptr;
static bool is_roof(void* ctx, int m) {
    if (!g_matConfig || !ctx || m < 0x80) return false;
    void* reg = *(void**)((uint8_t*)ctx + 0xf8);
    uint8_t* cfg = reg ? g_matConfig(reg, (uint8_t)(m - 0x80)) : nullptr;
    return cfg && cfg[0x6c] != 0;
}

// ---- connected cells (fill bucket)
// Breadth-first walk over a grid of shape-sized cells from cell (0,0,0) along the six
// world axes (with `diag`, also the 20 diagonal neighbours: roofs climb diagonally);
// a cell joins when `probe(i, j, k)` says it holds the clicked material.
// Found cells go to g_floodQ (the start first); returns their number, at most `limit`.
static int32_t g_floodQ[kMaxPending][3];
template <class Probe>
static int flood_cells(int limit, bool diag, Probe probe, bool* capped) {
    // Every found cell probes up to 26 neighbours, so up to 26 * FillLimit + 1 cells are
    // visited: the table holds 2^20 (FillLimit is at most 20000), always under half full.
    static const uint32_t kSeen = 1u << 20;
    static uint64_t seen[kSeen];                        // open addressing, 0 = empty
    memset(seen, 0, sizeof(seen));
    auto key = [](int i, int j, int k) -> uint64_t {
        return ((uint64_t)(uint32_t)(i + 0x100000) << 42) | ((uint64_t)(uint32_t)(j + 0x100000) << 21) | (uint64_t)(uint32_t)(k + 0x100000) | (1ull << 63);
    };
    auto visit = [&](int i, int j, int k) -> bool {     // false if seen before (or table full)
        uint64_t kk = key(i, j, k); uint32_t h = (uint32_t)((kk * 0x9E3779B97F4A7C15ull) >> 44);
        for (uint32_t n = 0; n < kSeen; n++, h = (h + 1) & (kSeen - 1)) {
            if (seen[h] == kk) return false;
            if (!seen[h]) { seen[h] = kk; return true; }
        }
        return false;
    };
    int head = 0, tail = 0;
    *capped = false;
    visit(0, 0, 0); g_floodQ[tail][0] = 0; g_floodQ[tail][1] = 0; g_floodQ[tail][2] = 0; tail++;
    // the six faces first, then edges and corners
    static int dirs[26][3]; static int nd = 0;
    if (!nd) {
        for (int pass = 1; pass <= 3; pass++)
            for (int x = -1; x <= 1; x++) for (int y = -1; y <= 1; y++) for (int z = -1; z <= 1; z++)
                if ((x != 0) + (y != 0) + (z != 0) == pass) { dirs[nd][0] = x; dirs[nd][1] = y; dirs[nd][2] = z; nd++; }
    }
    const int ndirs = diag ? 26 : 6;
    while (head < tail) {
        const int32_t* c = g_floodQ[head++];
        for (int q = 0; q < ndirs; q++) {
            const int* d = dirs[q];
            int i = c[0] + d[0], j = c[1] + d[1], k = c[2] + d[2];
            if (!visit(i, j, k)) continue;
            if (tail >= limit || tail >= kMaxPending) { *capped = true; continue; }
            if (probe(i, j, k)) { g_floodQ[tail][0] = i; g_floodQ[tail][1] = j; g_floodQ[tail][2] = k; tail++; }
        }
    }
    return tail;
}

// ---- connected replace (fill bucket, server)
// Starting at the aimed shape, walks the grid of shape-sized cells (6 neighbours,
// world axes) through every cell that contains the clicked material, up to
// FillLimit cells. Each cell is probed with a removal dry run, which reports the
// materials it would remove there. The found cells are then replaced like an area
// in overlap style, with only the clicked material allowed, so nothing else is
// touched and the shape's own form does not matter.
static uint8_t connected_fill(CostList* list, void* ctx, void* state, void* pdata, const CursorXf& start) {
    Aabb box; shape_bounds(state, &start, &box);
    int64_t step[3];
    for (int a = 0; a < 3; a++) {          // shape size, snapped to 1/16 m against float noise
        double sz = (double)(box.mx[a] - box.mn[a]);
        step[a] = (int64_t)((double)(int64_t)(sz * 16.0 + 0.5) * (core::kFix / 16.0));
    }
    if (step[0] <= 0 || step[1] <= 0 || step[2] <= 0) return 0;
    // The clicked material: the material (building or terrain) with the most volume in the first cell,
    // leaving out the material being placed. The cell is shape-sized, so with a big shape
    // it often also holds blocks already repainted; those must not win the vote.
    CursorXf xf = start;
    memset(g_remVols, 0, sizeof(g_remVols));
    price_removal(list, ctx, state, &xf);
    const int mine = pdata ? ((uint8_t*)pdata)[0x454] : 0;           // the material being placed
    int target = 0; float best = 0.f;
    for (int m = 1; m < 0x100; m++) if (m != mine && g_remVols[m] > best) { best = g_remVols[m]; target = m; }
    LOG(.str("connected replace: clicked material ").x(target).str(" volume ").f(best).str(", new material ").x(mine)
        .str(" volume ").f(mine > 0 ? g_remVols[mine] : 0.f));
    if (!target) {
        bool own = mine > 0 && g_remVols[mine] > 0.f;
        flash(T(own ? S_CREPLACE_OWN : S_CREPLACE_NOTHING));
        beep(MB_ICONHAND); return 0;
    }
    const int limit = g_cfg.fillLimit;
    const bool diag = is_roof(ctx, target);
    int probes = 0; bool capped = false;
    const int tail = flood_cells(limit, diag, [&](int i, int j, int k) {
        xf.pos[0] = start.pos[0] + i * step[0]; xf.pos[1] = start.pos[1] + j * step[1]; xf.pos[2] = start.pos[2] + k * step[2];
        memset(g_remVols, 0, sizeof(g_remVols));
        price_removal(list, ctx, state, &xf); probes++;
        return g_remVols[target] > 0.f;
    }, &capped);
    LOG(.str("connected replace: ").i(tail).str(" cells, ").i(probes).str(" probes").str(diag ? " (roof, diagonal)" : "").str(capped ? ", stopped at FillLimit" : ""));
    // price and queue every found cell (overlap style, only the clicked material)
    pend_begin(ctx, start, true, state ? *(uint32_t*)state : 0);
    g_pend.style = STYLE_OVERLAP;
    refund_reset();
    g_onlyMaterial = target;
    long priced = 0;
    for (int n = 0; n < tail; n++) {
        const int32_t* c = g_floodQ[n];
        xf.pos[0] = start.pos[0] + c[0] * step[0]; xf.pos[1] = start.pos[1] + c[1] * step[1]; xf.pos[2] = start.pos[2] + c[2] * step[2];
        if (price_shape(list, ctx, state, pdata, &xf, true)) priced++;
    }
    g_onlyMaterial = 0;
    g_pendCost[g_pend.n] = (uint32_t)list->count;
    if (priced) refund_build(list, ctx);
    LOG(.str("connected replace: priced ").i(priced).str(" of ").i(tail).str(" cost entries=").u(list->count)
        .str(" refund entries=").i(g_refundN));
    if (capped) { Buf b; b.fmt(T(S_CREPLACE_CAPPED), num(limit).s); flash(b.z()); }
    if (!priced) { g_pend.n = 0; flash(T(S_NOTHING_TO_REPLACE)); return 0; }
    g_pend.tick = GetTickCount();       // pricing thousands of shapes takes a while
    g_actList = list; g_actShapes = g_pend.n;
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

// Client cursor components seen by the blueprint system's per-frame cursor read
// (cursor_get_hook). The client skips PlaceShape while its ghost is blocked, which is
// common when flying (Airborne Architect), so without these the first clicks of a
// session had no client cursor at all. Single player has one or two client worlds.
static uint8_t* volatile g_frameCC[4] = {};

static volatile LONG g_frameCCNext = 0;
static uint8_t* live_cc() { uint8_t* c = g_clientCC; return c ? c : g_frameCC[(g_frameCCNext - 1) & 3]; }

static int aim_candidates(uint8_t* out[5]) {
    int n = 0;
    auto add = [&](uint8_t* c) {
        if (!c || !readable(c + 0x300, 0x70)) return;
        for (int k = 0; k < n; k++) if (out[k] == c) return;
        out[n++] = c;
    };
    add(g_clientCC);
    for (int k = 0; k < 4; k++) add(g_frameCC[k]);
    return n;
}

static bool aim_lookup(const CursorXf* place, uint32_t shape, CursorXf* out, const char** how) {
    uint8_t* cand[5]; const int nc = aim_candidates(cand);
    for (int k = 0; k < nc; k++) {
        uint8_t* c = cand[k];
        if (!memcmp(c + 0x300, place->pos, 0x18) && !zero_pos(*(CursorXf*)(c + 0x338))) {
            *out = *(CursorXf*)(c + 0x338); *how = "client cursor"; return true;
        }
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
    for (int n = 0; n < nc; n++) {
        uint8_t* c = cand[n];
        if (zero_pos(*(CursorXf*)(c + 0x338))) continue;
        const CursorXf* a = (const CursorXf*)(c + 0x338);
        int64_t lim = (int64_t)6 << 32;                       // 6 m
        bool close = true;
        for (int k = 0; k < 3; k++) { int64_t d = a->pos[k] - place->pos[k]; if (d < -lim || d > lim) close = false; }
        if (close) { *out = *a; *how = "client cursor (near)"; return true; }
    }
    for (int n = 0; n < nc; n++) {
        const CursorXf* cp = (const CursorXf*)(cand[n] + 0x300); const CursorXf* ca = (const CursorXf*)(cand[n] + 0x338);
        LOG(.str("  aim lookup failed: client place (").fix(cp->pos[0]).c(',').fix(cp->pos[1]).c(',').fix(cp->pos[2])
            .str(") client aim (").fix(ca->pos[0]).c(',').fix(ca->pos[1]).c(',').fix(ca->pos[2]).c(')'));
    }
    if (!nc) LOG(.str("  aim lookup failed: no client cursor known yet"));
    return false;
}

// The blueprint system reads the cursor transform with 218720(out, player, cursor
// component) for every hammer-holding player each frame, before it looks at the
// click events, so this also runs while the ghost is blocked. out = caller rbp+0xf8;
// rbp+0x50 is the undo storage, which only the server world has.
typedef uint8_t* (*CursorGetFn)(uint8_t* out, uint8_t* player, uint8_t* cc);
static CursorGetFn g_cursorGetOrig = nullptr;
static uint8_t* cursor_get_hook(uint8_t* out, uint8_t* player, uint8_t* cc) {
    uint8_t* r = g_cursorGetOrig(out, player, cc);
    uint8_t* frame = out - 0xf8;
    if (!cc || !readable(frame + 0x50, 8) || *(void**)(frame + 0x50)) return r;     // server world
    for (int k = 0; k < 4; k++) if (g_frameCC[k] == cc) return r;
    LONG k = InterlockedIncrement(&g_frameCCNext) - 1;      // a new world (load) replaces the oldest
    g_frameCC[k & 3] = cc;
    LOG(.str("client cursor ").i(k + 1).str(" found"));
    return r;
}

// ---- area removal (right click in Area mode)
// The dismantle system dry-runs RemoveShape at the aimed shape, pays the refund
// list and commits with RemoveCommit, all in its own context. In Area mode the
// dry run becomes a corner click (and the commit is suppressed) or, on the third
// corner, dry runs for every shape of the area plus one RemoveCommit per shape.
static CommitFn g_removeCommitOrig = nullptr;     // trampoline
struct RemovePending { void* ctx; DWORD tid, tick; int n; int64_t click[3]; bool suppress; CostList* list; int only; };
static uint32_t g_rpendCost[kMaxPending + 1];
static RemovePending g_rpend;
static int64_t g_rpendPos[kMaxPending][3];

static bool is_server_ctx(void* ctx) { return ctx && *(void**)((uint8_t*)ctx + 0xf0) != nullptr; }

// ---- connected removal (fill bucket, right click)
// Removal builds a 256-entry "may remove" table per material (48f1c0) in both the dry
// run (RemoveShape) and the real removal (RemoveCommit); the removal blit leaves every
// voxel whose material is 0 there. Both calls are redirected: while the mod removes
// connected cells on this thread, every material but the clicked one is cleared from
// the table, so other materials in the same cells stay.
typedef void* (*RemoveTableFn)(uint8_t* out, void* ctx, void* state);
static RemoveTableFn g_removeTableOrig = nullptr;
static bool g_removeTableOk = false;          // both calls redirected
static volatile LONG g_onlyRemove = 0;
static volatile DWORD g_onlyRemoveTid = 0;
static void* remove_table_hook(uint8_t* out, void* ctx, void* state) {
    void* r = g_removeTableOrig(out, ctx, state);
    const int only = g_onlyRemove;
    if (only && out && g_onlyRemoveTid == GetCurrentThreadId())
        for (int m = 1; m < 0x100; m++) if (m != only) out[m] = 0;
    return r;
}
static void only_remove(int material) { g_onlyRemoveTid = material ? GetCurrentThreadId() : 0; g_onlyRemove = material; }

// Starting at the aimed shape, walks the shape-sized cells holding the clicked material
// (the material with the most removable volume there, building or terrain, by the
// game's own rules for the selected material) and queues a removal of only that
// material in every found cell. The dismantle system then pays the refunds and
// remove_commit_hook removes the cells.
static uint8_t connected_remove(CostList* list, void* ctx, void* state, Aabb* bx, IntXf* xf, void* filter, uint8_t check) {
    int64_t step[3];
    for (int a = 0; a < 3; a++) {          // shape size, snapped to 1/16 m against float noise
        double sz = (double)(bx->mx[a] - bx->mn[a]);
        step[a] = (int64_t)((double)(int64_t)(sz * 16.0 + 0.5) * (core::kFix / 16.0));
    }
    g_rpend.ctx = ctx; g_rpend.tid = GetCurrentThreadId(); g_rpend.tick = GetTickCount(); g_rpend.n = 0;
    g_rpend.suppress = true; g_rpend.only = 0;
    if (!g_removeTableOk || step[0] <= 0 || step[1] <= 0 || step[2] <= 0) {
        flash(T(S_CDELETE_UNAVAILABLE)); beep(MB_ICONHAND); return 0;
    }
    auto cell = [&](int i, int j, int k, IntXf* ix, Aabb* b2) {
        *ix = *xf; *b2 = *bx;
        const int64_t d[3] = { i * step[0], j * step[1], k * step[2] };
        for (int a = 0; a < 3; a++) {
            ix->pos[a] += d[a];
            float df = (float)d[a] * kInv; b2->mn[a] += df; b2->mx[a] += df;
        }
    };
    // dry run with refunds noted, not added (refund_build_hook)
    auto probe = [&](int i, int j, int k) {
        IntXf ix; Aabb b2; cell(i, j, k, &ix, &b2);
        memset(g_remVols, 0, sizeof(g_remVols));
        g_ownRemoveTid = GetCurrentThreadId();
        g_remove(list, ctx, state, &b2, &ix, filter, check);
        g_ownRemoveTid = 0;
    };
    probe(0, 0, 0);
    int target = 0; float best = 0.f;
    for (int m = 1; m < 0x100; m++) if (g_remVols[m] > best) { best = g_remVols[m]; target = m; }
    LOG(.str("connected delete: clicked material ").x(target).str(" volume ").f(best));
    if (!target) { flash(T(S_CDELETE_NOTHING)); beep(MB_ICONHAND); return 0; }
    only_remove(target);
    const int limit = g_cfg.fillLimit;
    const bool diag = is_roof(ctx, target);
    int probes = 0; bool capped = false;
    const int tail = flood_cells(limit, diag, [&](int i, int j, int k) {
        probe(i, j, k); probes++;
        return g_remVols[target] > 0.f;
    }, &capped);
    LOG(.str("connected delete: ").i(tail).str(" cells, ").i(probes).str(" probes").str(diag ? " (roof, diagonal)" : "").str(capped ? ", stopped at FillLimit" : ""));
    // the real dry runs: refund entries for the clicked material go to the game's list
    uint8_t any = 0;
    for (int n = 0; n < tail; n++) {
        IntXf ix; Aabb b2; cell(g_floodQ[n][0], g_floodQ[n][1], g_floodQ[n][2], &ix, &b2);
        g_rpendCost[g_rpend.n] = (uint32_t)list->count;
        if (g_remove(list, ctx, state, &b2, &ix, filter, check)) any = 1;
        memcpy(g_rpendPos[g_rpend.n], ix.pos, 24); g_rpend.n++;
    }
    only_remove(0);
    g_rpendCost[g_rpend.n] = (uint32_t)list->count;
    g_rpend.list = list;
    memcpy(g_rpend.click, xf->pos, sizeof(g_rpend.click));
    g_rpend.suppress = !any;
    g_rpend.only = target;
    g_rpend.tick = GetTickCount();      // probing thousands of cells takes a while
    LOG(.str("connected delete: priced ").i(g_rpend.n).str(" cells, refund entries ").u(list->count));
    if (capped) { Buf b; b.fmt(T(S_CDELETE_CAPPED), num(limit).s); flash(b.z()); }
    return any;
}

static uint8_t remove_hook(void* list, void* ctx, void* state, void* box, void* xfp, void* filter, uint8_t check) {
    piece_target_restore();
    if (g_ownRemoveTid == GetCurrentThreadId() || !is_server_ctx(ctx) || !box || !xfp)
        return g_remove((CostList*)list, ctx, state, (Aabb*)box, (IntXf*)xfp, filter, check);
    const bool connected = g_mode == MODE_PREFAB && g_replace && g_replaceStyle == STYLE_CONNECTED;
    if (filter && (connected || g_mode == MODE_AREA)) {
        // A building piece (foundation, stairs...) dismantled by entity: the dry run gets
        // the entity as filter and the commit is 488670, never RemoveCommit. Normally
        // bt_piece_check sends pieces down the block path; without it the piece goes
        // on its own.
        LOG(.str("remove click on a building piece: removed on its own (check ").str(g_pieceCheckOrig ? "on" : "off").c(')'));
        if (!g_pieceCheckOrig) flash(T(S_REMOVE_PIECE));
        return g_remove((CostList*)list, ctx, state, (Aabb*)box, (IntXf*)xfp, filter, check);
    }
    if (connected && list && !small_box((Aabb*)box)) {
        LOG(.str("connected delete: big shape, removed as one shape"));
        return g_remove((CostList*)list, ctx, state, (Aabb*)box, (IntXf*)xfp, filter, check);
    }
    if (connected && list) {
        EnterCriticalSection(&g_stateLock);
        uint8_t r = connected_remove((CostList*)list, ctx, state, (Aabb*)box, (IntXf*)xfp, filter, check);
        LeaveCriticalSection(&g_stateLock);
        if (r) beep(MB_ICONASTERISK);
        return r;
    }
    if (g_mode != MODE_AREA)
        return g_remove((CostList*)list, ctx, state, (Aabb*)box, (IntXf*)xfp, filter, check);
    IntXf* xf = (IntXf*)xfp;
    Aabb* bx = (Aabb*)box;
    EnterCriticalSection(&g_stateLock);
    if (InterlockedExchange(&g_cancelReq, 0)) reset_corners();
    if (g_scOp != OP_REMOVE) { reset_corners(); g_scOp = OP_REMOVE; }
    g_rpend.ctx = ctx; g_rpend.tid = GetCurrentThreadId(); g_rpend.tick = GetTickCount(); g_rpend.n = 0; g_rpend.only = 0;
    LONG n = g_sc;
    if (n < 0 || n > g_cfg.corners - 1) n = 0;
    g_scPos[n] = I3{ xf->pos[0], xf->pos[1], xf->pos[2] };      // removal transforms are shape centres
    LOG(.str("remove click: corner ").i(n + 1).str(" at (").fix(xf->pos[0]).c(',').fix(xf->pos[1]).c(',').fix(xf->pos[2]).c(')'));
    if (n == 0) {
        for (int a = 0; a < 3; a++) g_scStep[a] = bx->mx[a] - bx->mn[a];
        g_scCursorOff = 0x338;
        uint8_t* cc = live_cc();
        if (cc && readable(cc + 0x338, 0x18)) for (int a = 0; a < 3; a++) g_scOff[a] = xf->pos[a] - ((int64_t*)(cc + 0x338))[a];
        else for (int a = 0; a < 3; a++) g_scOff[a] = 0;
    }
    if (n < g_cfg.corners - 1) {
        g_sc = n + 1; g_corners = g_sc;
        g_rpend.suppress = true;                       // the dismantle would still commit
        Buf b; b.fmt(T(S_REMOVE_CORNER), num(n + 1).s, num(g_cfg.corners).s);
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
        Buf b; b.fmt(T(S_TOO_BIG), num(pl.total).s, num(g_cfg.maxShapes).s);
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
    g_rpend.tick = GetTickCount();
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
    only_remove(rp.only);                   // connected delete: only the clicked material
    for (int i = 0; i < rp.n; i++) {
        undo_before(us, i);
        Commit rc = *c;
        for (int a = 0; a < 3; a++) rc.pos[a] += (float)(g_rpendPos[i][a] - rp.click[a]) * kInv;
        if (g_removeCommitOrig(ctx, &rc, shape)) { any = 1; done++; }
        undo_after(us);
    }
    only_remove(0);
    undo_end(us);
    LOG(.str("remove commit: ").i(rp.n).str(" shapes, removed ").i(done).str(rp.only ? " (connected delete)" : ""));
    Buf b; b.fmt(T(S_REMOVED), num(done).s);
    if (us.recorded < rp.n) b.str("  |  ").fmt(T(S_UNDO_COVERS), num(us.recorded).s);
    flash(b.z());
    return any;
}

static uint8_t place_hook(CostList* list, void* ctx, void* state, void* pdata, CursorXf* xf) {
    const bool replace = g_replace != 0 && item_kind(state) != ITEM_PIECE;     // pieces place as in the game
    if (g_replace) log_item("click", state);
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
            flash(T(S_REPLACE_READ_FAIL)); beep(MB_ICONHAND);
            LeaveCriticalSection(&g_stateLock);
            return 0;
        }
        target = aim;
    }

    uint8_t r = 0;
    const bool bigShape = mode == MODE_PREFAB && g_replaceStyle == STYLE_CONNECTED && !small_item(state);
    if (mode == MODE_PREFAB && g_replaceStyle == STYLE_CONNECTED && !bigShape) {
        r = connected_fill(list, ctx, state, pdata, target);
        LeaveCriticalSection(&g_stateLock);
        if (r) beep(MB_ICONASTERISK);
        return r;
    }
    if (mode == MODE_PREFAB) {
        pend_begin(ctx, target, true, shape);
        if (bigShape) { g_pend.style = STYLE_FULL; LOG(.str("  big shape with the fill bucket: full-shape replace")); }
        refund_reset();
        r = price_shape(list, ctx, state, pdata, &target, true) ? 1 : 0;
        { int na = 0; for (int m = 1; m < 0x100; m++) na += g_allow[m];
          LOG(.str("  pricing blit: voxels ").u(g_caveHits[0]).str(" solid ").u(g_caveHits[1]).str(" terrain ").u(g_caveHits[2])
              .str(" removable volume ").f(g_removedVolume).str(" materials allowed ").i(na)); }
        if (r) refund_build(list, ctx);
        if (!r) { flash(T(S_NOTHING_TO_REPLACE)); g_pend.n = 0; }
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
        Buf b; b.fmt(T(replace ? S_REPLACE_CORNER : S_BUILD_CORNER), num(n + 1).s, num(g_cfg.corners).s);
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
static char g_overlayText[1024];
static volatile LONG g_fontPx = 24;      // panel font height, scaled to the window
// The panel font, recreated only when its size changes (overlay thread only).
static HFONT panel_font(int px) {
    static HFONT font = nullptr; static int fontPx = 0;
    if (!font || fontPx != px) {
        if (font) DeleteObject(font);
        font = CreateFontW(px, 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        fontPx = px;
    }
    return font;
}

// The panel texts are UTF-8 (translations): drawn as UTF-16, Windows font linking
// supplies the CJK and Thai glyphs Segoe UI lacks.
static void draw_text(HDC dc, const char* t, RECT* r, UINT flags) {
    static wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 1024)) w[0] = 0;
    DrawTextW(dc, w, -1, r, flags);
}

static HWND find_game_window() {
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
// The game's main window, looked up again only when it is gone or every 2 s
// (enumerating every window on the desktop 20 times a second is wasteful).
static HWND game_window() {
    static HWND w = nullptr; static DWORD at = 0;
    DWORD now = GetTickCount();
    if (!w || !IsWindow(w) || now - at > 2000) { w = find_game_window(); at = now; }
    return w;
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
        HGDIOBJ of = SelectObject(dc, panel_font(px));
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(232, 226, 210));
        RECT tr = { r.left + px / 2, r.top + px / 4, r.right - px / 2, r.bottom - px / 4 };
        draw_text(dc, g_overlayText, &tr, DT_CENTER | DT_TOP | DT_NOPREFIX);
        SelectObject(dc, of);
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
    uint8_t* cc = live_cc();
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
    if (!ok || n > g_cfg.maxShapes) { b.str("  |  ").fmt(T(S_BAR_TOO_BIG), num(g_cfg.maxShapes).s); return; }
    b.str("  |  ").fmt(T(S_BAR_SIZE), num(pl.count[0]).s, num(pl.count[2]).s, num(pl.count[1]).s, num(n).s);
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
    int n = 0;
    auto put = [&](int64_t x, int64_t y, int64_t z) {
        if (n >= cap) return;
        out[n][0] = (uint32_t)x; out[n][1] = (uint32_t)y; out[n][2] = (uint32_t)z; n++;
    };
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

static volatile DWORD g_floatTick = 0;              // last time the floating info text was drawn
static volatile DWORD g_hudRowsTick = 0;        // last time the HUD rows were drawn (the info bar then hides its own)
// A key setting: virtual-key code in the low byte, plus modifiers that must be held.
enum { KEY_SHIFT = 0x1000, KEY_CTRL = 0x2000, KEY_ALT = 0x4000, KEY_MODS = 0x7000 };
static int held_mods() {
    return ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? KEY_SHIFT : 0) | ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? KEY_CTRL : 0) |
           ((GetAsyncKeyState(VK_MENU) & 0x8000) ? KEY_ALT : 0);
}
static int* const g_allKeys[] = { &g_cfg.keyStructure, &g_cfg.keyReplace, &g_cfg.keyCancel, &g_cfg.keyStyle, &g_cfg.keyHover,
                                  &g_cfg.keyFarther, &g_cfg.keyCloser, &g_cfg.wheelKey, &g_cfg.keyFill, &g_cfg.keyMenu, &g_cfg.keyOff };
// Down with exactly its modifiers. A key without modifiers ignores held modifiers (Shift
// while sprinting), unless another setting uses the same key with the held ones.
static bool key_down(int code) {
    int vk = code & 0xff;
    if (code <= 0 || !vk || !(GetAsyncKeyState(vk) & 0x8000)) return false;
    int mods = held_mods();
    if (code & KEY_MODS) return (code & KEY_MODS) == mods;
    if (!mods) return true;
    for (int* k : g_allKeys) if ((*k & 0xff) == vk && (*k & KEY_MODS) == mods) return false;
    return true;
}
static void vk_name(int code, char* out) {
    Buf b;
    if (code & KEY_CTRL) b.str(T(S_KEY_CTRL)).c('+');
    if (code & KEY_SHIFT) b.str(T(S_KEY_SHIFT)).c('+');
    if (code & KEY_ALT) b.str(T(S_KEY_ALT)).c('+');
    int vk = code & 0xff;
    char n[64];
    wchar_t w[32];
    if (vk >= VK_F1 && vk <= VK_F24) { Buf f; f.c('F').i(vk - VK_F1 + 1); lstrcpynA(n, f.z(), 24); }
    else {
        UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
        if (vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE ||
            (vk >= VK_LEFT && vk <= VK_DOWN)) sc |= 0x100;          // extended keys (not the numpad ones)
        // the name in the Windows language, as UTF-8 like every other text
        if (!sc || !GetKeyNameTextW((LONG)(sc << 16), w, 32) || !WideCharToMultiByte(CP_UTF8, 0, w, -1, n, sizeof n, nullptr, nullptr)) {
            Buf f; f.fmt(T(S_KEY_NUMBER), num(vk).s); copy_text(n, f.z(), sizeof n);
        }
    }
    b.str(n);
    copy_text(out, b.z(), 32);
}

// Info panel text, one item per line (shown bottom centre).
static void compose_overlay(char* out, int cap) {
    Buf b;
    bool vanilla = g_mode == MODE_PREFAB && !g_replace && !g_hover;
    bool flashing = GetTickCount() < g_flashUntil;
    bool hudRows = GetTickCount() - g_hudRowsTick < 400;     // the game's building box shows the state and keys
    if (GetTickCount() - g_floatTick < 400) { out[0] = 0; return; }   // the floating text shows them
    if (hudRows && !flashing && g_mode == MODE_AREA && g_corners > 0) {   // the bar keeps the area size
        b.fmt(T(S_BAR_AREA), num(g_corners).s, num(g_cfg.corners).s);
        live_size(b);
        copy_text(out, b.z(), cap); return;
    }
    if (vanilla || hudRows) { if (flashing) b.str(g_flash); copy_text(out, b.z(), cap); return; }
    // line 1: state (or the latest message)
    if (flashing) b.str(g_flash);
    else {
        if (g_mode == MODE_AREA) {
            b.fmt(T(g_scOp == OP_REMOVE ? S_BAR_AREA_REMOVE : g_replace ? S_BAR_AREA_REPLACE : S_BAR_AREA_BUILD),
                  num(g_corners).s, num(g_cfg.corners).s);
            if (g_fill % 3) b.str("  |  ").str(T(g_fill % 3 == 1 ? S_BAR_HOLLOW : S_BAR_WALLS));
            live_size(b);
        } else b.str(T(g_replace ? S_BAR_PREFAB_REPLACE : S_BAR_PREFAB));
        if (g_replace) b.str("  |  ").str(T(g_replaceStyle == STYLE_FULL ? S_BAR_FULL : g_replaceStyle == STYLE_CONNECTED ? S_BAR_CONNECTED : S_BAR_OVERLAP));
        if (g_hover) b.str("  |  ").fmt(T(S_BAR_FREE), tenths((int)(g_hoverDist * 10.f + 0.5f)).s);
        if (creative_on()) b.str("  |  ").str(T(S_BAR_CREATIVE));
    }
    // line 2: keys (named again after a rebinding in the menu)
    if (g_cfg.panelKeys) {
        static char k[8][32]; static LONG named = -1;
        if (named != g_keysVersion) {
            vk_name(g_cfg.keyStructure, k[0]); vk_name(g_cfg.keyReplace, k[1]); vk_name(g_cfg.keyCancel, k[2]);
            vk_name(g_cfg.keyStyle, k[3]); vk_name(g_cfg.keyHover, k[4]); vk_name(g_cfg.keyFill, k[5]);
            if (g_cfg.keyOff > 0) vk_name(g_cfg.keyOff, k[6]);
            if (g_cfg.wheelKey > 0) vk_name(g_cfg.wheelKey, k[7]);
            named = g_keysVersion;
        }
        b.str("\n").str(k[0]).c(' ').str(T(S_BAR_KEY_AREA)).str("  ").str(k[1]).c(' ').str(T(S_BAR_KEY_REPLACE))
         .str("  ").str(k[2]).c(' ').str(T(S_BAR_KEY_CLEAR)).str("  ").str(k[3]).c(' ').str(T(S_BAR_KEY_STYLE))
         .str("  ").str(k[4]).c(' ').str(T(S_BAR_KEY_FREE)).str("  ").str(k[5]).c(' ').str(T(S_BAR_KEY_FILL));
        if (g_hover && g_cfg.wheelKey > 0) b.str("  ").str(k[7]).str(T(S_BAR_KEY_WHEEL));
        if (g_cfg.keyOff > 0) b.str("  ").str(k[6]).c(' ').str(T(S_BAR_KEY_OFF));
    }
    copy_text(out, b.z(), cap);
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
    static char last[1024] = "", changed[1024] = "";
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
            HGDIOBJ of = SelectObject(dc, panel_font(px));
            RECT tr = { 0, 0, 4000, 0 };
            draw_text(dc, g_overlayText, &tr, DT_CALCRECT | DT_NOPREFIX);
            SelectObject(dc, of); ReleaseDC(g_overlay, dc);
            int w = (tr.right - tr.left) + px * 2, h = (tr.bottom - tr.top) + px / 2 + 3;
            int x = r.left + ((r.right - r.left) - w) / 2, y = r.bottom - (int)(g_cfg.panelBottom * sc) - h;
            if (x < r.left) x = r.left; if (y < r.top) y = r.top;
            static RECT lastPos = { 0, 0, 0, 0 };
            if (!shown || lastPos.left != x || lastPos.top != y || lastPos.right != w || lastPos.bottom != h) {
                SetWindowPos(g_overlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                lastPos = { x, y, w, h };
            }
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
    Buf b; b.fmt(T(S_FREE_DISTANCE), tenths((int)(v * 10.f + 0.5f)).s); flash(b.z());
}
static LRESULT CALLBACK wheel_proc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && w == WM_MOUSEWHEEL && g_active && g_hover && game_focused() &&
        key_down(g_cfg.wheelKey)) {
        short delta = (short)HIWORD(((MSLLHOOKSTRUCT*)l)->mouseData);
        hover_step(delta > 0 ? 0.5f : -0.5f);
        return 1;                                    // the game does not see this wheel turn
    }
    return CallNextHookEx(nullptr, code, w, l);
}
// A low-level mouse hook sits in the path of every mouse event on the system, so it
// is installed only while free placement is on and the game has focus (the only
// time Q + wheel is used), and removed otherwise. The thread runs at high priority
// and pumps messages continuously, so the hook never delays mouse input.
static DWORD WINAPI wheel_thread(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HHOOK h = nullptr;
    for (;;) {
        bool want = g_active && g_hover && g_cfg.wheelKey > 0 && game_focused();
        if (want && !h) {
            h = SetWindowsHookExW(WH_MOUSE_LL, wheel_proc, GetModuleHandleW(nullptr), 0);
            LOG(.str("mouse wheel hook ").str(h ? "on" : "failed"));
        } else if (!want && h) {
            UnhookWindowsHookEx(h); h = nullptr;
            LOG(.str("mouse wheel hook off"));
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
}

// ------------------------------------------------------------------ hotkeys
// ---- tool settings, changed by the hotkeys and by the in-game menu
static void set_mode(LONG m) {
    g_mode = m; g_corners = 0; g_cancelReq = 1;
    LOG(.str("mode -> ").str(m == MODE_AREA ? "area" : "prefab"));
}
static void set_replace(LONG r) { g_replace = r; LOG(.str("replace -> ").i(r)); }
static void set_replace_style(LONG st) {
    g_replaceStyle = st;
    LOG(.str("replace style -> ").str(st == STYLE_FULL ? "full" : st == STYLE_OVERLAP ? "overlap" : "connected"));
}
static void set_fill(LONG f) { g_fill = f; LOG(.str("fill -> ").i(f)); }
static void set_hover(LONG h) { g_hover = h; g_cxLogged = 0; LOG(.str("free placement -> ").i(h)); }

// ---- in-game menu (native game UI), stage 1 experiment
// The play-state UI runs every frame on the UI thread and draws the screen named by
// the HudScreenType byte at hud+0x1cd8 (1 = InGame). Screen 52 (TechGallery) is a
// developer test screen that no game path opens; the mod opens it with the game's
// toggleHudScreen from inside the play-state UI and will draw its own menu in that slot.
// Screen 53 is left alone (Creative Mode uses it).
enum { SCREEN_INGAME = 1, SCREEN_MOD = 52 };
typedef void (*PlayUiFn)(void*, void*, void*, void*);
typedef uint8_t (*ToggleScreenFn)(void* uiCtx, uint8_t screen);
static ToggleScreenFn g_toggleScreen = nullptr;
static PlayUiFn g_playUiOrig = nullptr, g_modScreenOrig = nullptr;
static uint8_t** g_uiCtxGlobal = nullptr;          // the UI context pointer (rip global)
static volatile LONG g_menuReq = 0;                 // 1 = open, 2 = close (input thread -> UI thread)
static volatile LONG g_menuOpen = 0;
static volatile LONG g_bindWaiting = -1;           // key action in the menu waiting for its new key
static bool g_tabsHooked = false;                   // the game's tab bar shows the menu's pages

// Same chain and null checks as the play-state UI itself, which reads it right after.
static uint8_t* hud_screen_byte() {
    uint8_t* ctx = *g_uiCtxGlobal;
    if (!ctx) return nullptr;
    uint8_t** a = *(uint8_t***)(ctx + 0xd0);
    if (!*a) return nullptr;
    uint8_t* hud = *(uint8_t**)(*a + 0x38);
    return hud ? hud + 0x1cd8 : nullptr;
}

static void draw_float_info();
static void play_ui_hook(void* a, void* b, void* c, void* d) {
    uint8_t* scr = hud_screen_byte();
    if (scr) {
        static int last = -1;
        LONG req = InterlockedExchange(&g_menuReq, 0);
        bool openable = *scr == SCREEN_INGAME || (*scr >= 29 && *scr <= 37);   // the game, or a multi-tool (hammer) screen
        if (req == 1 && openable) { g_toggleScreen(*g_uiCtxGlobal, SCREEN_MOD); LOG(.str("menu: open")); }
        else if (req == 1) LOG(.str("menu: not opened, screen ").i(*scr).str(" is up"));
        else if (req == 2 && *scr == SCREEN_MOD) { g_toggleScreen(*g_uiCtxGlobal, SCREEN_INGAME); LOG(.str("menu: close")); }
        if (*scr != last) {
            LOG(.str("hud screen ").i(last).str(" -> ").i(*scr));
            // Esc on our screen: the game opens its system menu; go back to the game instead.
            if (last == SCREEN_MOD && *scr == 2) { g_toggleScreen(*g_uiCtxGlobal, SCREEN_INGAME); LOG(.str("menu: esc, back to game")); }
            last = *scr;
        }
        g_menuOpen = *scr == SCREEN_MOD;
        if (!g_menuOpen) g_bindWaiting = -1;
    }
    g_playUiOrig(a, b, c, d);
    draw_float_info();
}

// Screen 52: the mod's own window, built like the game's popups (reportSharedWorldPopup):
// fullscreen window on layer 9, modal; dimmed root; centred panel with the game's menu
// frame (fancyBox style 8); text in the game's title and body styles.
typedef void* (*BeginWindowFn)(void* win, const void* name, uint8_t fullscreen, int32_t layer, int32_t inputMask, int32_t flags);
static BeginWindowFn g_beginWindow = nullptr;
struct UiVec2 { float x, y; };
struct UiColor { uint32_t argb; float intensity; };
struct UiStringView { const char* p; uint64_t n; };
struct UiTextString { const char* p; uint64_t n; uint64_t hash; };
static struct {
    void* (*windowContext)(void*);
    void (*beginRoot)(void*, void*);
    void (*beginBox)(void*, uintptr_t);
    void (*endBox)(void*);
    void (*endWindow)(void*);
    void (*setBackground)(void*, const UiColor*);
    void (*setPadding)(void*, const float*);
    void (*setSpacing)(void*, const UiVec2*, uintptr_t);
    void (*setSelfAlign)(void*, const uint16_t*);
    void (*setLayout)(void*, const uint16_t*);
    void (*setStretch)(void*, UiVec2);
    void (*setWidth)(void*, float);
    void (*fancyBox)(void*, const void*);
    void (*beginText)(void*, const UiTextString*, const void*, uintptr_t);
    void (*endText)(void*);
    bool (*settingOption)(void*, uint8_t*);
    void (*settingHeader)(const UiTextString*, float);
    bool (*button)(const void*);
    void (*setSpacingX)(void*, const UiVec2*, uintptr_t);
    void (*blurBehind)(void*, const float*, uint64_t);
    void (*fillRect)(const float*, uint64_t);
    void (*tabBar)(uint8_t menu, uint8_t flag);
    void* (*tabList)(uint8_t menu, void* list, void* state);
    bool (*tabDraw)(void* tab, void* out);
    void* (*makeHint)(void* hint, uint8_t action, uint8_t enabled);
    void (*drawHints)(const void* list, uint16_t align, const float* padding, uint8_t, uint8_t);
    void* (*imageCtx)();                                    // optional: small on/off mark
    uint32_t* (*resolveImage)(void* ctx, uint32_t* handle, const void* texture);
    void (*drawImage)(void* box, const uint32_t* handle, const void* color);
    void (*setSize)(void*, UiVec2);
    void* (*passObj)(void* uiCtx);                           // optional: soft backdrop
    int (*passId)(void* pass);
    void* (*drawList)(void* pass);
    void (*blend)(void* list, uint8_t mode);
    void (*nineSlice)(void* list, const float* rect, uint32_t image, const float* borders, const float* radii, uint64_t color);
    void* (*stretcher)(void* box, UiVec2);                  // optional: the HUD toggle row
    void (*hudSwitch)(uint8_t on);
} ui;
enum { STYLE_TITLE = 0x11b94, STYLE_BODY = 0x10a0c };   // text styles in the UI bundle

static UiTextString ui_str(const char* t) {
    UiTextString s{ t, 0, 0xcbf29ce484222325ull };          // FNV-1a 64, as the game hashes it
    for (; t[s.n]; s.n++) { s.hash ^= (uint8_t)t[s.n]; s.hash *= 0x100000001b3ull; }
    return s;
}
static void ui_text(const char* t, int style) {
    alignas(16) uint8_t text[0x200] = {};
    UiTextString s = ui_str(t);
    uint8_t* bundle = *(uint8_t**)(*g_uiCtxGlobal + 0xc8);
    ui.beginText(text, &s, bundle + style, 0);
    ui.endText(text);
}
static void* ui_node(uint8_t* box) { return *(void**)(box + 8); }

// settingOption parameters (0xc48 bytes): up to 64 entries of 0x30 {u32 value, TextString
// label @+8}, entry count u8 @0xc00, row label @0xc08, description @0xc20, arrows @0xc3a,
// interactive @0xc3c.
static bool ui_option(const char* label, const char* const* names, int count, LONG cur, LONG* out) {
    alignas(16) static uint8_t prm[0xc80];
    memset(prm, 0, sizeof prm);
    for (int k = 0; k < count; k++) {
        uint8_t* e = prm + k * 0x30;
        *(uint32_t*)e = (uint32_t)k;
        *(UiTextString*)(e + 8) = ui_str(names[k]);
    }
    prm[0xc00] = (uint8_t)count;
    *(UiTextString*)(prm + 0xc08) = ui_str(label);
    *(UiTextString*)(prm + 0xc20) = ui_str("");
    prm[0xc3a] = 1;
    *(uint32_t*)(prm + 0xc3c) = 1;
    uint8_t v = (uint8_t)cur;
    if (!ui.settingOption(prm, &v) || v == cur || v >= count) return false;
    *out = v;
    return true;
}

// The game's button (params 0xa8 as the Deletion Confirm menu fills them; text @+0x78).
static bool ui_button(const char* text, bool enabled = true) {
    alignas(16) uint8_t prm[0xc0] = {};
    *(uint32_t*)(prm + 0x04) = 0x10202;
    *(uint16_t*)(prm + 0x08) = 0x100;
    *(uint64_t*)(prm + 0x10) = 3;                            // style
    *(float*)(prm + 0x4c) = 10.0f;
    *(float*)(prm + 0x50) = 5.0f;
    *(float*)(prm + 0x54) = 1.0f;                            // scale
    prm[0x58] = enabled;
    *(UiTextString*)(prm + 0x78) = ui_str(text);
    return ui.button(prm) && enabled;
}

// Options page: the hotkey settings as rows of the game's options. Changes apply at once.
static void draw_tool_settings() {
    const char* const offOn[] = { T(S_OPT_OFF), T(S_OPT_ON) };
    const char* const modes[] = { T(S_OPT_MODE_PREFAB), T(S_OPT_MODE_AREA) };
    const char* const styles[] = { T(S_OPT_STYLE_FULL), T(S_OPT_STYLE_OVERLAP), T(S_OPT_STYLE_CONNECTED) };
    static const LONG styleIds[] = { STYLE_FULL, STYLE_OVERLAP, STYLE_CONNECTED };
    const char* const fills[] = { T(S_OPT_FILL_SOLID), T(S_OPT_FILL_HOLLOW), T(S_OPT_FILL_WALLS) };
    LONG v;
    if (ui_option(T(S_OPT_MODE), modes, 2, g_mode, &v)) set_mode(v);
    if (ui_option(T(S_OPT_REPLACE), offOn, 2, g_replace ? 1 : 0, &v)) set_replace(v);
    int st = g_replaceStyle == STYLE_OVERLAP ? 1 : g_replaceStyle == STYLE_CONNECTED ? 2 : 0;
    if (ui_option(T(S_OPT_STYLE), styles, 3, st, &v)) set_replace_style(styleIds[v]);
    if (ui_option(T(S_OPT_FILL), fills, core::FILL_COUNT, g_fill, &v)) set_fill(v);
    if (ui_option(T(S_OPT_FREE), offOn, 2, g_hover ? 1 : 0, &v)) set_hover(v);
    // language: as the game, or one by hand (saved as Language= in the ini)
    const char* langs[1 + kLanguageCount];
    langs[0] = T(S_OPT_LANGUAGE_GAME);
    for (int k = 0; k < kLanguageCount; k++) langs[1 + k] = kLanguages[k].name;
    int cur = same_ascii(g_cfg.language, "auto") ? 0 : lang_by_code(g_cfg.language) + 1;
    if (cur < 0) cur = 0;
    if (ui_option(T(S_OPT_LANGUAGE), langs, 1 + kLanguageCount, cur, &v) && v != cur) {
        lstrcpynA(g_cfg.language, v == 0 ? "auto" : kLanguages[v - 1].code, sizeof g_cfg.language);
        write_full_ini(g_cfg);
        LOG(.str("menu: language -> ").str(g_cfg.language));
        lang_update();
        flash(T(S_LANGUAGE_SET));
    }
}

// Keys page: one row per action, the button shows its key; click it, then press the
// new key (the input thread catches it). Esc cancels, Delete or Backspace clears.
struct KeyAction { Str label; int* key; };
static KeyAction g_keyActions[] = {
    { S_ACT_MENU, &g_cfg.keyMenu },
    { S_ACT_AREA, &g_cfg.keyStructure },
    { S_ACT_REPLACE, &g_cfg.keyReplace },
    { S_ACT_STYLE, &g_cfg.keyStyle },
    { S_ACT_FILL, &g_cfg.keyFill },
    { S_ACT_CLEAR, &g_cfg.keyCancel },
    { S_ACT_FREE, &g_cfg.keyHover },
    { S_ACT_FARTHER, &g_cfg.keyFarther },
    { S_ACT_CLOSER, &g_cfg.keyCloser },
    { S_ACT_WHEEL, &g_cfg.wheelKey },
    { S_ACT_OFF, &g_cfg.keyOff },
};
static const int kKeyActions = sizeof g_keyActions / sizeof g_keyActions[0];

static void draw_key_row(int k) {
    {
        alignas(16) uint8_t row[0x40] = {}, label[0x40] = {};
        ui.beginBox(row, 0);
        UiVec2 gap{ 24, 0 };
        ui.setSpacingX(ui_node(row), &gap, 0);
        uint16_t midLeft = 0x201;
        ui.setLayout(ui_node(row), &midLeft);
        ui.beginBox(label, 0);
        ui.setWidth(ui_node(label), 380);
        ui_text(T(g_keyActions[k].label), STYLE_BODY);
        ui.endBox(label);
        char name[40];
        if (g_bindWaiting == k) copy_text(name, T(S_MENU_PRESS_KEY), 40);
        else if (*g_keyActions[k].key > 0) vk_name(*g_keyActions[k].key, name);
        else copy_text(name, T(S_MENU_NO_KEY), 40);
        if (ui_button(name) && g_bindWaiting < 0) { g_bindWaiting = k; LOG(.str("menu: rebinding ").str(kStrEn[g_keyActions[k].label])); }
        ui.endBox(row);
    }
}

// Two columns side by side, so the list fits the screen.
static void draw_keys() {
    alignas(16) uint8_t cols[0x40] = {}, col[2][0x40] = {};
    ui.beginBox(cols, 0);
    UiVec2 cgap{ 60, 0 };
    ui.setSpacingX(ui_node(cols), &cgap, 0);
    uint16_t topLeft = 0x101;
    ui.setLayout(ui_node(cols), &topLeft);
    int half = (kKeyActions + 1) / 2;
    for (int c = 0; c < 2; c++) {
        ui.beginBox(col[c], 0);
        UiVec2 rgap{ 0, 14 };
        ui.setSpacing(ui_node(col[c]), &rgap, 0);
        ui.setLayout(ui_node(col[c]), &topLeft);
        for (int k = c * half; k < kKeyActions && k < (c + 1) * half; k++) draw_key_row(k);
        ui.endBox(col[c]);
    }
    ui.endBox(cols);
    ui_text(T(g_bindWaiting >= 0 ? S_MENU_KEYS_WAITING : S_MENU_KEYS_HELP), STYLE_BODY);
}

// Called by the input thread with the key pressed while an action waits for one.
static void bind_key(int vk) {
    LONG k = g_bindWaiting;
    if (k < 0 || k >= kKeyActions) return;
    if (vk == VK_ESCAPE) { g_bindWaiting = -1; LOG(.str("menu: rebinding cancelled")); return; }
    if (vk == VK_RETURN || vk == VK_TAB) return;          // no tool key; keep waiting
    if (vk == VK_DELETE || vk == VK_BACK) vk = 0;
    if (g_keyActions[k].key == &g_cfg.keyMenu && vk == 0) { g_bindWaiting = -1; return; }   // keep a way in
    for (int j = 0; j < kKeyActions; j++)                    // a key does one thing: take it from the other action
        if (j != k && vk > 0 && *g_keyActions[j].key == vk) *g_keyActions[j].key = 0;
    *g_keyActions[k].key = vk;
    write_full_ini(g_cfg);
    InterlockedIncrement(&g_keysVersion);
    g_bindWaiting = -1;
    char n[32] = "none"; if (vk > 0) vk_name(vk, n);
    LOG(.str("menu: ").str(kStrEn[g_keyActions[k].label]).str(" -> ").str(n));
}

// Bottom-right key hints like the game's menus. A hint shows the key of a UiActionId
// (1 = Back, the Esc key); custom label flag @+0x1b, label TextString @+0x20.
enum { UI_ACTION_BACK = 1 };
static void draw_hint_bar() {
    alignas(16) static uint8_t hints[16 * 0x48];
    int n = 0;
    auto add = [&](uint8_t action, const char* label) {
        uint8_t* h = hints + n++ * 0x48;
        ui.makeHint(h, action, 1);
        h[0x1b] = 1;
        *(UiTextString*)(h + 0x20) = ui_str(label);
    };
    add(UI_ACTION_BACK, T(g_bindWaiting >= 0 ? S_MENU_CANCEL : S_MENU_CLOSE));
    struct { void* p; uint64_t n; } list{ hints, (uint64_t)n };
    const float pad[4] = { 40, 40, 40, 40 };               // clear of the screen corner, as the system menu
    ui.drawHints(&list, 0x303, pad, 0, 0);
}

// Full-screen like the game's own menus: blurred, darkened world, the menu on the left.
static volatile LONG g_menuPage = 0;                 // 0 = keys, 1 = options
static const int kMenuPages = 2;
static const Str g_pageNames[kMenuPages] = { S_MENU_KEYS, S_MENU_OPTIONS };
static bool g_modTabs = false;                       // the tab bar being drawn is the mod's

// Tab list of the game's tab bar: the mod's pages while it draws its bar. A tab is 0x40
// bytes: shortcut @+1 (0x8b none), label TextString @+0x18, active @+0x30, hover and
// interactive @+0x31/+0x32, opacity @+0x34. The list grows like the game's vectors:
// {data, count, capacity, grow fn}; the bar passes inline room for 10.
static void* mod_tab_list(uint8_t menu, void* list, void* state) {
    if (!g_modTabs) return ui.tabList(menu, list, state);
    static uint64_t notEmpty = 1;
    uint8_t** v = (uint8_t**)list;
    for (int k = 0; k < kMenuPages; k++) {
        uint64_t n = (uint64_t)v[1];
        if (n >= (uint64_t)v[2]) break;
        uint8_t* t = v[0] + n * 0x40;
        memset(t, 0, 0x40);
        t[1] = 0x8b;
        *(UiTextString*)(t + 0x18) = ui_str(T(g_pageNames[k]));
        t[0x30] = g_menuPage == k;
        t[0x31] = 1; t[0x32] = 1;
        *(float*)(t + 0x34) = 1.0f;
        v[1] = (uint8_t*)(n + 1);
    }
    return &notEmpty;
}
// Its tab widget returns true for the active tab and the hovered one; a click on a
// hovered tab switches the page. The game never sees it (it would switch screens).
static bool g_menuClick = false;                     // left button went down this frame
static bool mod_tab_draw(void* tab, void* out) {
    bool hot = ui.tabDraw(tab, out);                  // the active tab, or the one under the mouse
    if (!g_modTabs) return hot;
    if (hot && g_menuClick && !((uint8_t*)tab)[0x30]) {
        uint64_t h = *(uint64_t*)((uint8_t*)tab + 0x28);
        for (int k = 0; k < kMenuPages; k++)
            if (ui_str(T(g_pageNames[k])).hash == h && g_menuPage != k) { g_menuPage = k; g_bindWaiting = -1; LOG(.str("menu: page ").str(kStrEn[g_pageNames[k]])); }
    }
    return false;
}
static void draw_mod_menu() {
    alignas(16) uint8_t win[0x40] = {}, root[0x40] = {}, panel[0x40] = {}, tabs[0x40] = {};
    UiStringView name{ "buildingToolsMenu", 17 };
    g_beginWindow(win, &name, 1, 9, -1, 1);
    ui.beginRoot(root, ui.windowContext(win));
    ui.setStretch(ui_node(root), UiVec2{ 1, 1 });
    UiVec2 rgap{ 0, 0 };
    ui.setSpacing(ui_node(root), &rgap, 0);
    const float* screen = (const float*)((uint8_t*)ui_node(root) + 0x114);   // node rect (last layout)
    ui.blurBehind(*g_uiCtxGlobal, screen, 0x3f800000ffffffffull);
    ui.fillRect(screen, 0x3f800000a0000000ull);   // {ABGR colour, intensity}
    {   // mouse click edge for the tabs
        static bool wasDown = false;
        bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        g_menuClick = down && !wasDown;
        wasDown = down;
    }
    if (g_tabsHooked) { g_modTabs = true; ui.tabBar(3, 0); g_modTabs = false; }

    ui.beginBox(panel, 0);
    void* n = ui_node(panel);
    const float pad[4] = { 120, 50, 60, 40 };
    ui.setPadding(n, pad);
    UiVec2 gap{ 0, 22 };
    ui.setSpacing(n, &gap, 0);
    uint16_t topLeft = 0x101;
    ui.setSelfAlign(n, &topLeft);
    ui.setLayout(n, &topLeft);
    ui.setWidth(n, 1000);
    ui.setStretch(n, UiVec2{ 0, 1 });                       // full height: the hints sit at the bottom

    ui_text(T(S_MENU_TITLE), STYLE_TITLE);
    if (!g_tabsHooked) {                                  // fallback: plain buttons
        ui.beginBox(tabs, 0);
        UiVec2 tgap{ 16, 0 };
        ui.setSpacingX(ui_node(tabs), &tgap, 0);
        for (int k = 0; k < kMenuPages; k++)
            if (ui_button(T(g_pageNames[k]), g_menuPage != k)) { g_menuPage = k; g_bindWaiting = -1; }
        ui.endBox(tabs);
    }
    UiTextString h = ui_str(T(g_pageNames[g_menuPage]));
    ui.settingHeader(&h, 0);
    if (g_menuPage == 0) draw_keys(); else draw_tool_settings();
    { Buf f; f.fmt(T(S_MENU_FOOTER), MOD_VERSION); ui_text(f.z(), STYLE_BODY); }
    if (*T(S_MENU_TRANSLATION_NOTE)) ui_text(T(S_MENU_TRANSLATION_NOTE), STYLE_BODY);   // "may contain mistakes, corrections welcome"

    ui.endBox(panel);
    draw_hint_bar();
    ui.endBox(root);
    ui.endWindow(win);
}

static void mod_screen_hook(void*, void*, void*, void*) {
    static LONG logged = 0;
    if (!InterlockedExchange(&logged, 1)) LOG(.str("menu: screen drawn"));
    draw_mod_menu();
}

// ---- mod rows in the game's building HUD box (bottom right, under "Undo Build Action")
// Each row is the game's bindingHint: key icon + label. The binding (0xc4 bytes) holds
// two keyboard combos @0/@0x30 and two gamepad combos @0x62/@0x92; a combo (0x30) is
// the input (12 bytes: device u8 @0 (1 key, 2 mouse), USB HID key code u16 @2, mouse
// button u8 @4) plus up to three modifier inputs. The row parameters (0x78) are copied
// from the game's own rows; label TextString @0x58, colour @0x70, @0x2f = take the
// action's name (cleared).
typedef void (*HudRowFn)(uint8_t action, void* params);
typedef void (*HudFinishFn)(void* box);
typedef void (*BindingHintFn)(const void* binding, const void* params);
static HudRowFn g_hudRowOrig = nullptr;
static HudFinishFn g_hudFinishOrig = nullptr;
static BindingHintFn g_bindingHint = nullptr;
alignas(16) static uint8_t g_hudRowTpl[0x80];
static bool g_hudRowTplOk = false;

static int vk_to_hid(int vk) {
    if (vk >= 'A' && vk <= 'Z') return 4 + vk - 'A';
    if (vk >= '1' && vk <= '9') return 30 + vk - '1';
    if (vk == '0') return 39;
    if (vk >= VK_F1 && vk <= VK_F12) return 58 + vk - VK_F1;
    if (vk >= VK_F13 && vk <= VK_F24) return 104 + vk - VK_F13;
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) return 89 + vk - VK_NUMPAD1;
    switch (vk) {
    case VK_RETURN: return 40; case VK_ESCAPE: return 41; case VK_BACK: return 42; case VK_TAB: return 43;
    case VK_SPACE: return 44; case VK_OEM_MINUS: return 45; case VK_OEM_PLUS: return 46; case VK_OEM_4: return 47;
    case VK_OEM_6: return 48; case VK_OEM_5: return 49; case VK_OEM_1: return 51; case VK_OEM_7: return 52;
    case VK_OEM_3: return 53; case VK_OEM_COMMA: return 54; case VK_OEM_PERIOD: return 55; case VK_OEM_2: return 56;
    case VK_CAPITAL: return 57; case VK_SNAPSHOT: return 70; case VK_SCROLL: return 71; case VK_PAUSE: return 72;
    case VK_INSERT: return 73; case VK_HOME: return 74; case VK_PRIOR: return 75; case VK_DELETE: return 76;
    case VK_END: return 77; case VK_NEXT: return 78; case VK_RIGHT: return 79; case VK_LEFT: return 80;
    case VK_DOWN: return 81; case VK_UP: return 82; case VK_NUMLOCK: return 83; case VK_DIVIDE: return 84;
    case VK_MULTIPLY: return 85; case VK_SUBTRACT: return 86; case VK_ADD: return 87; case VK_NUMPAD0: return 98;
    case VK_DECIMAL: return 99; case VK_OEM_102: return 100;
    case VK_CONTROL: case VK_LCONTROL: return 224; case VK_SHIFT: case VK_LSHIFT: return 225;
    case VK_MENU: case VK_LMENU: return 226; case VK_RCONTROL: return 228; case VK_RSHIFT: return 229; case VK_RMENU: return 230;
    }
    return 0;
}
static int vk_to_mouse(int vk) {
    switch (vk) { case VK_LBUTTON: return 1; case VK_RBUTTON: return 2; case VK_MBUTTON: return 3; case VK_XBUTTON1: return 4; case VK_XBUTTON2: return 5; }
    return 0;
}
// Fills a binding for a key setting; false if the game has no icon for the key.
static bool make_binding(int code, uint8_t* b) {
    memset(b, 0, 0xc4);
    int vk = code & 0xff, hid = vk_to_hid(vk), mb = vk_to_mouse(vk);
    if (code <= 0 || (!hid && !mb)) return false;
    if (hid) { b[0] = 1; *(uint16_t*)(b + 2) = (uint16_t)hid; }
    else { b[0] = 2; b[4] = (uint8_t)mb; }
    uint8_t* m = b + 0xc;
    const int mods[3][2] = { { KEY_CTRL, 224 }, { KEY_SHIFT, 225 }, { KEY_ALT, 226 } };
    for (auto& md : mods)
        if (code & md[0]) { m[0] = 1; *(uint16_t*)(m + 2) = (uint16_t)md[1]; m += 0xc; }
    return true;
}

static void hud_row_hook(uint8_t action, void* params) {
    memcpy(g_hudRowTpl, params, 0x78);
    g_hudRowTplOk = true;
    g_hudRowOrig(action, params);
}

// The small on/off mark the bottom-left HUD slots use (21 x 21).
static void hud_mark(bool on) {
    alignas(16) uint8_t box[0x40] = {};
    uint8_t* bundle = *(uint8_t**)(*g_uiCtxGlobal + 0xc8);
    ui.beginBox(box, 0);
    ui.setSize(ui_node(box), UiVec2{ 21.f, 21.f });
    UiColor white{ 0xffffffff, 1.0f };
    const void* color = on ? (const void*)&white : (const void*)(bundle + 0xd4d8);
    uint32_t handle = 0;
    uint32_t* h = ui.resolveImage(ui.imageCtx(), &handle, bundle + (on ? 0xfed4 : 0xfec4));
    ui.drawImage(box, h, color);
    ui.endBox(box);
}

// One row: key icon + label, and the small on/off mark after it for on/off settings
// (state: -1 = none, 0 = off, 1 = on). The box is narrow: labels stay short.
static void hud_key_row(int key, const char* label, int state = -1) {
    if (key <= 0) return;                       // the setting has no key
    alignas(16) uint8_t bind[0xd0];
    alignas(16) uint8_t prm[0x80];
    alignas(16) uint8_t row[0x40] = {};
    char text[256];
    memcpy(prm, g_hudRowTpl, 0x78);
    if (!make_binding(key, bind)) {             // no icon for this key: its name goes into the label
        char k[32]; vk_name(key, k);
        Buf b; b.c('[').str(k).str("] ").str(label); copy_text(text, b.z(), sizeof text);
        label = text;
        prm[0x2c] = 1;                          // draw the row without a binding
    }
    prm[0x28] = 0; prm[0x2f] = 0; prm[0x30] = 0; prm[0x41] = 0; prm[0x4c] = 0;
    *(float*)(prm + 0x48) = 0.f;
    *(UiTextString*)(prm + 0x58) = ui_str(label);
    *(uint32_t*)(prm + 0x70) = 0xffffffff; *(float*)(prm + 0x74) = 1.0f;
    bool sw = state >= 0 && (ui.hudSwitch || ui.imageCtx);
    if (sw) {                                   // as the game's "first person view" row
        ui.beginBox(row, 0);
        UiVec2 gap{ 10, 0 };
        ui.setSpacingX(ui_node(row), &gap, 0);
        ui.setStretch(ui_node(row), UiVec2{ 1, 0 });
    }
    g_bindingHint(bind, prm);
    if (sw) {
        if (ui.hudSwitch) { ui.stretcher(row, UiVec2{ 1, 1 }); ui.hudSwitch(state > 0); }
        else hud_mark(state > 0);
        ui.endBox(row);
    }
}

static void draw_hud_rows() {
    if (!g_cfg.hudRows || !g_hudRowTplOk || g_modOff || !g_active) return;
    if (GetTickCount() - g_buildTick > 500) return;            // only while the hammer is building
    static const Str styles[] = { S_HUD_STYLE_FULL, S_HUD_STYLE_OVERLAP, S_HUD_STYLE_CONNECTED };
    static const Str fills[] = { S_HUD_FILL_SOLID, S_HUD_FILL_HOLLOW, S_HUD_FILL_WALLS };
    bool sw = ui.hudSwitch || ui.imageCtx;
    hud_key_row(g_cfg.keyStructure, T(sw ? S_HUD_AREA : g_mode == MODE_AREA ? S_HUD_AREA_ON : S_HUD_AREA_OFF), g_mode == MODE_AREA);
    if (g_mode == MODE_AREA) {
        hud_key_row(g_cfg.keyFill, T(fills[g_fill % 3]));
        if (g_corners > 0) hud_key_row(g_cfg.keyCancel, T(S_HUD_CLEAR));
    }
    hud_key_row(g_cfg.keyReplace, T(sw ? S_HUD_REPLACE : g_replace ? S_HUD_REPLACE_ON : S_HUD_REPLACE_OFF), g_replace != 0);
    if (g_replace) {
        int st = g_replaceStyle == STYLE_OVERLAP ? 1 : g_replaceStyle == STYLE_CONNECTED ? 2 : 0;
        hud_key_row(g_cfg.keyStyle, T(styles[st]));
    }
    hud_key_row(g_cfg.keyHover, T(sw ? S_HUD_FREE : g_hover ? S_HUD_FREE_ON : S_HUD_FREE_OFF), g_hover != 0);
    if (g_hover && g_cfg.wheelKey > 0) {
        Buf h; h.fmt(T(S_HUD_WHEEL), tenths((int)(g_hoverDist * 10.f + 0.5f)).s); hud_key_row(g_cfg.wheelKey, h.z());
    }
    hud_key_row(g_cfg.keyMenu, T(S_HUD_MENU));
    g_hudRowsTick = GetTickCount();
}

// Mod messages (any time) and the area state and size (while building) for the floating text.
static bool hud_info(char* out, int cap) {
    if (!g_cfg.hudRows || !g_active) return false;
    Buf b;
    bool building = !g_modOff && GetTickCount() - g_buildTick < 500;
    if (GetTickCount() < g_flashUntil) b.str(g_flash);
    else if (building && g_mode == MODE_AREA) {
        b.fmt(T(g_scOp == OP_REMOVE ? S_BAR_AREA_REMOVE : g_replace ? S_BAR_AREA_REPLACE : S_BAR_AREA_BUILD),
              num(g_corners).s, num(g_cfg.corners).s);
        core::Plan pl; float step[3]; bool ok = false;
        if (g_corners > 0 && area_plan_live(&pl, step, &ok)) {
            long n = ok ? core::fill_count(pl, g_fill) : 0;
            if (!ok || n > g_cfg.maxShapes) b.str(": ").str(T(S_FLOAT_TOO_BIG));
            else b.str(": ").fmt(T(S_FLOAT_SIZE), num(pl.count[0]).s, num(pl.count[2]).s, num(pl.count[1]).s, num(n).s);
        }
    } else return false;
    copy_text(out, b.z(), cap);
    return true;
}
// Mod messages and the area size as floating text below the screen centre, drawn like
// the game's snapping hint (cab2a0): centred box, offset down, blurred backdrop, body text.
// The game's backdrop of the snapping hint: blur behind the rect, then the soft rounded frame.
static void soft_backdrop(const float* node) {
    void* ctx = *g_uiCtxGlobal;
    uint8_t* bundle = *(uint8_t**)((uint8_t*)ctx + 0xc8);
    const float rect[4] = { node[0] - 8, node[1] - 4, node[2] + 16, node[3] + 8 };
    float c = (rect[2] < rect[3] ? rect[2] : rect[3]) * 0.5f;
    alignas(16) const float radii[4] = { c, c, c, c };
    alignas(16) uint8_t tex[16]; memcpy(tex, bundle + 0xd3b0, 16);
    alignas(16) float borders[4]; memcpy(borders, bundle + 0xd3c0, 16);
    ui.blurBehind(ctx, rect, 0x3f800000ffffffffull);
    bool draw = ui.passId(ui.passObj(ctx)) == 4;
    if (draw) ui.blend(ui.drawList(ui.passObj(ctx)), 3);
    uint32_t h = 0;
    uint32_t image = *ui.resolveImage(ui.imageCtx(), &h, tex);
    if (draw) {
        ui.nineSlice(ui.drawList(ui.passObj(ctx)), rect, image, borders, radii, 0x3f800000ffffffffull);
        ui.blend(ui.drawList(ui.passObj(ctx)), 0);
    }
}

static void draw_float_info() {
    static char text[512];
    uint8_t* scr = hud_screen_byte();
    if (!scr || !(*scr == SCREEN_INGAME || (*scr >= 29 && *scr <= 37)) || !hud_info(text, sizeof text)) return;
    alignas(16) uint8_t win[0x40] = {}, root[0x40] = {}, box[0x40] = {};
    UiStringView name{ "buildingToolsInfo", 17 };
    g_beginWindow(win, &name, 1, 1, 0, 0);                  // fullscreen, low layer, takes no input
    ui.beginRoot(root, ui.windowContext(win));
    ui.setStretch(ui_node(root), UiVec2{ 1, 1 });
    ui.beginBox(box, 0);
    uint8_t* n = (uint8_t*)ui_node(box);
    uint16_t centre = 0x202;
    ui.setSelfAlign(n, &centre);
    *(UiVec2*)(n + 0x70) = UiVec2{ 0, 130 };               // offset (the snapping hint sits at +60)
    const float pad[4] = { 16, 6, 16, 6 };
    ui.setPadding(n, pad);
    const float* r = (const float*)(n + 0x114);            // node rect {x, y, w, h} (last layout)
    if (ui.nineSlice && ui.imageCtx) soft_backdrop(r);
    else { ui.blurBehind(*g_uiCtxGlobal, r, 0x3f800000ffffffffull); ui.fillRect(r, 0x3f80000080000000ull); }
    ui_text(text, STYLE_BODY);
    ui.endBox(box);
    ui.endBox(root);
    ui.endWindow(win);
    g_floatTick = GetTickCount();
}

static void hud_finish_hook(void* box) {
    static LONG logged = 0;
    if (g_cfg.hudRows && g_hudRowTplOk && !InterlockedExchange(&logged, 1)) LOG(.str("hud rows: drawn"));
    draw_hud_rows();
    g_hudFinishOrig(box);
}

static void install_hud_rows() {
    uint8_t* c1 = find_unique("HudRowCall1", SIG_HUD_ROW_CALL1);
    uint8_t* c2 = find_unique("HudRowCall2", SIG_HUD_ROW_CALL2);
    g_bindingHint = (BindingHintFn)find_unique("BindingHint", SIG_BINDING_HINT);
    if (!c1 || !c2 || !g_bindingHint) { LOG(.str("hud rows: not available (code not found)")); return; }
    uint8_t* s1 = c1 + 0xf; uint8_t* s2 = c2 + 0xf; uint8_t* fs = c2 + 0x68;
    uint8_t* row = s1 + 5 + *(int32_t*)(s1 + 1);
    if (s1[0] != 0xE8 || s2[0] != 0xE8 || fs[0] != 0xE8 || s2 + 5 + *(int32_t*)(s2 + 1) != row ||
        memcmp(fs - 4, "\x48\x8D\x4D\x20", 4) != 0) { LOG(.str("hud rows: not available (layout changed)")); return; }
    g_hudRowOrig = (HudRowFn)row;
    g_hudFinishOrig = (HudFinishFn)(fs + 5 + *(int32_t*)(fs + 1));
    uint8_t* rows[2] = { s1, s2 };
    if (hook_calls(row, (void*)&hud_row_hook, rows, 2) != 2 ||
        hook_calls((uint8_t*)g_hudFinishOrig, (void*)&hud_finish_hook, &fs, 1) != 1) {
        LOG(.str("hud rows: not available (hook failed)")); return;
    }
    LOG(.str("hud rows: available"));
}

static void install_menu() {
    uint8_t* pu = find_unique("PlayUi", SIG_PLAY_UI);
    uint8_t* call = find_unique("PlayUiCall", SIG_PLAY_UI_CALL);
    uint8_t* disp = find_unique("ScreenDispatch", SIG_SCREEN_DISPATCH);
    g_toggleScreen = (ToggleScreenFn)find_unique("ToggleScreen", SIG_TOGGLE_SCREEN);
    if (!pu || !call || !disp || !g_toggleScreen) { LOG(.str("menu: not available (UI code not found)")); return; }
    uint8_t* site = call + 2;
    uint8_t* je = disp + 10;                                     // 0F 84 rel32 -> E8 <screen 52>
    uint8_t* s52 = je + 6 + *(int32_t*)(je + 2);
    if (site + 5 + *(int32_t*)(site + 1) != pu || s52[0] != 0xE8 || !readable(s52, 5)) {
        LOG(.str("menu: not available (UI layout changed)")); return;
    }
    g_uiCtxGlobal = (uint8_t**)(pu + 0x12 + 7 + *(int32_t*)(pu + 0x12 + 3));
    g_modScreenOrig = (PlayUiFn)(s52 + 5 + *(int32_t*)(s52 + 1));
    g_playUiOrig = (PlayUiFn)pu;
    g_beginWindow = (BeginWindowFn)find_unique("BeginWindow", SIG_BEGIN_WINDOW);
    struct { void** fn; const char* name; const char* sig; } need[] = {
        { (void**)&ui.windowContext, "UiWindowCtx", SIG_UI_WINDOW_CTX }, { (void**)&ui.beginRoot, "UiBeginRoot", SIG_UI_BEGIN_ROOT },
        { (void**)&ui.beginBox, "UiBeginBox", SIG_UI_BEGIN_BOX },        { (void**)&ui.endBox, "UiEndBox", SIG_UI_END_BOX },
        { (void**)&ui.endWindow, "UiEndWindow", SIG_UI_END_WINDOW },     { (void**)&ui.setBackground, "UiBackground", SIG_UI_BACKGROUND },
        { (void**)&ui.setPadding, "UiPadding", SIG_UI_PADDING },         { (void**)&ui.setSpacing, "UiSpacing", SIG_UI_SPACING },
        { (void**)&ui.setSelfAlign, "UiSelfAlign", SIG_UI_SELF_ALIGN },  { (void**)&ui.setLayout, "UiLayout", SIG_UI_LAYOUT },
        { (void**)&ui.setStretch, "UiStretch", SIG_UI_STRETCH },         { (void**)&ui.setWidth, "UiWidth", SIG_UI_WIDTH },
        { (void**)&ui.fancyBox, "UiFancyBox", SIG_UI_FANCY_BOX },        { (void**)&ui.beginText, "UiBeginText", SIG_UI_BEGIN_TEXT },
        { (void**)&ui.endText, "UiEndText", SIG_UI_END_TEXT },        { (void**)&ui.settingHeader, "UiSettingHeader", SIG_UI_SETTING_HEADER },
        { (void**)&ui.button, "UiButton", SIG_UI_BUTTON },             { (void**)&ui.setSpacingX, "UiSpacingX", SIG_UI_SPACING_X },
        { (void**)&ui.blurBehind, "UiBlur", SIG_UI_BLUR },             { (void**)&ui.fillRect, "UiFillRect", SIG_UI_FILL_RECT },
        { (void**)&ui.makeHint, "UiMakeHint", SIG_UI_MAKE_HINT },      { (void**)&ui.drawHints, "UiDrawHints", SIG_UI_DRAW_HINTS },
    };
    bool all = g_beginWindow != nullptr;
    for (auto& f : need) all &= (*f.fn = find_unique(f.name, f.sig)) != nullptr;
    if (uint8_t* oc = find_unique("UiSettingOptionCall", SIG_UI_SETTING_OPTION_CALL))
        ui.settingOption = (bool (*)(void*, uint8_t*))(oc + 12 + *(int32_t*)(oc + 8));
    all &= ui.settingOption != nullptr;
    if (!all) { LOG(.str("menu: not available (UI functions not found)")); g_playUiOrig = nullptr; return; }
    if (uint8_t* sb = find_unique("UiSoftBackdrop", SIG_UI_SOFT_BACKDROP)) {
        auto at = [&](int o) { return (void*)(sb + o + 5 + *(int32_t*)(sb + o + 1)); };
        ui.passObj = (void* (*)(void*))at(0x7);
        ui.passId = (int (*)(void*))at(0xf);
        ui.drawList = (void* (*)(void*))at(0x28);
        ui.blend = (void (*)(void*, uint8_t))at(0x32);
        ui.nineSlice = (void (*)(void*, const float*, uint32_t, const float*, const float*, uint64_t))at(0xf4);
    }
    if (uint8_t* tr = find_unique("HudToggleRow", SIG_HUD_TOGGLE_ROW)) {   // the HUD rows' on/off switch (optional)
        ui.stretcher = (void* (*)(void*, UiVec2))(tr + 0x14 + *(int32_t*)(tr + 0x10));
        ui.hudSwitch = (void (*)(uint8_t))(tr + 0x1d + *(int32_t*)(tr + 0x19));
    }
    {   // the small on/off mark for the HUD rows (optional)
        uint8_t* t = find_unique("UiSmallToggle", SIG_UI_SMALL_TOGGLE);
        uint8_t* sz = find_unique("UiSize", SIG_UI_SIZE);
        if (t && sz && t[0x40] == 0xE8 && t[0x52] == 0xE8 && t[0x62] == 0xE8) {
            ui.setSize = (void (*)(void*, UiVec2))sz;
            ui.resolveImage = (uint32_t* (*)(void*, uint32_t*, const void*))(t + 0x57 + *(int32_t*)(t + 0x53));
            ui.drawImage = (void (*)(void*, const uint32_t*, const void*))(t + 0x67 + *(int32_t*)(t + 0x63));
            ui.imageCtx = (void* (*)())(t + 0x45 + *(int32_t*)(t + 0x41));
        }
    }
    {   // the game's tab bar for the pages; without it the menu shows plain buttons
        uint8_t* bar = find_unique("UiTabBar", SIG_UI_TAB_BAR);
        uint8_t* lc = find_unique("UiTabListCall", SIG_UI_TAB_LIST_CALL);
        uint8_t* dc = find_unique("UiTabDrawCall", SIG_UI_TAB_DRAW_CALL);
        if (bar && lc && dc && lc[0x19] == 0xE8 && dc[0xe] == 0xE8) {
            uint8_t* ls = lc + 0x19; uint8_t* ds = dc + 0xe;
            ui.tabBar = (void (*)(uint8_t, uint8_t))bar;
            ui.tabList = (void* (*)(uint8_t, void*, void*))(ls + 5 + *(int32_t*)(ls + 1));
            ui.tabDraw = (bool (*)(void*, void*))(ds + 5 + *(int32_t*)(ds + 1));
            g_tabsHooked = hook_calls((uint8_t*)ui.tabList, (void*)&mod_tab_list, &ls, 1) == 1 &&
                           hook_calls((uint8_t*)ui.tabDraw, (void*)&mod_tab_draw, &ds, 1) == 1;
        }
        LOG(.str(g_tabsHooked ? "menu: game tab bar used" : "menu: game tab bar not found, plain tab buttons"));
    }
    if (hook_calls((uint8_t*)g_modScreenOrig, (void*)&mod_screen_hook, &s52, 1) != 1 ||
        hook_calls(pu, (void*)&play_ui_hook, &site, 1) != 1) {
        LOG(.str("menu: not available (hook failed)")); g_playUiOrig = nullptr; return;
    }
    LOG(.str("menu: available (screen 52)"));
    install_hud_rows();
}

static DWORD WINAPI input_thread(void*) {
    bool prev[8] = {}, prevOff = false, prevMenu = false, prevEsc = false;
    for (;;) {
        Sleep(15);
        if (!g_active) continue;
        bool focus = game_focused();
        {   // whole mod off / on
            bool down = focus && key_down(g_cfg.keyOff);
            if (down && !prevOff) {
                LONG off = !g_modOff;
                if (off) {      // back to the vanilla game
                    g_mode = MODE_PREFAB; g_replace = 0; g_hover = 0; g_corners = 0; g_cancelReq = 1;
                }
                g_modOff = off;
                char k[32]; vk_name(g_cfg.keyOff, k);
                Buf b; b.fmt(T(off ? S_MOD_OFF : S_MOD_ON), k);
                flash(b.z());
                LOG(.str("key: mod -> ").str(off ? "off" : "on"));
                beep(MB_ICONQUESTION);
            }
            prevOff = down;
            if (g_modOff) { for (bool& p : prev) p = false; continue; }
        }
        {   // in-game menu
            bool down = focus && g_playUiOrig && key_down(g_cfg.keyMenu);
            if (g_bindWaiting >= 0) {           // rebinding: the next key pressed (not a mouse button)
                static bool held[256]; static LONG armed = -1;
                if (armed != g_bindWaiting) {   // keys already down when it started do not count
                    for (int vk = 8; vk < 256; vk++) held[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    armed = g_bindWaiting;
                }
                for (int vk = 8; vk < 256 && focus; vk++) {
                    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU)) continue;
                    bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    if (d && !held[vk]) {               // the key with the modifiers held now (Shift+K)
                        int mods = (vk == VK_ESCAPE || vk == VK_DELETE || vk == VK_BACK) ? 0 : held_mods();
                        bind_key(vk | mods); armed = -1; prevMenu = prevEsc = true; break;
                    }
                    held[vk] = d;
                }
                if (g_bindWaiting < 0) armed = -1;
                for (bool& p : prev) p = true;
                continue;
            }
            if (down && !prevMenu) {
                if (!g_menuOpen) lang_update();          // the game's language may have changed since
                g_menuReq = g_menuOpen ? 2 : 1; LOG(.str("key: menu"));
            }
            prevMenu = down;
            bool esc = focus && g_menuOpen && (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
            if (esc && !prevEsc) { g_menuReq = 2; LOG(.str("key: esc in menu")); }
            prevEsc = esc;
            if (g_menuOpen) {                    // tool keys sleep while the menu is up; Q / E switch pages
                static bool prevQ = true, prevE = true;
                bool q = focus && (GetAsyncKeyState('Q') & 0x8000), e = focus && (GetAsyncKeyState('E') & 0x8000);
                if (q && !prevQ) g_menuPage = (g_menuPage + kMenuPages - 1) % kMenuPages;
                if (e && !prevE) g_menuPage = (g_menuPage + 1) % kMenuPages;
                prevQ = q; prevE = e;
                for (bool& p : prev) p = true;
                continue;
            }
        }
        int keys[8] = { g_cfg.keyStructure, g_cfg.keyReplace, g_cfg.keyCancel, g_cfg.keyStyle, g_cfg.keyHover,
                        g_hover ? g_cfg.keyFarther : 0, g_hover ? g_cfg.keyCloser : 0, g_cfg.keyFill };
        for (int k = 0; k < 8; k++) {
            bool down = focus && key_down(keys[k]);
            if (down && !prev[k]) {
                if (k == 0) {
                    LONG m = g_mode == MODE_PREFAB ? MODE_AREA : MODE_PREFAB;
                    set_mode(m);
                    flash(T(m == MODE_AREA ? S_MODE_AREA : S_MODE_PREFAB));
                } else if (k == 1) {
                    LONG r = !g_replace; set_replace(r);
                    flash(T(r ? S_REPLACE_ON : S_REPLACE_OFF));
                } else if (k == 3) {
                    LONG st = g_replaceStyle == STYLE_FULL ? STYLE_OVERLAP : g_replaceStyle == STYLE_OVERLAP ? STYLE_CONNECTED : STYLE_FULL;
                    set_replace_style(st);
                    flash(T(st == STYLE_FULL ? S_STYLE_FULL : st == STYLE_OVERLAP ? S_STYLE_OVERLAP : S_STYLE_CONNECTED));
                } else if (k == 7) {
                    LONG f = (g_fill + 1) % core::FILL_COUNT; set_fill(f);
                    flash(T(f == core::FILL_SOLID ? S_FILL_SOLID : f == core::FILL_HOLLOW ? S_FILL_HOLLOW : S_FILL_WALLS));
                } else if (k == 5 || k == 6) {
                    hover_step(k == 5 ? 0.5f : -0.5f);
                } else if (k == 4) {
                    LONG h = !g_hover; set_hover(h);
                    flash(T(h ? S_FREE_ON : S_FREE_OFF));
                } else {
                    g_cancelReq = 1; g_corners = 0;
                    flash(T(S_CORNERS_CLEARED));
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
static uint8_t* g_self; static size_t g_selfSize, g_gameSize;
static volatile LONG g_crashLogs = 0;

static void addr_desc(Buf& b, uint64_t a) {
    uint64_t gb = (uint64_t)g_base, ge = gb + g_gameSize;
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
        bool inGame = v >= (uint64_t)g_textBase && v < (uint64_t)g_textBase + g_textSize;
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
    lang_update();
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
    uint8_t* refundBuild = find_hookable("RefundBuild", SIG_REFUND_BUILD, 5);
    uint8_t* blit = find_unique("BlitCheck", SIG_BLIT_CHECK);
    uint8_t* popUndo = find_hookable("PopUndo", SIG_POP_UNDO, 5);
    uint8_t* hudUpd = find_hookable("HudUpdate", SIG_HUD_UPDATE, 5);
    uint8_t* uiSnd = find_unique("UiSound", SIG_UI_SOUND);
    g_materialItem = (MaterialItemFn)matItem;
    g_remove = nullptr;
    g_bounds = (BoundsFn)bounds;
    // both call-site patterns must live in the same function, close together
    g_cursorLayoutOk = cc && pc && pc > cc && pc - cc < 0x400;
    // the cursor read is the E8 inside the signature; the hook tells worlds apart by
    // rbp+0x50, which the blueprint system tests (cmp qword [rbp+0x50],0) before ServerCheck
    static const uint8_t kUndoTest[5] = { 0x48, 0x83, 0x7D, 0x50, 0x00 };
    if (g_cursorLayoutOk && sc && sc - 0x6b > cc && !memcmp(sc - 0x6b, kUndoTest, 5)) {
        uint8_t* site = cc + 15;
        g_cursorGetOrig = (CursorGetFn)(site + 5 + *(int32_t*)(site + 1));
        if (hook_calls((uint8_t*)g_cursorGetOrig, (void*)&cursor_get_hook, &site, 1) != 1) g_cursorGetOrig = nullptr;
        if (!g_cursorGetOrig) LOG(.str("cursor read hook not installed (aim only from clicks)"));
    }
    // the server/client branch must sit directly before the placement call
    g_worldLayoutOk = g_cursorLayoutOk && sc && sc + 81 == pc;
    if (!place || !bounds || !remove || !pay || !pcommit || !rcommit || !matItem || !refundBuild || !blit || !g_worldLayoutOk) {
        LOG(.str("required code not found; mod disabled (game version not supported, or another mod changed this code). place=").i(place != nullptr)
            .str(" bounds=").i(bounds != nullptr).str(" remove=").i(remove != nullptr).str(" pay=").i(pay != nullptr)
            .str(" layout=").i(g_worldLayoutOk));
        flash(T(S_UNSUPPORTED));
        return 0;
    }
    {
        const char* names[4] = { "PayPlace", "PayRemove", "PayRemovePrefab", "PayUndo" };
        const char* sigs[4] = { SIG_PAY_PLACE, SIG_PAY_REMOVE, SIG_PAY_REMOVE_PREFAB, SIG_PAY_UNDO };
        int ok = 0;
        for (int k = 0; k < 4; k++) {
            uint8_t* at = find_unique(names[k], sigs[k]);
            // must be one of the redirected PayCost calls
            if (at && at[0] == 0xE8 && at + 5 + *(int32_t*)(at + 1) == pay) { g_creativeSites[k] = at + 5; ok++; }
        }
        uint8_t* mc = find_unique("MissingCall", SIG_MISSING_CALL);
        if (mc && mc[0] == 0xE8) {
            uint8_t* fn = mc + 5 + *(int32_t*)(mc + 1);
            g_missingOrig = (MissingFn)fn;
            if (hook_calls(fn, (void*)&missing_hook, &mc, 1) != 1) g_missingOrig = nullptr;
        }
        uint8_t* st = find_unique("Stocks", SIG_STOCKS);
        auto rel = [](uint8_t* at) -> uint8_t* { return at[0] == 0xE8 ? at + 5 + *(int32_t*)(at + 1) : nullptr; };
        if (st && memcmp(st + 0x867, "\x48\x69\xC8\x48\x06\x00\x00", 7) == 0 &&
            memcmp(st + 0x78f, "\x48\x8D\xBE\xF8\x2B\x00\x00", 7) == 0 &&
            st[0x194] == 0x44 && st[0x195] == 0x8B && st[0x196] == 0x05 &&
            rel(st + 0x876) && rel(st + 0x705) && rel(st + 0x721) && rel(st + 0x89d) && rel(st + 0xa76)) {
            g_stockAdd = (StockAddFn)rel(st + 0x876);
            g_itemGroups = (RegistryFn)rel(st + 0x705);
            g_iterBegin = (IterBeginFn)rel(st + 0x721);
            g_iterNext = (IterNextFn)rel(st + 0x89d);
            g_noContainer = (uint32_t*)(st + 0x194 + 7 + *(int32_t*)(st + 0x197));
            g_stocksTls = TlsAlloc();
            uint8_t* fin = rel(st + 0xa76); uint8_t* finSite = st + 0xa76;
            g_stockFinalOrig = (StockFinalFn)fin;
            g_stocksOrig = (StocksFn)st;
            if (g_stocksTls == TLS_OUT_OF_INDEXES || hook_calls(st, (void*)&stocks_hook, nullptr, 0) <= 0 ||
                hook_calls(fin, (void*)&stock_final_hook, &finSite, 1) != 1) g_stockAdd = nullptr;
        }
        uint8_t* pc = find_unique("PickCall", SIG_PICK_CALL);
        if (pc && pc[0] == 0xE8) {
            uint8_t* fn = pc + 5 + *(int32_t*)(pc + 1);
            g_pickOrig = (PickFn)fn;
            if (hook_calls(fn, (void*)&pick_hook, &pc, 1) != 1) g_pickOrig = nullptr;
        }
        LOG(.str("creative building: ").str(g_cfg.creative ? "on" : "off").str(", ").i(ok)
            .str("/4 payment calls, preview check ").str(g_missingOrig ? "yes" : "no")
            .str(", full material list ").str(g_stockAdd ? "yes" : "no").str(", empty-type pick ").str(g_pickOrig ? "yes" : "no"));
    }
    // Magic chests of all bases. The game functions stay untouched; their calls are
    // redirected. Everything is checked first and installed only together.
    {
        uint8_t* cl = find_unique("ChestList", SIG_CHEST_LIST);
        uint8_t* cs = find_unique("ClientStock", SIG_CLIENT_STOCK);
        uint8_t* ba = find_unique("BaseAt", SIG_BASE_AT);
        uint8_t* cnt = find_unique("StockCount", SIG_STOCK_COUNT);
        uint8_t* dep = find_unique("DepositChests", SIG_DEPOSIT_CHESTS);
        auto rel = [](uint8_t* at) -> uint8_t* { return at && at[0] == 0xE8 ? at + 5 + *(int32_t*)(at + 1) : nullptr; };
        // the missing-materials check's two stock counts
        uint8_t* mf = (uint8_t*)g_missingOrig;
        uint8_t* cntSites[2] = { mf ? mf + 0x137 : nullptr, mf ? mf + 0x2d7 : nullptr };
        // ChestList and ClientStock use BaseAt and read a base record as {chest ids, count}
        // at sys + 0x30 + i * 0x130; BaseAt reads the base boxes and sub-boxes
        bool layout = cl && cs && ba && cnt && rel(cl + 0x74) == ba && rel(cs + 0xef) == ba &&
                      rel(cntSites[0]) == cnt && rel(cntSites[1]) == cnt &&
                      memcmp(cl + 0x96, "\x48\x69\xC8\x30\x01\x00\x00\x48\x8B\x44\x29\x30\x48\x89\x06\x48\x8B\x44\x29\x38", 20) == 0 &&
                      memcmp(cs + 0x12a, "\x48\x69\xC8\x30\x01\x00\x00\x4C\x8B\x44\x31\x38\x4C\x8B\x54\x31\x30", 17) == 0 &&
                      memcmp(ba + 0x28, "\x4C\x8D\x82\x24\x01\x00\x00", 7) == 0 &&
                      memcmp(ba + 0xb9, "\x48\x8B\x83\x30\x13\x00\x00", 7) == 0;
        if (layout) g_chestTls = TlsAlloc();
        if (layout && g_chestTls != TLS_OUT_OF_INDEXES) {
            g_baseAt = (BaseAtFn)ba;
            // the loot deposit's call keeps the local chests; without it known, only the
            // building systems' payments get every base (crafting stays local)
            if (rel(dep) == cl) g_depositRet = dep + 5;
            g_chestListOrig = (ChestListFn)cl;
            g_clientStockOrig = (ClientStockFn)cs;
            g_stockCountOrig = (StockCountFn)cnt;
            int nList = hook_calls(cl, (void*)&chest_list_hook, nullptr, 0);
            int nStock = hook_calls(cs, (void*)&client_stock_hook, nullptr, 0);
            int nCount = hook_calls(cnt, (void*)&stock_count_hook, cntSites, 2);
            LOG(.str("magic chests of all bases: ").str(g_cfg.globalChests ? "on" : "off").str(" (calls redirected: chest list ")
                .i(nList).str(", stock ").i(nStock).str(", check ").i(nCount).str("; crafting ")
                .str(g_depositRet ? "included)" : "local, deposit call not found)"));
        } else {
            LOG(.str("magic chests of all bases: not available (game code differs)"));
        }
    }
    // PayCost is redirected at its call sites, not hooked at its entry: Creative Mode
    // scans for its first bytes and hooks it too.
    {
        int calls = hook_calls(pay, (void*)&pay_hook, nullptr, 0);
        LOG(.str("PayCost: ").i(calls).str(" calls redirected"));
        if (calls <= 0 || calls > 64) { LOG(.str("pay hook failed; mod disabled")); return 0; }
        g_payOrig = (PayFn)pay;
    }
    g_remove = (RemoveFn)hook(remove, 10, (void*)&remove_hook);
    if (!g_remove) { LOG(.str("remove hook failed; mod disabled")); return 0; }
    g_removeCommitOrig = (CommitFn)hook(rcommit, 5, (void*)&remove_commit_hook);
    if (!g_removeCommitOrig) { LOG(.str("remove commit hook failed; mod disabled")); return 0; }
    {
        uint8_t* sites[2] = { find_unique("RemoveTableDry", SIG_REMOVE_TABLE_DRY), find_unique("RemoveTableCommit", SIG_REMOVE_TABLE_COMMIT) };
        auto rel = [](uint8_t* at) -> uint8_t* { return at && at[0] == 0xE8 ? at + 5 + *(int32_t*)(at + 1) : nullptr; };
        uint8_t* fn = rel(sites[0]);
        if (fn && rel(sites[1]) == fn) {
            g_removeTableOrig = (RemoveTableFn)fn;      // set first: a redirected call needs it
            g_removeTableOk = hook_calls(fn, (void*)&remove_table_hook, sites, 2) == 2;
        }
        LOG(.str("connected delete: ").str(g_removeTableOk ? "available" : "not available"));
        // the table builder 48f070 (called at +0x144) looks up the material config (call at
        // +0xe0, id = m + 0x80 as a byte at +0xd3) and tests isRoofBlock (+0xef)
        uint8_t* tb = rel(fn ? fn + 0x144 : nullptr);
        if (tb && !memcmp(tb + 0xd3, "\x8D\x93\x80\x00\x00\x00", 6) && !memcmp(tb + 0xef, "\x40\x38\x68\x6C", 4))
            g_matConfig = (MatConfigFn)rel(tb + 0xe0);
        LOG(.str("diagonal roof fill: ").str(g_matConfig ? "available" : "not available"));
    }
    g_refundBuildOrig = (RefundBuildFn)hook(refundBuild, 5, (void*)&refund_build_hook);
    if (!g_refundBuildOrig) { LOG(.str("refund hook failed; mod disabled")); return 0; }
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
    // only the dismantle system's own call is redirected; 9bc1a0 itself is left
    // untouched (Creative Mode looks for its first bytes)
    uint8_t* sdeps = find_hookable("SystemDeps", SIG_SYS_DEPS, 5);
    uint8_t* ddeps = find_unique("DismantleDeps", SIG_DISM_DEPS);
    if (sdeps && ddeps && ddeps[0x10] == 0xE8 && ddeps + 0x15 + *(int32_t*)(ddeps + 0x11) == sdeps) {
        g_dismDepsRet = ddeps + 0x15;
        uint8_t* site = ddeps + 0x10;
        g_depsOrig = (DepsFn)sdeps;
        if (hook_calls(sdeps, (void*)&deps_hook, &site, 1) != 1) g_depsOrig = nullptr;
    }
    LOG(.str("refund pop-up: ").str(g_depsOrig ? "available" : "not available"));
    uint8_t* pcall = g_depsOrig ? find_unique("PieceCheckCall", SIG_PIECE_CHECK_CALL) : nullptr;
    if (pcall) {
        uint8_t* site = pcall + 10;
        uint8_t* target = site + 5 + *(int32_t*)(site + 1);
        g_pieceCheckOrig = (PieceCheckFn)target;
        if (hook_calls(target, (void*)piece_check_thunk, &site, 1) != 1) g_pieceCheckOrig = nullptr;
    }
    LOG(.str("building pieces as blocks: ").str(g_pieceCheckOrig ? "available" : "not available"));
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
    install_menu();
    LOG(.str("partial area actions: ").str(g_stockFinalOrig ? "available" : "not available (picker counts not found)"));
    g_placeCommitOrig = (CommitFn)hook(pcommit, 5, (void*)&place_commit_hook);
    if (!g_placeCommitOrig) { LOG(.str("commit hook failed; mod disabled")); return 0; }
    g_placeOrig = (PlaceFn)hook(place, 10, (void*)&place_hook);
    if (!g_placeOrig) { LOG(.str("hook failed; mod disabled")); return 0; }
    g_active = 1;
    LOG(.str("ready. keys: area=").i(g_cfg.keyStructure).str(" replace=").i(g_cfg.keyReplace).str(" style=").i(g_cfg.keyStyle)
        .str(" cancel=").i(g_cfg.keyCancel).str(" maxShapes=").i(g_cfg.maxShapes));
    { Buf b; b.fmt(T(g_cfg.creative ? S_READY_CREATIVE : S_READY), MOD_VERSION); flash(b.z()); }
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
    auto image_size = [](void* m) -> size_t {
        auto* dos = (IMAGE_DOS_HEADER*)m; auto* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)m + dos->e_lfanew);
        return nt->OptionalHeader.SizeOfImage;
    };
    g_selfSize = image_size(self); g_gameSize = image_size(g_base);
    AddVectoredExceptionHandler(0, crash_handler);
    CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, input_thread, nullptr, 0, nullptr);
    if (g_cfg.overlay) CreateThread(nullptr, 0, overlay_thread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, wheel_thread, nullptr, 0, nullptr);
    return TRUE;
}
