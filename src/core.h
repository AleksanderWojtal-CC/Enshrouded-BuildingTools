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

// Fill shape of an area: solid, hollow (walls + floor + ceiling) or walls only.
// A side that is only one or two shapes thick has no inside, so floors and walls
// stay complete in every mode.
enum { FILL_SOLID = 0, FILL_HOLLOW = 1, FILL_WALLS = 2, FILL_COUNT = 3 };
inline bool fill_keep(const Plan& p, int i, int j, int k, int mode) {
    bool ex = i == 0 || i == p.count[0] - 1, ez = k == 0 || k == p.count[2] - 1;
    bool ey = j == 0 || j == p.count[1] - 1;
    if (mode == FILL_HOLLOW) return ex || ey || ez;
    if (mode == FILL_WALLS) return ex || ez;
    return true;
}
inline long fill_count(const Plan& p, int mode) {
    long cx = p.count[0], cy = p.count[1], cz = p.count[2];
    long ix = cx > 2 ? cx - 2 : 0, iy = cy > 2 ? cy - 2 : 0, iz = cz > 2 ? cz - 2 : 0;
    if (mode == FILL_HOLLOW) return cx * cy * cz - ix * iy * iz;
    if (mode == FILL_WALLS) return cx * cy * cz - ix * cy * iz;
    return cx * cy * cz;
}

} // namespace core
