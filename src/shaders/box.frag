#version 450
// Building-ghost look: faint pulsing fill, a 0.5 m grid and bright soft edges, all
// sized in screen pixels so they stay crisp near and far. Faces turned away from the
// camera (the far side of the box) are dimmer and their edges dashed, so it reads
// which edge is in front. Built twice: with DEPTH the game's depth image is read too,
// and parts behind the world (not the player's character) are drawn like the far side.
// The box turns red while the character stands in it.
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 size;    // xyz: box size in metres, w: seconds
    vec4 eye;     // xyz: camera in box units, w: depth mode (1 reversed, 2 normal) + 4 when the character is inside
    vec4 capA;    // xyz: character capsule bottom in view space, w: 1 / projection x scale
    vec4 capB;    // xyz: capsule top, w: 1 / projection y scale
} pc;
layout(location = 0) in vec3 vLocal;
layout(location = 1) in vec4 vClip;
layout(location = 0) out vec4 outColor;
#ifdef DEPTH
layout(set = 0, binding = 0) uniform sampler2D sceneDepth;
#endif
void main() {
    float flags = pc.eye.w;
    bool red = flags >= 3.5;
    float mode = red ? flags - 4.0 : flags;
    vec3 m = vLocal * pc.size.xyz;                         // metres from the low corner
    vec3 border = min(vLocal, 1.0 - vLocal);
    int f = border.x < border.y ? (border.x < border.z ? 0 : 2) : (border.y < border.z ? 1 : 2);   // face axis
    int a = (f + 1) % 3, b = (f + 2) % 3;
    bool high = vLocal[f] > 0.5;
    bool front = high ? pc.eye[f] > 1.0 : pc.eye[f] < 0.0;
#ifdef DEPTH
    if (mode > 0.5 && vClip.w > 0.0) {
        vec2 ndc = vClip.xy / vClip.w;
        float d = texture(sceneDepth, ndc * 0.5 + 0.5).r;
        const float n = 0.01, fz = 10000.0;
        float sceneZ = mode < 1.5 ? n * fz / max(d * (fz - n) + n, 1e-7)      // reversed depth (1 near, 0 far)
                                  : n * fz / max(fz - d * (fz - n), 1e-7);   // normal depth
        float viewZ = vClip.w;
        if (sceneZ < viewZ - 0.15 - viewZ * 0.005) {       // something in the world is in front
            vec3 sp = vec3(ndc.x * sceneZ * pc.capA.w, -ndc.y * sceneZ * pc.capB.w, sceneZ);
            vec3 ab = pc.capB.xyz - pc.capA.xyz;
            float t = clamp(dot(sp - pc.capA.xyz, ab) / max(dot(ab, ab), 1e-4), 0.0, 1.0);
            bool character = length(sp - (pc.capA.xyz + ab * t)) < 0.75;
            if (!character) front = false;
        }
    }
#endif
    vec2 mm = vec2(m[a], m[b]);
    vec2 px = max(fwidth(mm), vec2(1e-5));                 // metres per pixel
    vec2 toEdge = vec2(border[a] * pc.size[a], border[b] * pc.size[b]) / px;
    float e = min(toEdge.x, toEdge.y);                     // pixels to the box edge
    vec2 g2 = abs(fract(mm * 2.0 + 0.5) - 0.5) * 0.5 / px; // pixels to the 0.5 m grid
    float g = min(g2.x, g2.y);
    float pulse = 0.85 + 0.15 * sin(pc.size.w * (red ? 6.0 : 3.0));
    float edge = 1.0 - smoothstep(front ? 1.2 : 0.5, front ? 2.8 : 1.5, e);
    if (!front) {                                          // dashed far / hidden edges, 0.25 m dashes
        float along = toEdge.x < toEdge.y ? mm.y : mm.x;
        edge *= step(0.5, fract(along * 2.0));
    }
    float glow = exp(-e / 14.0);
    float grid = (1.0 - smoothstep(0.4, 1.4, g)) * clamp(1.0 - px.x * 40.0, 0.0, 1.0);   // fades out far away
    float k = front ? 1.0 : 0.4;
    vec3 base = red ? vec3(1.0, 0.32, 0.25) : vec3(0.40, 0.72, 1.0);
    vec3 hi = red ? vec3(1.0, 0.85, 0.8) : vec3(0.85, 0.95, 1.0);
    float alpha = k * ((red ? 0.10 : 0.06) * pulse + 0.18 * glow + 0.22 * grid) + (front ? 0.9 : 0.5) * edge;
    vec3 col = mix(base, hi, edge * (front ? 0.7 : 0.2));
    col = mix(vec3(dot(col, vec3(0.299, 0.587, 0.114))), col, 0.8);   // 20% less saturated
    outColor = vec4(col, clamp(alpha, 0.0, 1.0));
}
