#version 450
/* Untextured fragment: emit the interpolated vertex colour. Mask bit (alpha)
 * is carried so the stencil-mirror pass reads it; for opaque draws alpha=1. */
layout(location = 0) noperspective in vec4 v_col;
layout(location = 1) noperspective in float v_vz;
layout(location = 0) out vec4 o_col;
/* The VR depth plane; without that attachment (no VR) the write is dropped. */
layout(location = 1) out vec4 o_depth;
void main() {
    o_col = v_col;
    o_depth = vec4(v_vz, 0.0, 0.0, 1.0);
}
