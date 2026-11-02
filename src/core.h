// Platform-independent core of the Enshrouded Building Tools mod.
// Compiled into the Windows DLL and into the Linux unit tests.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace core {

// ---------------------------------------------------------------- signatures
// "48 89 ?? 24" style patterns. Returns number of parsed bytes.
inline int parse_sig(const char* s, uint8_t* bytes, uint8_t* mask, int cap) {
    int n = 0;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    while (*s && n < cap) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') {
            bytes[n] = 0; mask[n] = 0; n++;
            s += (s[1] == '?') ? 2 : 1;
            continue;
        }
        int h = hex(s[0]), l = hex(s[1]);
        if (h < 0 || l < 0) return -1;
        bytes[n] = (uint8_t)(h * 16 + l); mask[n] = 1; n++;
        s += 2;
    }
    return n;
}

// Find all matches of a pattern in [base, base+len). Writes up to `cap` hits.
inline int scan(const uint8_t* base, size_t len, const char* sig, const uint8_t** hits, int cap) {
    uint8_t b[128], m[128];
    int n = parse_sig(sig, b, m, 128);
    if (n <= 0 || (size_t)n > len) return 0;
    int found = 0;
    // first fixed byte as anchor
    int a = 0;
    while (a < n && !m[a]) a++;
    for (size_t i = 0; i + n <= len; i++) {
        if (base[i + a] != b[a]) continue;
        int k = 0;
        for (; k < n; k++)
            if (m[k] && base[i + k] != b[k]) break;
        if (k == n) {
            if (found < cap) hits[found] = base + i;
            found++;
        }
    }
    return found;
}

// --------------------------------------------------------------- game layout
// World positions are signed 64-bit fixed point, 1 m == 2^32.
static const double kFix = 4294967296.0;

struct I3 { int64_t x, y, z; };

// Cursor transform as stored in the ServerCursor component (0x38 bytes).
struct CursorXf {
    int64_t pos[3];      // +0x00
    float   rot[4];      // +0x18 quaternion
    float   scale;       // +0x28
    uint8_t rest[0x0c];  // +0x2c
};
static_assert(sizeof(CursorXf) == 0x38, "cursor transform size");

// Transform the shape add/remove functions take (built by the game from the
// shape's bounding-box centre).
struct IntXf {
    int64_t pos[3];      // +0x00
    float   rot[4];      // +0x18
    float   scale[3];    // +0x28  always 1,1,1
    uint32_t pad;        // +0x34
};
static_assert(sizeof(IntXf) == 0x38, "int transform size");

struct Aabb { float mn[3]; float pad0; float mx[3]; float pad1; };
static_assert(sizeof(Aabb) == 0x20, "aabb size");

// Same float arithmetic the game uses to turn a bounding box into an
// integer centre: cvttss2si(((max + min) * 0.5) * 2^32 + 0.5).
inline int64_t centre_fixed(float mn, float mx) {
    volatile float s = mx + mn;
    volatile float h = s * 0.5f;
    volatile float f = h * 4294967296.0f;
    volatile float r = f + 0.5f;
    return (int64_t)r;
}

// --------------------------------------------------------------- fill planner
// Plans a structure fill: the axis-aligned box spanned by `n` corner positions
// (n = 2 or 3), stepping by the shape size on each axis. Corner positions are
// already snapped by the game, so spans are multiples of the step; any rounding
// is absorbed by interpolating between the exact corner coordinates.
// Shapes are laid side by side: exactly one shape size apart, starting at corner 1
// and running toward the farthest other corner on each axis. The number of shapes
// is the corner-to-corner distance in shape sizes, rounded (so the last shape sits
// as close to the far corner as the grid allows).
struct Plan {
    int64_t origin[3];  // corner 1 (centre of the first shape)
    int64_t stepFix[3]; // signed spacing per axis (fixed point)
    int     count[3];   // shapes per axis (>= 1)
    long    total;
    int     roofAcrossX; // roof fills: the slopes rise along x (ridge along z), else along z
};

