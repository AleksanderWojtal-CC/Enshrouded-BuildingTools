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
    // fill options (plan_area)
    int     thick;       // wall / room / roof thickness in shapes (1..3; 0 counts as 1)
    int     roofShape;   // ROOF_*
    int     roofAxisX;   // the slopes rise along x (ridge along z), else along z
    int     roofFlip;    // shed: rises toward the low end of the axis
    int     overhang;    // shapes the roof reaches past the area on every side
    int     roofH;       // height of the roof itself (shapes)
    int     roofBase;    // shapes of the plan below the area's bottom (overhang drop + thickness)
    int     roundShape;  // ROUND_*
    int     roundSide;   // half: flat side, quarter: centre corner, in box space (see round_inside)
    int     roundHollow; // a wall `thick` shapes thick along the curve, else solid
    int     roundAxis;   // the axis the round shape runs along (1 = y: a round in the ground plan)
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
    out->thick = 1; out->roofShape = out->roofAxisX = out->roofFlip = out->overhang = out->roofH = out->roofBase = 0;
    out->roundShape = out->roundSide = out->roundHollow = 0; out->roundAxis = 1;
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

// Fill shape of an area: solid, hollow room (walls + floor + ceiling), walls only, or a
// roof. Rooms, walls and roofs are `thick` shapes thick. A side too thin to have an inside
// stays complete.
enum { FILL_SOLID = 0, FILL_HOLLOW = 1, FILL_WALLS = 2, FILL_ROOF = 3, FILL_ROUND = 4, FILL_COUNT = 5 };
enum { ROOF_GABLE = 0, ROOF_HIP = 1, ROOF_GAMBREL = 2, ROOF_MANSARD = 3, ROOF_SHED = 4, ROOF_COUNT = 5 };
// how many ways a roof shape can be turned (gable / gambrel: ridge along either side,
// shed: rising toward each of the four sides; hip and mansard look the same every way)
inline int roof_turns(int shape) { return shape == ROOF_SHED ? 4 : (shape == ROOF_HIP || shape == ROOF_MANSARD) ? 1 : 2; }

enum { ROUND_FULL = 0, ROUND_HALF = 1, ROUND_QUARTER = 2, ROUND_COUNT = 3 };
inline int round_turns(int shape) { return shape == ROUND_FULL ? 1 : 4; }

struct FillOpts { int roofShape, roofRot, thick, overhang, roundShape = 0, roundRot = 0, roundHollow = 0; };

// Round fills: an ellipse (a circle in a square area) that fills the area's box, or half of
// one (flat side on one side of the box) or a quarter (centre on one corner of the box).
// Cell (u, v) of a nu x nv plane, counted from the box's low corner, belongs to the shape
// when its centre is inside the ellipse. Cells may lie outside the box (neighbour tests):
// past a half's flat side or a quarter's straight sides the ellipse goes on, so those sides
// are not walls. side: half 0 = flat side at v = 0, 1 at v = nv, 2 at u = 0, 3 at u = nu;
// quarter: centre corner, bit 0 = at u = nu, bit 1 = at v = nv. Same rule in cells.vert.
inline bool round_inside(int shape, int side, int u, int v, int nu, int nv) {
    float cu = (float)nu, cv = (float)nv, au = (float)nu, av = (float)nv;   // doubled units
    if (shape == ROUND_HALF) {
        if (side < 2) { av = 2.f * nv; cv = side == 1 ? 2.f * nv : 0.f; }
        else { au = 2.f * nu; cu = side == 3 ? 2.f * nu : 0.f; }
    } else if (shape == ROUND_QUARTER) {
        au = 2.f * nu; av = 2.f * nv;
        cu = (side & 1) ? 2.f * nu : 0.f; cv = (side & 2) ? 2.f * nv : 0.f;
    }
    float x = ((float)(2 * u + 1) - cu) / au, y = ((float)(2 * v + 1) - cv) / av;
    return x * x + y * y <= 1.0f;
}
// solid: every cell inside; hollow: the inside cells near a cell outside: one diagonal or
// straight step, then t - 1 straight steps (t shapes thick across a straight side, and
// where the curve runs diagonally the blocks still share faces, so the wall has no gaps)
inline bool round_keep(int shape, int side, bool hollow, int t, int u, int v, int nu, int nv) {
    if (!round_inside(shape, side, u, v, nu, nv)) return false;
    if (!hollow) return true;
    for (int du = -t; du <= t; du++)
        for (int dv = -t; dv <= t; dv++) {
            int au = du < 0 ? -du : du, av = dv < 0 ? -dv : dv;
            if ((au > 1 ? au - 1 : 0) + (av > 1 ? av - 1 : 0) > t - 1) continue;
            if (!round_inside(shape, side, u + du, v + dv, nu, nv)) return true;
        }
    return false;
}
// the plane of a round fill's plan: the ground plan (along y), unless the area is a
// standing wall (one cell thin across x or z and more than one high): then the shape
// stands in that wall
inline void round_axes(int axis, int* ua, int* va) {
    if (axis == 0) { *ua = 2; *va = 1; }
    else if (axis == 2) { *ua = 0; *va = 1; }
    else { *ua = 0; *va = 2; }
}
inline void round_plane(const Plan& p, int* ua, int* va) { round_axes(p.roundAxis, ua, va); }

