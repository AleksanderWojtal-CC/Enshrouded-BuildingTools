// Blueprint objects (issue #49 stage 2), DIAGNOSTIC in 0.32.0-dev7: lists the props (entities)
// in a saved blueprint's box in BuildingTools.log; nothing is saved or placed yet.
//
// The engine finds entities in an area with asynchronous range queries (docs/reverse-
// engineering-notes.md, "Blueprints stage 2 research"): 9600c0 queues one (box, component type
// hash = the low 32 bits of the type's reflection hash, i.e. FNV-1a of "keen::ecs::<Name>"),
// the query system resolves it, and 9b83d0 hands back up to 256 entity ids a frame or more later.
// Queries must be written from inside a system: the mod takes over the one call of
// `comfort_energy_gathering` (server) that reads its own query's results, and from there writes
// its queries (the system's writer [[sys]+8], the entity it is processing), reads their results
// on later runs and frees them (960390). Each entity's components are read from its record
// (+0x18 archetype: bit mask, +0x84 offsets, +0xa84 strides; +0x20 data; +0x30 row);
// CurrentTransform through the comfort system's own handle at its rbp+0x60.

static const char* SIG_BP_COMFORT_READ = "4C 8B 45 90 48 8D 95 A0 00 00 00 49 8B CF 45 8B 00 E8 ?? ?? ?? ?? 84 C0";        // call at +0x11 (9b83d0)
static const char* SIG_BP_QUERY_CREATE = "48 8B CB 48 89 7C 24 20 E8 ?? ?? ?? ?? 48 8B 4D B8 8B 9D 90 03 00 00 48 85 C9";   // call at +8 (9600c0)
static const char* SIG_BP_ENTITY_GET   = "42 8B 1C 2B 8B D3 E8 ?? ?? ?? ?? 48 8B F0 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 55 48"; // call at +6 (9b25f0)
static const char* SIG_BP_QUERY_FREE   = "44 8B C3 48 8B CF 8B 10 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B 4D B8";        // call at +8 (960390)

struct BpQResult { uint8_t* entries; uint64_t count; };     // 16-byte entries, entity id first
typedef uint8_t  (*BpQResultsFn)(void* sys, BpQResult* out, uint32_t id);
typedef uint32_t* (*BpQCreateFn)(void* writer, uint32_t* outId, uint32_t entity, const float* aabb, const void* filter, uint32_t comp, uint8_t flag);
typedef uint8_t  (*BpQFreeFn)(void* writer, uint32_t entity, uint32_t id);
typedef uint8_t* (*BpEntityFn)(void* sys, uint32_t id);
static BpQResultsFn g_bpQResults = nullptr;
static BpQCreateFn  g_bpQCreate = nullptr;
static BpQFreeFn    g_bpQFree = nullptr;
static BpEntityFn   g_bpEntity = nullptr;

static uint32_t fnv1a32(const char* s) { uint32_t h = 0x811c9dc5u; for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x01000193u; } return h; }
// Only component types the query system has registered can be asked for: an unknown hash
// crashes it (0.32.0-dev7: null lookup at a290d5). These are queried by the game itself:
// ComfortProvider (comfort_energy_gathering), InteractionOfferRuntime (interactions: doors,
// chests, beds, stations), Ladder.
static const char* const kBpProbeTypes[] = {
    "keen::ecs::InteractionOfferRuntime", "keen::ecs::ComfortProvider", "keen::ecs::Ladder",
};
static const int kBpProbeN = sizeof kBpProbeTypes / sizeof kBpProbeTypes[0];
alignas(16) static float g_bpProbeBox[8];            // min xyz, 0, max xyz, 0 (metres)
alignas(16) static const uint8_t kBpProbeFilter[0x18] = { 1 };   // as the comfort system's (0x14 bytes used)
static volatile LONG g_bpProbeReq = 0;
static uint32_t g_bpProbeIds[kBpProbeN]; static int g_bpProbeLive = 0;
static uint32_t g_bpProbeOwner = 0; static DWORD g_bpProbeTick = 0;
static volatile LONG g_bpProbeHits = 0;

