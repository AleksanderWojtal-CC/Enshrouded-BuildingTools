// Blueprint objects (issue #49 stage 2): the props in a saved box (doors, chests, beds,
// stations, furniture, lights, ladders) with their item, position and rotation, and placing
// them again when the blueprint is built.
//
// Finding them (docs/reverse-engineering-notes.md, "Blueprints stage 2 research"): the engine
// finds entities in an area with asynchronous range queries. 9600c0 queues one (box, component
// type hash = the low 32 bits of the type's reflection hash, i.e. FNV-1a of "keen::ecs::<Name>";
// only types the query system registered: an unknown one crashes it), the query system resolves
// it, and 9b83d0 hands back up to 256 entity ids a frame or more later. Queries must be written
// from inside a system: the mod takes over the one call of `comfort_energy_gathering` (server)
// that reads its own query's results, and from there writes its queries (the system's writer
// [[sys]+8], the entity it is processing), reads their results on later runs and frees them
// (960390). An entity's components are read from its record (+0x18 archetype: bit mask, +0x84
// offsets, +0xa84 strides; +0x20 data; +0x30 row): CurrentTransform through the comfort system's
// own handle (its rbp+0x60), the item from the 4-byte component that holds the item id (the
// one the game's item lookup e94470 knows; index 610 in 1.0.1.2).
//
// Placing them: PlaceProp (objects.h's g_placePropOrig) with the blueprint system's building
// context (the same struct the prop system passes: item registry +0x28, undo +0xe8 / +0xf0).

static const char* SIG_BP_COMFORT_READ = "4C 8B 45 90 48 8D 95 A0 00 00 00 49 8B CF 45 8B 00 E8 ?? ?? ?? ?? 84 C0";        // call at +0x11 (9b83d0)
static const char* SIG_BP_QUERY_CREATE = "48 8B CB 48 89 7C 24 20 E8 ?? ?? ?? ?? 48 8B 4D B8 8B 9D 90 03 00 00 48 85 C9";   // call at +8 (9600c0)
static const char* SIG_BP_ENTITY_GET   = "42 8B 1C 2B 8B D3 E8 ?? ?? ?? ?? 48 8B F0 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 55 48"; // call at +6 (9b25f0)
static const char* SIG_BP_QUERY_FREE   = "44 8B C3 48 8B CF 8B 10 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B 4D B8";        // call at +8 (960390)
static const char* SIG_BP_ITEM_BY_ID   = "8B 7F 38 8B CF E8 ?? ?? ?? ?? 0F 57 C0 4C 89 65 58 0F 57 C9 66 0F 7F 45 60";        // call at +5 (e94470, dismantle)

struct BpQResult { uint8_t* entries; uint64_t count; };     // 16-byte entries, entity id first
typedef uint8_t  (*BpQResultsFn)(void* sys, BpQResult* out, uint32_t id);
typedef uint32_t* (*BpQCreateFn)(void* writer, uint32_t* outId, uint32_t entity, const float* aabb, const void* filter, uint32_t comp, uint8_t flag);
typedef uint8_t  (*BpQFreeFn)(void* writer, uint32_t entity, uint32_t id);
typedef uint8_t* (*BpEntityFn)(void* sys, uint32_t id);
typedef uint8_t* (*BpItemByIdFn)(uint32_t id, void* registry);
static BpQResultsFn g_bpQResults = nullptr;
static BpQCreateFn  g_bpQCreate = nullptr;
static BpQFreeFn    g_bpQFree = nullptr;
static BpEntityFn   g_bpEntity = nullptr;
static BpItemByIdFn g_bpItemById = nullptr;
static void* volatile g_bpItemReg = nullptr;          // item registry (building context +0x28), checked at each save / build

