#version 450
// The object placement path (see src/objects.h): a ribbon a few pixels wide along the path's
// points and a ring over each object's spot, always facing the screen. Points are camera
// relative metres in the shared buffer (after the block bits): path first, then the spots.
// Instance < cells.x - 1: segment between point i and i + 1; else: marker (instance - (cells.x - 1)).
layout(push_constant) uniform Push { mat4 mvp; vec4 size; vec4 eye; vec4 capA; vec4 capB; vec4 cells; } pc;
// cells: x = path points, y = markers, z, w = screen width, height in pixels; size.x = half width (px),
// size.y = marker radius (px), size.z = first float of the path in the buffer
layout(set = 1, binding = 0) readonly buffer Pts { float v[]; } pts;
layout(location = 0) out vec2 vUv;
layout(location = 1) flat out int vKind;
vec3 pt(int i) { int o = int(pc.size.z) + 3 * i; return vec3(pts.v[o], pts.v[o + 1], pts.v[o + 2]); }
const vec2 kQuad[6] = vec2[6](vec2(0, -1), vec2(1, -1), vec2(1, 1), vec2(0, -1), vec2(1, 1), vec2(0, 1));
void main() {
    int np = int(pc.cells.x), seg = max(np - 1, 0);
    vec2 q = kQuad[gl_VertexIndex];
    vec2 half_px = 2.0 / pc.cells.zw;                 // one pixel in NDC
    const float nearW = 0.06;
    if (gl_InstanceIndex < seg) {
        vec4 a = pc.mvp * vec4(pt(gl_InstanceIndex), 1.0);
        vec4 b = pc.mvp * vec4(pt(gl_InstanceIndex + 1), 1.0);
        if (a.w < nearW && b.w < nearW) { gl_Position = vec4(0, 0, -1, 1); vKind = 0; vUv = vec2(0); return; }
        if (a.w < nearW) a = mix(a, b, (nearW - a.w) / (b.w - a.w));
        if (b.w < nearW) b = mix(b, a, (nearW - b.w) / (a.w - b.w));
        vec2 sa = a.xy / a.w, sb = b.xy / b.w;
        vec2 d = (sb - sa) * pc.cells.zw;
        float len = length(d);
        vec2 dir = len > 1e-4 ? d / len : vec2(1, 0);
        vec2 nrm = vec2(-dir.y, dir.x) * pc.size.x * half_px;
        vec2 ext = dir * pc.size.x * 0.5 * half_px;   // a little past each end so the segments join
        vec4 c = q.x < 0.5 ? a : b;
        vec2 s = (q.x < 0.5 ? sa - ext : sb + ext) + nrm * q.y;
        gl_Position = vec4(s * c.w, c.z, c.w);
        vUv = vec2(q.x, q.y);
        vKind = 0;
    } else {
        int m = gl_InstanceIndex - seg;
        vec4 c = pc.mvp * vec4(pt(np + m), 1.0);
        if (c.w < nearW) { gl_Position = vec4(0, 0, -1, 1); vKind = 1; vUv = vec2(0); return; }
        vec2 corner = vec2(q.x * 2.0 - 1.0, q.y);
        vec2 s = c.xy / c.w + corner * pc.size.y * half_px;
        gl_Position = vec4(s * c.w, c.z, c.w);
        vUv = corner;
        vKind = 1;
    }
}