static uint32_t bp_sys_entity(uint8_t* sys) {          // 9bf9e0: the entity the system is processing
    uint32_t idx = *(uint32_t*)(sys + 8);
    if (!idx) return 0;
    uint8_t* base = *(uint8_t**)sys;
    if (!readable(base + 0x830, 8)) return 0;
    uint8_t* arr = *(uint8_t**)(base + 0x830);
    if (!arr || !readable(arr + (size_t)(idx - 1) * 16, 4)) return 0;
    return *(uint32_t*)(arr + (size_t)(idx - 1) * 16);
}
// inline component data of entity record `e` for component index k (null if absent)
static uint8_t* bp_comp(uint8_t* e, uint32_t k, uint32_t* size) {
    if (!e || !readable(e, 0x38) || k >= 640) return nullptr;
    uint8_t* arch = *(uint8_t**)(e + 0x18);
    if (!arch || !readable(arch, 0xa84 + 640 * 2)) return nullptr;
    if (!((*(uint64_t*)(arch + (k >> 6) * 8) >> (k & 63)) & 1)) return nullptr;
    uint32_t stride = *(uint16_t*)(arch + 0xa84 + k * 2), off = *(uint16_t*)(arch + 0x84 + k * 2);
    uint8_t* data = *(uint8_t**)(e + 0x20);
    uint32_t row = *(uint32_t*)(e + 0x30);
    if (size) *size = stride;
    if (!data) return nullptr;
    uint8_t* p = data + off + (size_t)row * stride;
    return readable(p, stride ? stride : 8) ? p : nullptr;
}
static void bp_probe_dump(uint8_t* sys, uint8_t* frame, int q, const BpQResult& r) {
    LOG(.str("objects probe: ").str(kBpProbeTypes[q]).str(" -> ").u(r.count).str(" entities"));
    uint16_t* trH = frame && readable(frame + 0x60, 8) ? *(uint16_t**)(frame + 0x60) : nullptr;
    const uint32_t trIdx = trH && readable(trH, 2) ? *trH : 0xffff;
    for (uint64_t n = 0; n < r.count && n < 256; n++) {
        uint8_t* en = r.entries + n * 16;
        if (!readable(en, 16)) break;
        uint32_t id = *(uint32_t*)en;
        uint8_t* e = g_bpEntity(sys, id);
        Buf b; b.str("  entity ").x(id).str(" rec ").x((uint64_t)e);
        uint32_t sz = 0;
        if (uint8_t* t = bp_comp(e, trIdx, &sz)) {
            const int64_t* pos = (const int64_t*)t; const float* rot = (const float*)(t + 0x18);
            b.str(" pos (").fix(pos[0]).c(',').fix(pos[1]).c(',').fix(pos[2]).str(") rot (").f(rot[0]).c(',').f(rot[1]).c(',').f(rot[2]).c(',').f(rot[3]).c(')');
        }
        log_line(b);
        if (q > 1 || n >= 24 || !e) continue;             // the components of the first two queries' entities
        for (uint32_t k = 0; k < 640; k++) {
            uint8_t* c = bp_comp(e, k, &sz);
            if (!c) continue;
            Buf cb; cb.str("    comp ").u(k).str(" size ").u(sz).str(k == trIdx ? " (CurrentTransform)" : "").str(": ");
            for (uint32_t i = 0; i < sz && i < 0x30; i++) cb.u(c[i], 16, 2);
            log_line(cb);
        }
    }
}
// the comfort system's results call: the game's own read, then the mod's probe step
static uint8_t bt_comfort_read_body(uint8_t* sys, BpQResult* out, uint32_t id, uint8_t* frame) {
    uint8_t r = g_bpQResults(sys, out, id);
    if (!InterlockedExchange(&g_bpProbeHits, 1)) LOG(.str("objects probe: comfort system seen (sys ").x((uint64_t)sys).c(')'));
    if (!sys || !readable(sys, 0x10) || !readable(*(uint8_t**)sys, 0x838)) return r;
    void* writer = *(void**)(*(uint8_t**)sys + 8);
    if (InterlockedExchange(&g_bpProbeReq, 0) && writer) {
        if (g_bpProbeLive) for (int q = 0; q < kBpProbeN; q++) if (g_bpProbeIds[q]) g_bpQFree(writer, g_bpProbeOwner, g_bpProbeIds[q]);
        g_bpProbeOwner = bp_sys_entity(sys);
        for (int q = 0; q < kBpProbeN; q++) {
            g_bpProbeIds[q] = 0;
            g_bpQCreate(writer, &g_bpProbeIds[q], g_bpProbeOwner, g_bpProbeBox, kBpProbeFilter, fnv1a32(kBpProbeTypes[q]), 0);
        }
        g_bpProbeLive = 1; g_bpProbeTick = GetTickCount();
        LOG(.str("objects probe: ").i(kBpProbeN).str(" queries written, owner ").x(g_bpProbeOwner).str(", first id ").u(g_bpProbeIds[0]));
        return r;
    }
    if (!g_bpProbeLive || GetTickCount() - g_bpProbeTick < 150) return r;
    bool all = true;
    for (int q = 0; q < kBpProbeN; q++) {
        if (!g_bpProbeIds[q]) continue;
        BpQResult res = {};
        if (g_bpQResults(sys, &res, g_bpProbeIds[q])) bp_probe_dump(sys, frame, q, res);
        else { all = false; continue; }
        g_bpQFree(writer, g_bpProbeOwner, g_bpProbeIds[q]);
        g_bpProbeIds[q] = 0;
    }
    if (all || GetTickCount() - g_bpProbeTick > 4000) {
        if (!all) { LOG(.str("objects probe: some queries gave no results in 4 s"));
                    for (int q = 0; q < kBpProbeN; q++) if (g_bpProbeIds[q]) { g_bpQFree(writer, g_bpProbeOwner, g_bpProbeIds[q]); g_bpProbeIds[q] = 0; } }
        g_bpProbeLive = 0;
        LOG(.str("objects probe: done"));
    }
    return r;
}
extern "C" uint8_t bt_comfort_read(uint8_t* sys, BpQResult* out, uint32_t id, void*, uint8_t* frame) {
    return bt_comfort_read_body(sys, out, id, frame);
}
// rcx, rdx, r8, r9 pass through; the caller's rbp becomes the 5th argument
extern "C" uint8_t bp_comfort_thunk[];
asm(".text\n"
    ".globl bp_comfort_thunk\n"
    "bp_comfort_thunk:\n"
    "    push %rbp\n"
    "    sub $0x30, %rsp\n"
    "    mov %rbp, 0x20(%rsp)\n"
    "    call bt_comfort_read\n"
    "    add $0x30, %rsp\n"
    "    pop %rbp\n"
    "    ret\n");