inline int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

inline bool plan_fill(const I3* c, int n, const float step_m[3], long max_total, Plan* out) {
    if (n < 1) return false;
    const int64_t o[3] = { c[0].x, c[0].y, c[0].z };
    int64_t farOff[3] = { 0, 0, 0 };          // signed offset of the farthest corner
    for (int i = 1; i < n; i++) {
        const int64_t v[3] = { c[i].x - o[0], c[i].y - o[1], c[i].z - o[2] };
        for (int a = 0; a < 3; a++) if (iabs64(v[a]) > iabs64(farOff[a])) farOff[a] = v[a];
    }
    int64_t total = 1;
    for (int a = 0; a < 3; a++) {
        double step = (double)step_m[a];
        if (step < 0.05) step = 0.05;
        int64_t sf = (int64_t)(step * kFix + 0.5);
        double k = (double)iabs64(farOff[a]) / (double)sf;
        long steps = (long)(k + 0.5);
        if (steps > 100000) return false;
        out->origin[a] = o[a];
        out->stepFix[a] = farOff[a] < 0 ? -sf : sf;
        out->count[a] = (int)steps + 1;
        total *= out->count[a];
        if (total > (int64_t)max_total) { out->total = total > 0x7fffffff ? 0x7fffffff : (long)total; return false; }
    }
    out->total = (long)total;
    return true;
}

// Axis lock for structure corners. Corners 1 and 2 are free. Axes on which they
// differ are "used"; the third corner may only extend along the unused axes, so
//   1->2 along one axis  (line)  : corner 3 opens it into a plane or box,
//   1->2 in a plane      (plane) : corner 3 only adds height/thickness,
//   1->2 differ on all 3 (box)   : corner 3 is ignored.
// `tol` is how close (fixed point) two coordinates must be to count as equal.
inline I3 lock_third(const I3& a, const I3& b, const I3& c, int64_t tol) {
    const int64_t av[3] = { a.x, a.y, a.z }, bv[3] = { b.x, b.y, b.z }, cv[3] = { c.x, c.y, c.z };
    int64_t r[3];
    for (int k = 0; k < 3; k++) {
        bool used = iabs64(bv[k] - av[k]) > tol;
        r[k] = used ? bv[k] : cv[k];
    }
    return I3{ r[0], r[1], r[2] };
}

// Position of shape (i,j,k) of a plan.
inline I3 plan_pos(const Plan& p, int i, int j, int k) {
    return I3{ p.origin[0] + p.stepFix[0] * i, p.origin[1] + p.stepFix[1] * j, p.origin[2] + p.stepFix[2] * k };
}

// Fill shape of an area: solid, hollow (walls + floor + ceiling), walls only, or a roof.
// A side that is only one or two shapes thick has no inside, so floors and walls
// stay complete in every mode.
enum { FILL_SOLID = 0, FILL_HOLLOW = 1, FILL_WALLS = 2, FILL_GABLE = 3, FILL_HIP = 4, FILL_COUNT = 5 };
inline bool fill_roof(int mode) { return mode == FILL_GABLE || mode == FILL_HIP; }

