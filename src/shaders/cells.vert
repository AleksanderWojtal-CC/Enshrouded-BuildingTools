#version 450
// The blocks the action will touch: one small cube per kept cell of the area grid
// (instance = cell); only faces between a kept block and a left-out cell are drawn, so
// the blocks read as one solid. Everything else collapses off screen.
layout(push_constant) uniform Push { mat4 mvp; vec4 size; vec4 eye; vec4 capA; vec4 capB; vec4 cells; } pc;
// cells: xyz = cell size in metres, w = fill mode (0 solid, 1 hollow, 2 walls, 3 roof) + 4 * action
// + 16 when only cells that already hold blocks count (remove, replace overlap: one bit per cell in occ)
// + 32 roof slopes rise along x + 64 shed flipped + 128 * roof shape + 1024 * (thickness - 1)
// + 4096 * roof overhang + 16384 * roof base (shapes below the area): see core::plan_area
layout(set = 1, binding = 0) readonly buffer Occ { uint bits[]; } occ;
layout(location = 0) out vec3 vLocal;      // box units
layout(location = 1) out vec4 vClip;
layout(location = 2) out vec3 vCell;       // position inside the cell, 0..1
layout(location = 3) flat out vec3 vNormal;
const ivec3 kCorner[8] = ivec3[8](ivec3(0,0,0), ivec3(1,0,0), ivec3(1,1,0), ivec3(0,1,0),
                                  ivec3(0,0,1), ivec3(1,0,1), ivec3(1,1,1), ivec3(0,1,1));
const int kTri[36] = int[36](0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,
                             3,2,6, 3,6,7,  0,3,7, 0,7,4,  1,5,6, 1,6,2);
const vec3 kNormal[6] = vec3[6](vec3(0,0,-1), vec3(0,0,1), vec3(0,-1,0), vec3(0,1,0), vec3(-1,0,0), vec3(1,0,0));
// roofs: the same rule as core::roof_column (src/core.h)
int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int roofProfile(int shape, int d, int D, int h) {
    if (D <= 0) return d >= 0 ? h : h + d;
    d = min(d, D);
    int num = d, den = 1;
    if (shape == 2) { num = min(3 * d, D + d); den = 2; }
    if (shape == 3) { num = min(8 * d, 3 * D + 2 * d); den = 5; }
    return floorDiv(2 * h * num + den * D, 2 * den * D);
}
bool roofKept(ivec3 c, ivec3 n, int w) {
    bool axisX = (w & 32) != 0, flip = (w & 64) != 0;
    int shape = (w >> 7) & 7, thick = ((w >> 10) & 3) + 1, o = (w >> 12) & 3, base = (w >> 14) & 63;
    int ox = n.x - 2 * o, oz = n.z - 2 * o;
    int dx = min(c.x, n.x - 1 - c.x) - o, dz = min(c.z, n.z - 1 - c.z) - o;
    int d, D;
    if (shape == 1 || shape == 3) { d = min(dx, dz); D = (min(ox, oz) - 1) / 2; }
    else if (shape == 4) { int cc = axisX ? c.x : c.z, nn = axisX ? n.x : n.z; d = (flip ? nn - 1 - cc : cc) - o; D = (axisX ? ox : oz) - 1; }
    else { d = axisX ? dx : dz; D = ((axisX ? ox : oz) - 1) / 2; }
    int h = n.y - base - 1;
    int top = roofProfile(shape, d, D, h);
    int bot = d > -o ? roofProfile(shape, d - 1, D, h) + 1 : (o > 0 ? top : 0);
    bot = min(bot, top);
    int lo = max(bot - (thick - 1) + base, 0), hi = min(top + base, n.y - 1);
    return c.y >= lo && c.y <= hi;
}
bool kept(ivec3 c, ivec3 n, int w, bool onlyFull) {
    int fill = w & 3;
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, n))) return false;
    if (onlyFull) {
        int id = c.x + n.x * (c.y + n.y * c.z);
        if ((occ.bits[id >> 5] & (1u << (id & 31))) == 0u) return false;
    }
    if (fill == 3) return roofKept(c, n, w);
    int t = ((w >> 10) & 3) + 1;
    bool ex = c.x < t || c.x >= n.x - t, ey = c.y < t || c.y >= n.y - t, ez = c.z < t || c.z >= n.z - t;
    return fill == 1 ? (ex || ey || ez) : fill == 2 ? (ex || ez) : true;
}
void main() {
    ivec3 n = max(ivec3(round(pc.size.xyz / pc.cells.xyz)), ivec3(1));
    int id = gl_InstanceIndex;
    ivec3 c = ivec3(id % n.x, (id / n.x) % n.y, id / (n.x * n.y));
    int w = int(pc.cells.w);
    bool onlyFull = (w & 16) != 0;
    vec3 nrm = kNormal[gl_VertexIndex / 6];
    // only faces on the surface of the kept blocks: the blocks merge into one solid shape
    bool show = kept(c, n, w, onlyFull) && !kept(c + ivec3(nrm), n, w, onlyFull);
    vec3 p = vec3(kCorner[kTri[gl_VertexIndex]]);
    vec3 q = (vec3(c) + p) / vec3(n);
    vLocal = q; vCell = p; vNormal = nrm;
    gl_Position = show ? pc.mvp * vec4(q, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);
    vClip = gl_Position;
}