inline int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }   // b > 0

// Roofs. Gable = two slopes meeting at a ridge, hip = four slopes, gambrel = a barn roof
// (two slopes, each steep below and flatter above), mansard = the same on four sides,
// shed = one slope. Over a flat area the roof is as high as it is deep (one shape up per
// shape in on average; 45 degrees for gable, hip and shed) and the plan grows upward to
// fit. A 3D area keeps its height. The surface height at distance d (in shapes) from the
// eave, with the ridge at D and the roof h shapes high; d < 0 is the overhang, which
// carries the slope on down past the area's edge.
inline int roof_profile(int shape, int d, int D, int h) {
    if (D <= 0) return d >= 0 ? h : h + d;
    if (d > D) d = D;
    int64_t num = d, den = 1;
    if (shape == ROOF_GAMBREL) { num = 3 * (int64_t)d < (int64_t)D + d ? 3 * (int64_t)d : (int64_t)D + d; den = 2; }
    if (shape == ROOF_MANSARD) { num = 8 * (int64_t)d < 3 * (int64_t)D + 2 * d ? 8 * (int64_t)d : 3 * (int64_t)D + 2 * d; den = 5; }
    return (int)floor_div(2 * (int64_t)h * num + den * D, 2 * den * D);
}
// distance from the eave and ridge distance of roof column (x, z) of a plan (n* = plan
// counts including the overhang `o`)
inline void roof_dist(int shape, bool axisX, bool flip, int o, int x, int z, int nx, int nz, int* d, int* D) {
    int ox = nx - 2 * o, oz = nz - 2 * o;
    int dx = (x < nx - 1 - x ? x : nx - 1 - x) - o, dz = (z < nz - 1 - z ? z : nz - 1 - z) - o;
    if (shape == ROOF_HIP || shape == ROOF_MANSARD) {
        *d = dx < dz ? dx : dz; *D = ((ox < oz ? ox : oz) - 1) / 2;
    } else if (shape == ROOF_SHED) {
        int c = axisX ? x : z, n = axisX ? nx : nz;
        *d = (flip ? n - 1 - c : c) - o; *D = (axisX ? ox : oz) - 1;
    } else {
        *d = axisX ? dx : dz; *D = ((axisX ? ox : oz) - 1) / 2;
    }
}
// Shapes of roof column (x, z), counted up from the plan's bottom: from just above the
// previous column's top (so every step touches the next one) up to its own top, made
// `thick` shapes deeper.
inline void roof_column(const Plan& p, int x, int z, int* lo, int* hi) {
    int d, D;
    roof_dist(p.roofShape, p.roofAxisX != 0, p.roofFlip != 0, p.overhang, x, z, p.count[0], p.count[2], &d, &D);
    int h = p.roofH - 1;
    int top = roof_profile(p.roofShape, d, D, h);
    int bot = d > -p.overhang ? roof_profile(p.roofShape, d - 1, D, h) + 1 : (p.overhang ? top : 0);
    if (bot > top) bot = top;
    int t = p.thick < 1 ? 1 : p.thick;
    *lo = bot - (t - 1) + p.roofBase; *hi = top + p.roofBase;
    if (*lo < 0) *lo = 0;
    if (*hi > p.count[1] - 1) *hi = p.count[1] - 1;
}
inline bool fill_keep(const Plan& p, int i, int j, int k, int mode) {
    if (mode == FILL_ROUND) {
        int c[3] = { i, j, k }, ua, va;
        for (int a = 0; a < 3; a++) if (p.stepFix[a] < 0) c[a] = p.count[a] - 1 - c[a];   // box space
        round_plane(p, &ua, &va);
        return round_keep(p.roundShape, p.roundSide, p.roundHollow != 0, p.thick < 1 ? 1 : p.thick,
                          c[ua], c[va], p.count[ua], p.count[va]);
    }
    if (mode == FILL_ROOF) {
        int y = p.stepFix[1] >= 0 ? j : p.count[1] - 1 - j, lo, hi;
        roof_column(p, i, k, &lo, &hi);
        return y >= lo && y <= hi;
    }
    int t = p.thick < 1 ? 1 : p.thick;
    bool ex = i < t || i >= p.count[0] - t, ez = k < t || k >= p.count[2] - t;
    bool ey = j < t || j >= p.count[1] - t;
    if (mode == FILL_HOLLOW) return ex || ey || ez;
    if (mode == FILL_WALLS) return ex || ez;
    return true;
}
inline long fill_count(const Plan& p, int mode) {
    long cx = p.count[0], cy = p.count[1], cz = p.count[2];
    if (mode == FILL_ROUND) {
        int ua, va; round_plane(p, &ua, &va);
        const int wa = 3 - ua - va;
        long nu = p.count[ua], nv = p.count[va], n = 0;
        if (nu * nv > 1000000) return cx * cy * cz;
        for (int u = 0; u < nu; u++)
            for (int v = 0; v < nv; v++)
                n += round_keep(p.roundShape, p.roundSide, p.roundHollow != 0, p.thick < 1 ? 1 : p.thick, u, v, (int)nu, (int)nv);
        return n * p.count[wa];
    }
    if (mode == FILL_ROOF) {
        // every column holds at least one shape: a huge area is over any limit anyway
        if (cx * cz > 1000000) return cx * cz;
        long n = 0;
        for (int x = 0; x < cx; x++)
            for (int z = 0; z < cz; z++) { int lo, hi; roof_column(p, x, z, &lo, &hi); if (hi >= lo) n += hi - lo + 1; }
        return n;
    }
    long t = p.thick < 1 ? 1 : p.thick;
    long ix = cx > 2 * t ? cx - 2 * t : 0, iy = cy > 2 * t ? cy - 2 * t : 0, iz = cz > 2 * t ? cz - 2 * t : 0;
    if (mode == FILL_HOLLOW) return cx * cy * cz - ix * iy * iz;
    if (mode == FILL_WALLS) return cx * cy * cz - ix * cy * iz;
    return cx * cy * cz;
}

