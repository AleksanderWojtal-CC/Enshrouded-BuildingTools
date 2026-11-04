#version 450
// Object placement path: a soft white line with a dark edge, rings in the game's gold over the spots.
layout(location = 0) in vec2 vUv;
layout(location = 1) flat in int vKind;
layout(location = 0) out vec4 outColor;
void main() {
    if (vKind == 0) {
        float e = abs(vUv.y);
        float a = 1.0 - smoothstep(0.75, 1.0, e);
        vec3 col = mix(vec3(0.95, 0.97, 1.0), vec3(0.08, 0.1, 0.14), smoothstep(0.45, 0.75, e));
        outColor = vec4(col, 0.85 * a);
    } else {
        float r = length(vUv);
        if (r > 1.0) discard;
        float ring = smoothstep(0.5, 0.62, r) * (1.0 - smoothstep(0.86, 1.0, r));
        float dot_ = 1.0 - smoothstep(0.18, 0.28, r);
        vec3 col = vec3(1.0, 0.80, 0.35);
        float a = max(ring, dot_);
        if (a < 0.01) discard;
        outColor = vec4(mix(vec3(0.1), col, max(ring, dot_)), 0.9 * a);
    }
}