static uint32_t fnv1a32(const char* s) { uint32_t h = 0x811c9dc5u; for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x01000193u; } return h; }
// Component types the game itself queries (so the query system has them registered).
static const char* const kBpProbeTypes[] = {
    "keen::ecs::InteractionOfferRuntime", "keen::ecs::ComfortProvider", "keen::ecs::Ladder",
};
static const int kBpProbeN = sizeof kBpProbeTypes / sizeof kBpProbeTypes[0];
alignas(16) static float g_bpProbeBox[8];            // min xyz, 0, max xyz, 0 (metres)
alignas(16) static const uint8_t kBpProbeFilter[0x18] = { 1 };   // as the comfort system's (0x14 bytes used)
static volatile LONG g_bpProbeReq = 0;
static uint32_t g_bpProbeIds[kBpProbeN]; static int g_bpProbeLive = 0;
static uint32_t g_bpProbeOwner = 0; static DWORD g_bpProbeTick = 0;
static volatile LONG g_bpProbeHits = 0;              // the comfort system was seen (its read call ran)
static volatile DWORD g_bpProbeSeen = 0;             // ... last time
static uint32_t g_bpItemComp = 0xffffffff;           // component index holding the item id (found on the first prop)
// found objects of the current probe (world positions; the save turns them into box offsets)
struct BpFound { uint32_t entity, item; int64_t pos[3]; float rot[4]; };
static BpFound g_bpFound[core::BP_MAX_OBJS]; static int g_bpFoundN = 0;
static void bp_probe_finished(bool answered);        // blueprint.h: writes the pending save

