#version 450
// Unit cube (0..1) stretched onto the area box by the model-view-projection matrix.
layout(push_constant) uniform Push { mat4 mvp; vec4 size; vec4 eye; vec4 capA; vec4 capB; } pc;
layout(location = 0) out vec3 vLocal;
layout(location = 1) out vec4 vClip;     // clip position: screen position and view depth (w, metres) per pixel
const ivec3 kCorner[8] = ivec3[8](ivec3(0,0,0), ivec3(1,0,0), ivec3(1,1,0), ivec3(0,1,0),
                                  ivec3(0,0,1), ivec3(1,0,1), ivec3(1,1,1), ivec3(0,1,1));
const int kTri[36] = int[36](0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,
                             3,2,6, 3,6,7,  0,3,7, 0,7,4,  1,5,6, 1,6,2);
void main() {
    vec3 p = vec3(kCorner[kTri[gl_VertexIndex]]);
    vLocal = p;
    gl_Position = pc.mvp * vec4(p, 1.0);
    vClip = gl_Position;
}