// plan_fill plus the fill options. A roof's slopes rise across the shorter side of the
// area (turned: the longer side; a shed's turns go round all four sides). A flat area grows
// upward to the roof's height; the overhang widens the plan on every side and the roof's
// drop over it (and its thickness) extend it downward.
inline bool plan_area(const I3* c, int n, const float step_m[3], long max_total, int mode, const FillOpts& fo, Plan* out) {
    if (!plan_fill(c, n, step_m, max_total, out)) return false;
    out->thick = fo.thick < 1 ? 1 : fo.thick > 3 ? 3 : fo.thick;
    out->roofShape = out->roofAxisX = out->roofFlip = out->overhang = out->roofH = out->roofBase = 0;
    if (mode == FILL_ROUND) {
        // the plane: the ground plan, or a standing wall the area is one cell thin across
        int axis = 1;
        if (out->count[1] > 1 && out->count[0] == 1 && out->count[2] > 1) axis = 0;
        else if (out->count[1] > 1 && out->count[2] == 1 && out->count[0] > 1) axis = 2;
        out->roundAxis = axis;
        const int shape = fo.roundShape >= 0 && fo.roundShape < ROUND_COUNT ? fo.roundShape : ROUND_FULL;
        const int rot = fo.roundRot & 3;
        out->roundShape = shape; out->roundHollow = fo.roundHollow != 0;
        int ua, va; round_plane(*out, &ua, &va);
        // turns count from corner 1 (plan index 0; the height counts from the bottom)
        const bool mu = out->stepFix[ua] < 0, mv = va != 1 && out->stepFix[va] < 0;
        int side = 0;
        if (shape == ROUND_HALF) {
            // the flat side on the longer side of the box first (a circle in a 2:1 area),
            // on corner 1's side, then the opposite side, then the two shorter sides
            static const int longU[4] = { 0, 1, 2, 3 }, longV[4] = { 2, 3, 0, 1 };
            side = out->count[ua] >= out->count[va] ? longU[rot] : longV[rot];
            if (side < 2 && mv) side ^= 1;
            if (side >= 2 && mu) side ^= 1;
        } else if (shape == ROUND_QUARTER) {
            // the curve runs from corner 1 to corner 2 (bending one way, then the other),
            // then the centre on corner 1, then on corner 2
            static const int centre[4] = { 2, 1, 0, 3 };
            side = centre[rot] ^ (mu ? 1 : 0) ^ (mv ? 2 : 0);
        }
        out->roundSide = side;
        return true;
    }
    if (mode != FILL_ROOF) return true;
    const int shape = fo.roofShape >= 0 && fo.roofShape < ROOF_COUNT ? fo.roofShape : ROOF_GABLE;
    const int o = fo.overhang < 0 ? 0 : fo.overhang > 3 ? 3 : fo.overhang;
    const int rot = fo.roofRot & 3;
    bool axisX = out->count[0] < out->count[2];
    if (rot & 1) axisX = !axisX;
    out->roofShape = shape; out->roofAxisX = axisX; out->roofFlip = shape == ROOF_SHED && (rot & 2); out->overhang = o;
    int d, D;
    roof_dist(shape, axisX, false, 0, 0, 0, out->count[0], out->count[2], &d, &D);
    const int H = out->count[1] == 1 ? D + 1 : out->count[1];
    out->roofH = H;
    const int drop = -roof_profile(shape, -o, D, H - 1);
    out->roofBase = (drop > 0 ? drop : 0) + out->thick - 1;
    for (int a = 0; a < 3; a += 2) { out->origin[a] -= out->stepFix[a] * o; out->count[a] += 2 * o; }
    if (out->stepFix[1] >= 0) out->origin[1] -= out->stepFix[1] * out->roofBase;
    out->count[1] = H + out->roofBase;
    int64_t total = (int64_t)out->count[0] * out->count[1] * out->count[2];
    out->total = total > 0x7fffffff ? 0x7fffffff : (long)total;
    return total <= (int64_t)max_total;
}