static bool prop_item(const uint8_t* it);            // objects.h
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
// The item a placed prop came from: the 4-byte component holding an item id the lookup knows.
static uint32_t bp_entity_item(uint8_t* e) {
    if (!g_bpItemById || !g_bpItemReg) return 0;
    uint32_t sz = 0;
    auto test = [&](uint32_t k) -> uint32_t {
        uint8_t* c = bp_comp(e, k, &sz);
        if (!c || sz != 4) return 0;
        uint32_t v = *(uint32_t*)c;
        if (v < 0x10000) return 0;
        uint8_t* it = g_bpItemById(v, g_bpItemReg);
        return it && prop_item(it) ? v : 0;
    };
    if (g_bpItemComp < 640) { if (uint32_t v = test(g_bpItemComp)) return v; }
    for (uint32_t k = 0; k < 640; k++)
        if (uint32_t v = test(k)) {
            if (g_bpItemComp != k) LOG(.str("blueprint objects: the item is in component ").u(k));
            g_bpItemComp = k; return v;
        }
    return 0;
}
static void bp_probe_collect(uint8_t* sys, uint8_t* frame, int q, const BpQResult& r) {
    uint16_t* trH = frame && readable(frame + 0x60, 8) ? *(uint16_t**)(frame + 0x60) : nullptr;
    const uint32_t trIdx = trH && readable(trH, 2) ? *trH : 0xffff;
    int added = 0;
    for (uint64_t n = 0; n < r.count && n < 256; n++) {
        uint8_t* en = r.entries + n * 16;
        if (!readable(en, 16)) break;
        uint32_t id = *(uint32_t*)en;
        bool dup = false;
        for (int k = 0; k < g_bpFoundN && !dup; k++) dup = g_bpFound[k].entity == id;
        if (dup || g_bpFoundN >= core::BP_MAX_OBJS) continue;
        uint8_t* e = g_bpEntity(sys, id);
        uint32_t sz = 0;
        uint8_t* t = bp_comp(e, trIdx, &sz);
        if (!t || sz < 0x28) continue;
        const int64_t* pos = (const int64_t*)t;
        bool inside = true;
        for (int a = 0; a < 3; a++) {
            float v = (float)((double)pos[a] / core::kFix);
            if (v < g_bpProbeBox[a] - 0.01f || v > g_bpProbeBox[4 + a] + 0.01f) inside = false;
        }
        uint32_t item = inside ? bp_entity_item(e) : 0;
        if (!item) continue;
        BpFound& f = g_bpFound[g_bpFoundN++];
        f.entity = id; f.item = item;
        memcpy(f.pos, pos, 24); memcpy(f.rot, t + 0x18, 16);
        added++;
        LOG(.str("  object ").x(item).str(" at (").fix(pos[0]).c(',').fix(pos[1]).c(',').fix(pos[2]).str(") rot (")
            .f(f.rot[0]).c(',').f(f.rot[1]).c(',').f(f.rot[2]).c(',').f(f.rot[3]).c(')'));
    }
    LOG(.str("blueprint objects: ").str(kBpProbeTypes[q] + 10).str(" -> ").u(r.count).str(" entities, ").i(added).str(" new objects"));
}
// the comfort system's results call: the game's own read, then the mod's queries
static uint8_t bt_comfort_read_body(uint8_t* sys, BpQResult* out, uint32_t id, uint8_t* frame) {
    uint8_t r = g_bpQResults(sys, out, id);
    g_bpProbeSeen = GetTickCount();
    if (!InterlockedExchange(&g_bpProbeHits, 1)) LOG(.str("blueprint objects: comfort system seen"));
    if (!sys || !readable(sys, 0x10) || !readable(*(uint8_t**)sys, 0x838)) return r;
    void* writer = *(void**)(*(uint8_t**)sys + 8);
    if (InterlockedExchange(&g_bpProbeReq, 0) && writer) {
        if (g_bpProbeLive) for (int q = 0; q < kBpProbeN; q++) if (g_bpProbeIds[q]) g_bpQFree(writer, g_bpProbeOwner, g_bpProbeIds[q]);
        g_bpProbeOwner = bp_sys_entity(sys);
        g_bpFoundN = 0;
        for (int q = 0; q < kBpProbeN; q++) {
            g_bpProbeIds[q] = 0;
            g_bpQCreate(writer, &g_bpProbeIds[q], g_bpProbeOwner, g_bpProbeBox, kBpProbeFilter, fnv1a32(kBpProbeTypes[q]), 0);
        }
        g_bpProbeLive = 1; g_bpProbeTick = GetTickCount();
        return r;
    }
    if (!g_bpProbeLive || GetTickCount() - g_bpProbeTick < 150) return r;
    bool all = true;
    for (int q = 0; q < kBpProbeN; q++) {
        if (!g_bpProbeIds[q]) continue;
        BpQResult res = {};
        if (g_bpQResults(sys, &res, g_bpProbeIds[q])) bp_probe_collect(sys, frame, q, res);
        else { all = false; continue; }
        g_bpQFree(writer, g_bpProbeOwner, g_bpProbeIds[q]);
        g_bpProbeIds[q] = 0;
    }
    if (all || GetTickCount() - g_bpProbeTick > 4000) {
        if (!all) for (int q = 0; q < kBpProbeN; q++) if (g_bpProbeIds[q]) { g_bpQFree(writer, g_bpProbeOwner, g_bpProbeIds[q]); g_bpProbeIds[q] = 0; }
        g_bpProbeLive = 0;
        bp_probe_finished(all);
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

// The item registry from a building context: +0x28 (the piece removal 48fab0 and PlaceProp read
// it there), checked with the item in hand (the shape state is its definition).
static bool bp_item_registry(void* ctx, void* state) {
    void* reg = ctx && readable((uint8_t*)ctx + 0x28, 8) ? *(void**)((uint8_t*)ctx + 0x28) : nullptr;
    uint8_t* self = g_bpItemById && reg && state ? g_bpItemById(*(uint32_t*)state, reg) : nullptr;
    if (self == (uint8_t*)state) { g_bpItemReg = reg; return true; }
    LOG(.str("blueprint objects: item registry not confirmed (").x((uint64_t)self).str(" for ").x((uint64_t)state).c(')'));
    return false;
}
// The save asks for the objects in its box (server thread). False = they cannot be found now
// (signatures missing, no item registry, or the comfort system is not running).
static bool bp_probe_request(const I3* c, const float* step, void* ctx, void* state) {
    if (!g_bpQResults || !bp_item_registry(ctx, state)) return false;
    if (!g_bpProbeHits || GetTickCount() - g_bpProbeSeen > 3000) { LOG(.str("blueprint objects: the comfort system is not running here")); return false; }
    for (int a = 0; a < 3; a++) {
        const int64_t v0 = a == 0 ? c[0].x : a == 1 ? c[0].y : c[0].z, v1 = a == 0 ? c[1].x : a == 1 ? c[1].y : c[1].z;
        double lo = (double)(v0 < v1 ? v0 : v1) / core::kFix - step[a] * 0.5, hi = (double)(v0 < v1 ? v1 : v0) / core::kFix + step[a] * 0.5;
        g_bpProbeBox[a] = (float)lo; g_bpProbeBox[4 + a] = (float)hi;
    }
    g_bpProbeBox[3] = g_bpProbeBox[7] = 0.f;
    LOG(.str("blueprint objects: query for (").f(g_bpProbeBox[0]).c(',').f(g_bpProbeBox[1]).c(',').f(g_bpProbeBox[2]).str(") - (")
        .f(g_bpProbeBox[4]).c(',').f(g_bpProbeBox[5]).c(',').f(g_bpProbeBox[6]).c(')'));
    g_bpProbeReq = 1;
    return true;
}

// ---- placing (server, the blueprint commit; building context of the blueprint system)
typedef uint8_t (*PlacePropFn)(void* ctx, uint8_t* info, uint32_t item);
static PlacePropFn g_placePropOrig = nullptr;        // objects.h finds it
static volatile LONG g_bpObjRecord = 0;              // obj_rec_check (objects.h): record the blueprint's objects
// Logic objects (item flag 0x80000 at +0x5d8) make PlaceProp send a LogicEntityPlacedEvent
// through the context's +0xe0, which only the prop system's context has (null in the blueprint
// system's: crash at 9c3760, 0.32.0-dev10). They are left out until the mod can give that event.
static bool bp_logic_object(void* ctx, const uint8_t* it) {
    return (*(const uint32_t*)(it + 0x5d8) & 0x80000) && !*(void**)((uint8_t*)ctx + 0xe0);
}
static bool bp_place_object(void* ctx, uint32_t item, const double* posM, const float* rot) {
    if (!g_placePropOrig || !g_bpItemById || !g_bpItemReg) return false;
    uint8_t* it = g_bpItemById(item, g_bpItemReg);
    if (!it || !prop_item(it) || bp_logic_object(ctx, it)) return false;
    alignas(16) uint8_t info[0x50] = {};
    memcpy(info, it + 0x3c0, 16);                        // the placed entity template
    for (int a = 0; a < 3; a++) *(float*)(info + 0x10 + a * 4) = (float)posM[a];
    memcpy(info + 0x1c, rot, 16);
    for (int a = 0; a < 3; a++) { *(float*)(info + 0x2c + a * 4) = -0.25f; *(float*)(info + 0x38 + a * 4) = 0.25f; }   // local bounds (the place event's volume)
    g_bpObjRecord = 1;
    uint8_t r = g_placePropOrig(ctx, info, item);
    g_bpObjRecord = 0;
    return r != 0;
}

static void bp_objects_install() {
    uint8_t* rd = find_unique("BpComfortRead", SIG_BP_COMFORT_READ);
    uint8_t* cr = find_unique("BpQueryCreate", SIG_BP_QUERY_CREATE);
    uint8_t* eg = find_unique("BpEntityGet", SIG_BP_ENTITY_GET);
    uint8_t* fr = find_unique("BpQueryFree", SIG_BP_QUERY_FREE);
    uint8_t* ib = find_unique("BpItemById", SIG_BP_ITEM_BY_ID);
    if (!rd || !cr || !eg || !fr || !ib) { LOG(.str("blueprint objects: not available (signatures)")); return; }
    auto tgt = [](uint8_t* call) { return call + 5 + *(int32_t*)(call + 1); };
    uint8_t* site = rd + 0x11;
    g_bpQCreate = (BpQCreateFn)tgt(cr + 8);
    g_bpEntity = (BpEntityFn)tgt(eg + 6);
    g_bpQFree = (BpQFreeFn)tgt(fr + 8);
    g_bpItemById = (BpItemByIdFn)tgt(ib + 5);
    g_bpQResults = (BpQResultsFn)tgt(site);
    if (hook_calls((uint8_t*)g_bpQResults, (void*)bp_comfort_thunk, &site, 1) != 1) { g_bpQResults = nullptr; LOG(.str("blueprint objects: hook failed")); return; }
    LOG(.str("blueprint objects: available"));
}
