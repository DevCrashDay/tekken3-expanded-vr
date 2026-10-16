#version 450
/* Untextured geometry (flat / gouraud triangles, lines, flat rects).
 * Position is in PSX VRAM pixel space (0..1024, 0..512); map to NDC for the
 * 1024x512 VRAM image (resolution-independent, so the SxS supersample is just
 * a larger framebuffer). VRAM y increases downward; Vulkan clip y also points
 * down, so VRAM y=0 lands at image row 0 (top) with no flip.
 *
 * u_shift = half an HR pixel (0.5/S in native units): the rasterizer samples
 * coverage at pixel centers, the PS1 DDA at integer coords. The shift aligns
 * the two grids (matches the GL backend's GEO_VS). u_xoff/u_xhalf carry the
 * native-wide x translation / clip half-extent (0 / 512 canonical; Phase 4). */
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec4 in_col;
/* VR depth plane (PSX_VR): inverse depth 4096/SZ from the GTE projection,
 * 0 for 2D prims. Linear in screen space, so noperspective is exact. */
layout(location = 2) in float in_vz;
layout(push_constant) uniform PC {
    float u_shift;
    float u_xoff;
    float u_xhalf;
} pc;
layout(location = 0) noperspective out vec4 v_col;
layout(location = 1) noperspective out float v_vz;
void main() {
    gl_Position = vec4((in_pos.x + pc.u_shift + pc.u_xoff) / pc.u_xhalf - 1.0,
                       (in_pos.y + pc.u_shift) / 256.0 - 1.0, 0.0, 1.0);
    v_col = in_col;
    v_vz = in_vz;
}