// Roofs. Gable = two slopes meeting at a ridge, hip = four slopes (a pyramid on a square).
// Over a flat area the roof climbs one shape up for every shape in from the edge (45
// degrees, the steps of the game's roof blocks) and the area grows upward to fit it. A 3D
// area keeps its height: the slopes rise from the bottom edge to the top at the ridge.
// A column at distance d from the eave holds the shapes from just above the previous
// column's top to its own top, so every step touches the next one (diagonally on a 45
// degree slope, like the game's roof blocks; straight up on steeper ones).
// x, y, z: cell in the area (y counted up from the bottom), n*: cells per axis.
inline int roof_top(int d, int dmax, int ny) {
    if (dmax <= 0) return ny - 1;
    return (2 * d * (ny - 1) + dmax) / (2 * dmax);
}
inline int roof_dmax(int nx, int nz, int mode, bool acrossX) {
    int m = mode == FILL_HIP ? (nx < nz ? nx : nz) : acrossX ? nx : nz;
    return (m - 1) / 2;
}
inline bool roof_keep(int x, int y, int z, int nx, int ny, int nz, int mode, bool acrossX) {
    int dx = x < nx - 1 - x ? x : nx - 1 - x, dz = z < nz - 1 - z ? z : nz - 1 - z;
    int d = mode == FILL_HIP ? (dx < dz ? dx : dz) : acrossX ? dx : dz;
    int dmax = roof_dmax(nx, nz, mode, acrossX);
    int hi = roof_top(d, dmax, ny), lo = d > 0 ? roof_top(d - 1, dmax, ny) + 1 : 0;
    if (lo > hi) lo = hi;
    return y >= lo && y <= hi;
}
inline bool fill_keep(const Plan& p, int i, int j, int k, int mode) {
    if (fill_roof(mode)) {
        int y = p.stepFix[1] >= 0 ? j : p.count[1] - 1 - j;
        return roof_keep(i, y, k, p.count[0], p.count[1], p.count[2], mode, p.roofAcrossX != 0);
    }
    bool ex = i == 0 || i == p.count[0] - 1, ez = k == 0 || k == p.count[2] - 1;
    bool ey = j == 0 || j == p.count[1] - 1;
    if (mode == FILL_HOLLOW) return ex || ey || ez;
    if (mode == FILL_WALLS) return ex || ez;
    return true;
}
inline long fill_count(const Plan& p, int mode) {
    long cx = p.count[0], cy = p.count[1], cz = p.count[2];
    if (fill_roof(mode)) {
        // every column holds at least one shape: a huge area is over any limit anyway
        if (cx * cz > 1000000) return cx * cz;
        bool ax = p.roofAcrossX != 0;
        int dmax = roof_dmax((int)cx, (int)cz, mode, ax);
        long n = 0;
        for (int x = 0; x < cx; x++)
            for (int z = 0; z < cz; z++) {
                int dx = x < cx - 1 - x ? x : (int)cx - 1 - x, dz = z < cz - 1 - z ? z : (int)cz - 1 - z;
                int d = mode == FILL_HIP ? (dx < dz ? dx : dz) : ax ? dx : dz;
                int hi = roof_top(d, dmax, (int)cy), lo = d > 0 ? roof_top(d - 1, dmax, (int)cy) + 1 : 0;
                n += lo > hi ? 1 : hi - lo + 1;
            }
        return n;
    }
    long ix = cx > 2 ? cx - 2 : 0, iy = cy > 2 ? cy - 2 : 0, iz = cz > 2 ? cz - 2 : 0;
    if (mode == FILL_HOLLOW) return cx * cy * cz - ix * iy * iz;
    if (mode == FILL_WALLS) return cx * cy * cz - ix * cy * iz;
    return cx * cy * cz;
}

// plan_fill plus the fill mode: a roof's ridge runs along the longer side of the area
// (`rot` odd: along the shorter side), and a flat area grows upward to the roof's height.
inline bool plan_area(const I3* c, int n, const float step_m[3], long max_total, int mode, int rot, Plan* out) {
    out->roofAcrossX = 0;
    if (!plan_fill(c, n, step_m, max_total, out)) return false;
    if (!fill_roof(mode)) return true;
    bool acrossX = out->count[0] < out->count[2];
    if (rot & 1) acrossX = !acrossX;
    out->roofAcrossX = acrossX ? 1 : 0;
    if (out->count[1] == 1) {
        int h = roof_dmax(out->count[0], out->count[2], mode, acrossX) + 1;
        int64_t total = (int64_t)out->count[0] * h * out->count[2];
        out->count[1] = h;
        out->total = total > 0x7fffffff ? 0x7fffffff : (long)total;
        if (total > (int64_t)max_total) return false;
    }
    return true;
}

