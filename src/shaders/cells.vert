#version 450
// The blocks the action will touch: one small cube per kept cell of the area grid
// (instance = cell); only faces between a kept block and a left-out cell are drawn, so
// the blocks read as one solid. Everything else collapses off screen.
layout(push_constant) uniform Push { mat4 mvp; vec4 size; vec4 eye; vec4 capA; vec4 capB; vec4 cells; } pc;
// cells: xyz = cell size in metres, w = fill mode (0 solid, 1 hollow, 2 walls) + 4 * action + 16 when only
// cells that already hold blocks count (remove, replace overlap): one bit per cell in occ
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
bool kept(ivec3 c, ivec3 n, int fill, bool onlyFull) {
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, n))) return false;
    if (onlyFull) {
        int id = c.x + n.x * (c.y + n.y * c.z);
        if ((occ.bits[id >> 5] & (1u << (id & 31))) == 0u) return false;
    }
    bool ex = c.x == 0 || c.x == n.x - 1, ey = c.y == 0 || c.y == n.y - 1, ez = c.z == 0 || c.z == n.z - 1;
    return fill == 1 ? (ex || ey || ez) : fill == 2 ? (ex || ez) : true;
}
void main() {
    ivec3 n = max(ivec3(round(pc.size.xyz / pc.cells.xyz)), ivec3(1));
    int id = gl_InstanceIndex;
    ivec3 c = ivec3(id % n.x, (id / n.x) % n.y, id / (n.x * n.y));
    int fill = int(pc.cells.w) & 3;
    bool onlyFull = (int(pc.cells.w) & 16) != 0;
    vec3 nrm = kNormal[gl_VertexIndex / 6];
    // only faces on the surface of the kept blocks: the blocks merge into one solid shape
    bool show = kept(c, n, fill, onlyFull) && !kept(c + ivec3(nrm), n, fill, onlyFull);
    vec3 p = vec3(kCorner[kTri[gl_VertexIndex]]);
    vec3 q = (vec3(c) + p) / vec3(n);
    vLocal = q; vCell = p; vNormal = nrm;
    gl_Position = show ? pc.mvp * vec4(q, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);
    vClip = gl_Position;
}