// the save box asks for a probe (server thread, bp_save_box)
static void bp_probe_request(const I3* c, const float* step) {
    if (!g_bpQResults) { LOG(.str("objects probe: not available (signatures)")); return; }
    for (int a = 0; a < 3; a++) {
        const int64_t v0 = a == 0 ? c[0].x : a == 1 ? c[0].y : c[0].z, v1 = a == 0 ? c[1].x : a == 1 ? c[1].y : c[1].z;
        double lo = (double)(v0 < v1 ? v0 : v1) / core::kFix - step[a] * 0.5, hi = (double)(v0 < v1 ? v1 : v0) / core::kFix + step[a] * 0.5;
        g_bpProbeBox[a] = (float)lo; g_bpProbeBox[4 + a] = (float)hi;
    }
    g_bpProbeBox[3] = g_bpProbeBox[7] = 0.f;
    LOG(.str("objects probe requested: box (").f(g_bpProbeBox[0]).c(',').f(g_bpProbeBox[1]).c(',').f(g_bpProbeBox[2]).str(") - (")
        .f(g_bpProbeBox[4]).c(',').f(g_bpProbeBox[5]).c(',').f(g_bpProbeBox[6]).str("); comfort system seen: ").i(g_bpProbeHits));
    g_bpProbeReq = 1;
}
static void bp_objects_install() {
    uint8_t* rd = find_unique("BpComfortRead", SIG_BP_COMFORT_READ);
    uint8_t* cr = find_unique("BpQueryCreate", SIG_BP_QUERY_CREATE);
    uint8_t* eg = find_unique("BpEntityGet", SIG_BP_ENTITY_GET);
    uint8_t* fr = find_unique("BpQueryFree", SIG_BP_QUERY_FREE);
    if (!rd || !cr || !eg || !fr) { LOG(.str("blueprint objects probe: not available")); return; }
    auto tgt = [](uint8_t* call) { return call + 5 + *(int32_t*)(call + 1); };
    uint8_t* site = rd + 0x11;
    g_bpQCreate = (BpQCreateFn)tgt(cr + 8);
    g_bpEntity = (BpEntityFn)tgt(eg + 6);
    g_bpQFree = (BpQFreeFn)tgt(fr + 8);
    g_bpQResults = (BpQResultsFn)tgt(site);
    if (hook_calls((uint8_t*)g_bpQResults, (void*)bp_comfort_thunk, &site, 1) != 1) { g_bpQResults = nullptr; LOG(.str("blueprint objects probe: hook failed")); return; }
    LOG(.str("blueprint objects probe: installed (diagnostic)"));
}