// Roof shape + turn in 4 bits (the client sends its settings in few bits)
inline int roof_pose_pack(int shape, int rot) {
    static const int first[ROOF_COUNT] = { 0, 2, 3, 5, 6 };
    if (shape < 0 || shape >= ROOF_COUNT) shape = 0;
    return first[shape] + (rot % roof_turns(shape));
}
inline void roof_pose_unpack(int v, int* shape, int* rot) {
    static const int first[ROOF_COUNT] = { 0, 2, 3, 5, 6 };
    *shape = 0; *rot = 0;
    for (int k = ROOF_COUNT - 1; k >= 0; k--)
        if (v >= first[k]) { *shape = k; *rot = v - first[k]; if (*rot >= roof_turns(k)) *rot = 0; return; }
}
// Round shape, turn and solid / hollow in 18 values (pose: full 0, half 1-4, quarter 5-8)
inline int round_pack(int shape, int rot, int hollow) {
    if (shape < 0 || shape >= ROUND_COUNT) shape = 0;
    int pose = shape == ROUND_FULL ? 0 : shape == ROUND_HALF ? 1 + (rot & 3) : 5 + (rot & 3);
    return pose * 2 + (hollow ? 1 : 0);
}
inline void round_unpack(int v, int* shape, int* rot, int* hollow) {
    if (v < 0 || v >= 18) v = 0;
    *hollow = v & 1; int pose = v >> 1;
    *shape = pose == 0 ? ROUND_FULL : pose <= 4 ? ROUND_HALF : ROUND_QUARTER;
    *rot = pose == 0 ? 0 : (pose - 1) & 3;
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
    NET_ROOF_SHIFT = 24,         // 4 bits: roof shape and turn (roof_pose_pack)
    NET_THICK_SHIFT = 28,        // 2 bits: thickness - 1
    NET_OVERHANG_SHIFT = 30,     // 2 bits: roof overhang
};
inline uint32_t net_field(uint32_t s, int shift, uint32_t bits) { return (s >> shift) & ((1u << bits) - 1); }
// The fill and its options. The 2-bit fill field has no room for the round fill: it is sent
// as the roof fill with roof pose 10..15 (no roof uses those) and the round settings
// (round_pack) in that pose and the overhang bits.
inline uint32_t net_fill_bits(int fill, const FillOpts& fo) {
    if (fill == FILL_ROUND) {
        uint32_t v = (uint32_t)round_pack(fo.roundShape, fo.roundRot, fo.roundHollow);
        return ((uint32_t)FILL_ROOF << NET_FILL_SHIFT) | ((10 + v / 4) << NET_ROOF_SHIFT) | ((v % 4) << NET_OVERHANG_SHIFT);
    }
    return (((uint32_t)fill & 3) << NET_FILL_SHIFT) | ((uint32_t)roof_pose_pack(fo.roofShape, fo.roofRot) << NET_ROOF_SHIFT) |
           (((uint32_t)fo.overhang & 3) << NET_OVERHANG_SHIFT);
}
// the fill (FILL_*) and its options from a state; thickness is not touched
inline int net_fill_unpack(uint32_t s, FillOpts* fo) {
    int fill = (int)net_field(s, NET_FILL_SHIFT, 2);
    int pose = (int)net_field(s, NET_ROOF_SHIFT, 4), oh = (int)net_field(s, NET_OVERHANG_SHIFT, 2);
    if (fill == FILL_ROOF && pose >= 10) {
        round_unpack((pose - 10) * 4 + oh, &fo->roundShape, &fo->roundRot, &fo->roundHollow);
        return FILL_ROUND;
    }
    roof_pose_unpack(pose, &fo->roofShape, &fo->roofRot);
    fo->overhang = oh;
    return fill;
}
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
