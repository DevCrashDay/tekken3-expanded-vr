#version 450
/* VR stereo pass (PSX_VR): one full-screen triangle over an eye image; v_uv
 * runs 0..1 across the picture. See stereo.frag. */
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
