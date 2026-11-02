#version 450
// Surface of the affected blocks (shaded, block seams as faint lines), faces turned towards the camera; colour by
// action (build blue, replace amber, remove red-purple); behind the world dimmer.
layout(push_constant) uniform Push { mat4 mvp; vec4 size; vec4 eye; vec4 capA; vec4 capB; vec4 cells; } pc;
layout(location = 0) in vec3 vLocal;
layout(location = 1) in vec4 vClip;
layout(location = 2) in vec3 vCell;
layout(location = 3) flat in vec3 vNormal;
layout(location = 0) out vec4 outColor;
#ifdef DEPTH
layout(set = 0, binding = 0) uniform sampler2D sceneDepth;
#endif
void main() {
    vec3 toEye = (pc.eye.xyz - vLocal) * pc.size.xyz;
    if (dot(vNormal, toEye) <= 0.0) discard;                // back faces: only the near side of each block
    float mode = pc.eye.w >= 3.5 ? pc.eye.w - 4.0 : pc.eye.w;
    float k = 1.0;
#ifdef DEPTH
    if (mode > 0.5 && vClip.w > 0.0) {
        vec2 ndc = vClip.xy / vClip.w;
        float d = texture(sceneDepth, ndc * 0.5 + 0.5).r;
        const float n = 0.01, fz = 10000.0;
        float sceneZ = mode < 1.5 ? n * fz / max(d * (fz - n) + n, 1e-7) : n * fz / max(fz - d * (fz - n), 1e-7);
        if (sceneZ < vClip.w - 0.3 - vClip.w * 0.006) k = 0.6;   // blocks sit on the surfaces they replace: generous margin, no flicker
    }
#endif
    // edges of this block's face, in pixels
    vec3 b = min(vCell, 1.0 - vCell);
    int f = abs(vNormal.x) > 0.5 ? 0 : abs(vNormal.y) > 0.5 ? 1 : 2;
    int a = (f + 1) % 3, c = (f + 2) % 3;
    vec2 uv = vec2(b[a], b[c]);
    vec2 px = max(fwidth(vec2(vCell[a], vCell[c])), vec2(1e-5));
    float e = min(uv.x / px.x, uv.y / px.y);
    float edge = (1.0 - smoothstep(0.5, 1.5, e)) * clamp(1.0 - max(px.x, px.y) * 4.0, 0.0, 1.0);   // block seams fade far away
    int act = (int(pc.cells.w) >> 3) & 3;
    vec3 col = act == 1 ? vec3(1.0, 0.62, 0.12) : act == 2 ? vec3(1.0, 0.22, 0.42) : vec3(0.45, 0.75, 1.0);
    // simple light from above so the solid reads in 3D: tops bright, sides medium, bottoms dark
    float shade = vNormal.y > 0.5 ? 1.0 : vNormal.y < -0.5 ? 0.6 : (abs(vNormal.x) > 0.5 ? 0.85 : 0.75);
    float alpha = k * (0.30 + 0.08 * shade + 0.30 * edge);   // sides almost as strong as tops
    col *= 0.7 + 0.3 * shade;
    col = mix(vec3(dot(col, vec3(0.299, 0.587, 0.114))), col, 0.8);   // 20% less saturated
    outColor = vec4(col, clamp(alpha, 0.0, 1.0));
}
