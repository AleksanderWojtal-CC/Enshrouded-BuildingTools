#version 450
// The blocks the action will touch: one small cube per kept cell of the area grid
// (instance = cell); only faces between a kept block and a left-out cell are drawn, so
// the blocks read as one solid. Everything else collapses off screen.
layout(push_constant) uniform Push { mat4 mvp; vec4 size; vec4 eye; vec4 capA; vec4 capB; vec4 cells; } pc;
// cells: xyz = cell size in metres, w = fill mode (0 solid, 1 hollow, 2 walls, 3 gable roof, 4 hip roof)
// + 8 * action + 32 when only cells that already hold blocks count (remove, replace overlap): one bit per
// cell in occ + 64 when a roof's slopes rise along x (else along z)
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
// roofs: the same rule as core::roof_keep (src/core.h)
int roofTop(int d, int dmax, int ny) { return dmax <= 0 ? ny - 1 : (2 * d * (ny - 1) + dmax) / (2 * dmax); }
bool roofKept(ivec3 c, ivec3 n, int fill, bool acrossX) {
    int dx = min(c.x, n.x - 1 - c.x), dz = min(c.z, n.z - 1 - c.z);
    int d = fill == 4 ? min(dx, dz) : acrossX ? dx : dz;
    int dmax = ((fill == 4 ? min(n.x, n.z) : acrossX ? n.x : n.z) - 1) / 2;
    int hi = roofTop(d, dmax, n.y), lo = d > 0 ? roofTop(d - 1, dmax, n.y) + 1 : 0;
    lo = min(lo, hi);
    return c.y >= lo && c.y <= hi;
}
bool kept(ivec3 c, ivec3 n, int fill, bool onlyFull, bool acrossX) {
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, n))) return false;
    if (onlyFull) {
        int id = c.x + n.x * (c.y + n.y * c.z);
        if ((occ.bits[id >> 5] & (1u << (id & 31))) == 0u) return false;
    }
    if (fill == 3 || fill == 4) return roofKept(c, n, fill, acrossX);
    bool ex = c.x == 0 || c.x == n.x - 1, ey = c.y == 0 || c.y == n.y - 1, ez = c.z == 0 || c.z == n.z - 1;
    return fill == 1 ? (ex || ey || ez) : fill == 2 ? (ex || ez) : true;
}
void main() {
    ivec3 n = max(ivec3(round(pc.size.xyz / pc.cells.xyz)), ivec3(1));
    int id = gl_InstanceIndex;
    ivec3 c = ivec3(id % n.x, (id / n.x) % n.y, id / (n.x * n.y));
    int fill = int(pc.cells.w) & 7;
    bool onlyFull = (int(pc.cells.w) & 32) != 0;
    bool acrossX = (int(pc.cells.w) & 64) != 0;
    vec3 nrm = kNormal[gl_VertexIndex / 6];
    // only faces on the surface of the kept blocks: the blocks merge into one solid shape
    bool show = kept(c, n, fill, onlyFull, acrossX) && !kept(c + ivec3(nrm), n, fill, onlyFull, acrossX);
    vec3 p = vec3(kCorner[kTri[gl_VertexIndex]]);
    vec3 q = (vec3(c) + p) / vec3(n);
    vLocal = q; vCell = p; vNormal = nrm;
    gl_Position = show ? pc.mvp * vec4(q, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);
    vClip = gl_Position;
}
