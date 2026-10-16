#version 450
/* VR stereo pass (PSX_VR): one eye's view of the displayed frame, from its
 * colour and the depth plane the geometry passes wrote (inverse depth
 * 4096/SZ per pixel from the GTE projection, 0 where a 2D prim drew).
 *
 * A pixel with disparity s (a fraction of the picture width; positive = in
 * front of the screen plane) moves by -eye * s / 2: the left eye (eye -1)
 * sees near things further right, the right eye further left. The pass maps
 * backwards: for the output pixel u it walks candidate disparities from the
 * nearest to the farthest and takes the first source pixel whose own
 * disparity reaches the candidate — the front-most surface that lands here,
 * so near objects occlude far ones. Where the crossing is a jump in depth
 * (an object's edge, not a surface) nothing in the picture lands on the
 * pixel: the gap the near object opens takes the far side's colour. */
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D u_color;   /* linear */
layout(set = 0, binding = 1) uniform sampler2D u_depth;   /* nearest */
layout(push_constant) uniform PC {
    vec4  rect;        /* the displayed frame in the source image (normalized x0,y0,x1,y1) */
    vec4  clampr;      /* colour sampling clamp (normalized, half a texel inside rect) */
    float eye;         /* -1 left, +1 right, 0 = the flat picture */
    float strength;    /* the far plane's disparity, fraction of the width */
    float ref;         /* inverse depth of the screen plane */
    float flat_s;      /* disparity of pixels without depth */
    int   steps;       /* candidates per pixel */
    int   has_depth;   /* 0 = no depth plane for this frame (FMV): flat */
} pc;
layout(location = 0) out vec4 o_col;

vec2 src_at(float u) {
    return vec2(mix(pc.rect.x, pc.rect.z, u), mix(pc.rect.y, pc.rect.w, v_uv.y));
}
float disparity_at(float u) {
    float d = texture(u_depth, src_at(u)).r;
    if (d <= 0.0) return pc.flat_s;
    return clamp((d / pc.ref - 1.0) * pc.strength, -pc.strength, 2.0 * pc.strength);
}
vec3 color_at(float u) {
    vec2 t = clamp(src_at(u), pc.clampr.xy, pc.clampr.zw);
    return texture(u_color, t).rgb;
}

void main() {
    float u = v_uv.x;
    if (pc.eye == 0.0 || pc.strength <= 0.0 || pc.has_depth == 0 || pc.ref <= 0.0) {
        o_col = vec4(color_at(u), 1.0);
        return;
    }
    float s_near = 2.0 * pc.strength;
    float s_far = min(-pc.strength, pc.flat_s);
    s_near = max(s_near, pc.flat_s);
    int n = max(pc.steps, 2);
    float step_s = (s_near - s_far) / float(n - 1);
    float half_eye = 0.5 * pc.eye;
    float prev_u = u + half_eye * s_near;
    float prev_g = disparity_at(clamp(prev_u, 0.0, 1.0)) - s_near;
    float src = prev_u;
    if (prev_g < 0.0) {
        src = u + half_eye * s_far;
        for (int i = 1; i < n; i++) {
            float sc = s_near - step_s * float(i);
            float uc = u + half_eye * sc;
            float g = disparity_at(clamp(uc, 0.0, 1.0)) - sc;
            if (g >= 0.0) {
                /* Disparity rose by more than the candidates moved: an edge. */
                float jump = (g + sc) - (prev_g + sc + step_s);
                if (jump > 1.5 * step_s) {
                    /* A texel and a half clear of the edge, so the linear
                     * colour fetch does not pick up the near object. */
                    float texel = 1.0 / (float(textureSize(u_color, 0).x) * (pc.rect.z - pc.rect.x));
                    src = prev_u + sign(prev_u - uc) * 1.5 * texel;
                } else {   /* a surface crossing between the last two candidates */
                    float t = prev_g / (prev_g - g);
                    src = mix(prev_u, uc, t);
                }
                break;
            }
            prev_u = uc;
            prev_g = g;
        }
    }
    o_col = vec4(color_at(clamp(src, 0.0, 1.0)), 1.0);
}