// ------------------------------------------------------ client -> server state
// On a dedicated server the mod's settings live on each player's client. The client
// sends them in the lowest 16 bits of the camera position it sends to the server
// every frame (ClientPlayerInputData.camera.position, int64 fixed point, so the
// camera moves by less than 0.02 mm): x = state bits 0-15, y = bits 16-31,
// z = a check of the state. Bits 0-7 are a fixed tag, so a camera position without
// mod data (a client without the mod) is rejected (1 in 2^24 passes by chance).
enum : uint32_t {
    NET_TAG = 0xB7, NET_TAG_MASK = 0xff,
    NET_ON = 1u << 8,            // the mod is on (not the vanilla switch)
    NET_AREA = 1u << 9,          // Area mode
    NET_REPLACE = 1u << 10,      // Replace on
    NET_STYLE_SHIFT = 11,        // 2 bits: replace style (1 overlap, 2 full, 3 connected)
    NET_FILL_SHIFT = 13,         // 2 bits: area fill (FILL_*)
    NET_CORNERS3 = 1u << 15,     // 3 area corners (else 2)
    NET_COUNT_SHIFT = 16,        // 2 bits: corners the client holds after its latest area click
    NET_OP_REMOVE = 1u << 18,    // those corners are for a removal
    NET_SEQ_SHIFT = 19,          // 3 bits: area clicks so far (wraps), tells a new click from a repeat
    NET_VERSION_SHIFT = 22,      // 2 bits: format version
    NET_VERSION = 1,
    NET_FILL_HI = 1u << 24,      // area fill bit 2 (roofs: FILL_GABLE / FILL_HIP)
    NET_ROOF_ROT = 1u << 25,     // roof turned (ridge along the shorter side)
};
inline uint32_t net_field(uint32_t s, int shift, uint32_t bits) { return (s >> shift) & ((1u << bits) - 1); }
inline uint16_t net_check(uint32_t s) {
    uint32_t h = s * 0x9E3779B1u;
    h ^= h >> 15; h *= 0x85EBCA77u; h ^= h >> 13;
    return (uint16_t)((h ^ (h >> 16)) ^ 0x5A17u);
}
inline void net_pack(uint32_t s, int64_t pos[3]) {
    const uint64_t v[3] = { s & 0xffffu, s >> 16, net_check(s) };
    for (int a = 0; a < 3; a++) pos[a] = (int64_t)(((uint64_t)pos[a] & ~(uint64_t)0xffff) | v[a]);
}
inline bool net_unpack(const int64_t pos[3], uint32_t* s) {
    const uint32_t v = (uint32_t)((uint64_t)pos[0] & 0xffff) | (uint32_t)(((uint64_t)pos[1] & 0xffff) << 16);
    if ((v & NET_TAG_MASK) != NET_TAG || net_field(v, NET_VERSION_SHIFT, 2) != NET_VERSION) return false;
    if (((uint64_t)pos[2] & 0xffff) != net_check(v)) return false;
    *s = v;
    return true;
}

// Server side of an area click. The client counts its own corner clicks and sends,
// with every frame, how many corners it holds after its latest area click (`count`,
// 0 right after the final click) and a click counter (`seq`). The server keeps its
// own corners (positions from the click it processes) and uses these to stay in step:
//   - the same seq as the last click it handled: the game repeated the click, ignore it;
//   - otherwise the corner this click sets is the one before `count` (the last one when
//     count is 0); when the server holds a different number of corners (it missed a
//     click, or the client counted one the server refused), it starts over with this
//     click as corner 1.
// Returns the corner index (0-based) the click sets, or -1 to ignore the click.
inline int net_corner_index(int serverCount, int lastSeq, int seq, int count, int corners) {
    if (lastSeq == seq) return -1;
    int expect = count == 0 ? corners - 1 : count - 1;
    if (expect < 0 || expect >= corners) return 0;
    return serverCount == expect ? expect : 0;
}

} // namespace core
