/* Arcade Stages (steps E4 to E6): the Tekken 3 arcade stages in place of
 * the PS1 ones, arenas 0..12, with their animations. Package
 * tekken3.stage.arcade-stages, feature "Arcade Stages"; its packs come from
 * the player's tekken3.zip at setup (tools/arcade_stages/install.py). TIPS
 * sections 92 to 95 and 106 (the Embu).
 *
 * The PS1 game is a port of the arcade one and keeps its world: same units
 * (floor cells of 2500), same camera struct (0x800A85C0 = 0x802C6BD4: pitch,
 * yaw, roll at +0, +4, +8, position at +0x14), same fight axis (0x800A9188 =
 * 0x8021FB60, both 0 at the start of a fight, both moved in the tile drawing
 * by the same routine, 0x8006E1B8 = 0x801DB238), same stage placement (the
 * tiles' origin under the fighters' midpoint, smoothed over 4 frames, turned
 * by minus the axis). Only the display differs: 368 x 480 instead of
 * 512 x 240, so the PS1 camera matrix has its own row scaling, roll sign and
 * GTE screen. The arcade stage is therefore drawn with the PS1's own
 * matrices, built as the PS1 builds them for its own stage (0x8006D014) and
 * its floor (0x80045CA8, 0x8004ADC4), with the arcade data (tiles in world
 * units / 10, stage origin y 50 as 0x8017C454) from a stageNN.ast pack
 * (tools/arcade_stages/pack.py, format 3).
 *
 * E5b, once per stage frame (tools/arcade_stages/anim.py is the reference):
 * the animated texture of stages 6 and 12 (0x801E2114: the next of 16
 * frames moved in every 4th frame), the stage 3 objects (0x801E5AD0, every
 * frame) and the stage 11 helicopter (0x801E3F68 / 0x801E40C0), started
 * again at each round as 0x801E3D94 does; the objects drawn as 0x801E2A68
 * (main ranks 1011 and 1009, stage 11: 1022), the helicopter only once the
 * round is decided, as in the arcade.
 *
 * Hooks, all inert until the package's activation found the upper VRAM half:
 *   0x80048E58 / 0x80049848  the PS1 floor (called through 0x8009E728 by
 *            0x80048240, before the fighters move): skipped, the arcade
 *            floor (0x801A941C / 0x801AA9C0, scene.floor_commands) linked
 *            at main OT ranks 1021..982 instead;
 *   0x8006D014  the PS1 tiles: run unchanged (it also counts frames,
 *            0x800A08F0, keeps the fighters' history and moves the axis),
 *            then its private OT 0x800A08C8 + 8 * buffer is emptied; the
 *            tiles the arcade's cone walk keeps (0x801DA734) in OT A (main
 *            rank 1010) and OT B (1021), the far background (0x801A7688)
 *            at rank 1023.
 *   0x80048240 / 0x80048208  the two callers of *0x8009E728: in the Embu
 *            (state 6) 0x8004808C points it at the overlay's own floor
 *            (0x800D494C, a small grid of FT4 that met the PS1 tiles only),
 *            pointed at 0x80048E58 instead so the arcade floor is drawn.
 * Any missing piece (no pack for the arena, Vulkan, a special mode) leaves
 * the frame to the PS1 code. */
#include "mod_plugins.h"
#include "psx_runtime.h"
#include "psx_sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK_FORMAT 3u
#define BUFFER_BYTES 0x30000u        /* packets of one OT buffer */
#define FLOOR_BYTES 0x8000u          /* of which the floor's */

/* PS1 addresses */
#define ARENA        0x800ADC84u
#define CAMERA       0x800A85C0u
#define AXIS         0x800A9188u
#define OT_BUFFER    0x800ADEFCu
#define PRIVATE_OT   0x800A08C8u
#define DRAWENV      0x800A8C54u
#define WORLD        0x800ADF70u
#define FLOOR_SHOWN  0x800ADFD0u
#define FLOOR_FN     0x8009E728u    /* the floor routine, set by 0x8004808C at each stage load */
#define EMBU_FLOOR   0x800D494Cu    /* ... in the Embu (state 0x800AE204 = 6): the overlay's */
#define KIND0_FLOOR  0x80048E58u
#define MODE         0x800AFA88u
#define DARK_ARENA   0x800AFAA0u
#define FIXED_STAGE  0x800AFAA1u
#define NO_STAGE     0x800ADCA4u
#define P1_X 0x800AA190u
#define P1_Z 0x800AA198u
#define P2_X 0x800ABA1Cu
#define P2_Z 0x800ABA24u
#define HISTORY_P1 0x800A08F8u       /* 4 x (x, -, z, -) */
#define HISTORY_P2 0x800A0938u
#define SIN_PS1 0x8001E8C4u
#define COS_PS1 0x8001F0C4u
#define SCREEN_W 368
#define SCREEN_H 480
#define STAGE_Y  50                  /* arcade 0x8017C454, every stage */

typedef struct { int16_t x, y, z, pad; } Vert;
typedef struct {
    uint32_t header;
    uint8_t kind, code, cull, ncol, idx[4], uv[4][2];
    uint16_t clut, tpage;
    uint8_t rgb[4][3], win[4];
} Prim;
typedef struct { uint8_t slot, layer_b, skipped, pad; uint16_t nv, np; } TileHead;
typedef struct { const TileHead *head; const Vert *v; const Prim *p; } Tile;
typedef struct { const Vert *v; const Prim *p; unsigned nv, np; } Model;
typedef struct { uint8_t model, pad; uint16_t rank, size, pad2; } Layout;   /* OBJS: drawing call order */

typedef struct {
    int stage;
    uint8_t *file;
    uint32_t size;
    Tile tiles[256];
    unsigned ntiles, tile_size;          /* tile size: world units / 10 */
    /* far background */
    int16_t fcols, frows, fring, fyoff, fdegrees, fpitch;
    const uint8_t *fcolours;            /* cell rgb, top rgb, top on, bottom rgb, bottom on */
    const uint16_t *fdesc, *fcluts;
    const uint8_t *fkinds;
    /* floor */
    int16_t lkind, lfog;
    int32_t lnear;
    const uint32_t *lcells, *lhalves;   /* (w0, w1) x 100, x 400 */
    const uint8_t *lunique;
    uint16_t lnv;
    const int16_t *lverts;              /* (x, z) x lnv */
    uint8_t lcount[100];
    const uint32_t *lquads[100];
    const uint16_t *luv[5];             /* [n]: 4 rotations x n x 4 */
    /* tables */
    const int16_t *sin, *cos, *sqrt1, *sqrt2;
    const uint8_t *fog;
    /* objects (stages 3, 11) */
    Model models[4];
    unsigned nmodels, nlayout;
    const Layout *layout;
    const uint32_t *rot;                /* 0x8021688C: sine | cosine << 16 */
    /* animated texture (stages 6, 12): frames moved onto (ax, ay) */
    uint16_t ax, ay, aw, ah, aperiod, aframes;
    uint16_t *apixels;                  /* aframes x aw x ah, from the VRAM section */
} Pack;

static int s_on;                        /* the package's activation succeeded */
static int s_shadow;                    /* probe: computed and logged, the PS1 frame untouched */
static FILE *s_probe;
static uint32_t s_memory;               /* 2 x BUFFER_BYTES of GPU-DMA memory */
static Pack s_pack;
static int s_tried = -1;                /* arena whose pack was last looked for */
static int16_t s_sin[4096], s_cos[4096];
static int s_trig_ps1;
static int s_log = -1;
static unsigned s_frames;
static unsigned s_stat_tiles, s_stat_floor, s_stat_far, s_stat_cone, s_stat_behind;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */
static int tdiv(int64_t a, int64_t b) { return (int)(a / b); }           /* C: towards zero */
static int tmod(int64_t a, int64_t b) { return (int)(a % b); }
static int pmod(int a, int b) { int r = a % b; return r < 0 ? r + b : r; }
static int s12(int a) { a &= 0xFFF; return a >= 0x800 ? a - 0x1000 : a; }
static int shift12(int64_t v) { return (int)((v < 0 ? v + 0xFFF : v) >> 12); }
static int16_t sat16(int64_t v) { return (int16_t)(v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v); }
static uint32_t xy(int x, int y) { return (uint32_t)(y & 0xFFFF) << 16 | (uint32_t)(x & 0xFFFF); }
static uint32_t rgbw(const uint8_t c[3]) { return (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16; }

static int log_on(void) {
    if (s_log < 0) s_log = getenv("TEKKEN3_ARCADE_STAGES_LOG") != NULL;
    return s_log;
}

/* TEKKEN3_ARCADE_STAGES_PERF: every 600 frames, the module's time per frame
 * (floor hook + stage frame) and the time between two stage frames */
#ifdef _WIN32
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *count);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *frequency);
static uint64_t now_ns(void) {
    static long long f;
    long long c;
    if (!f) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c * 1e9 / (double)f);
}
#else
#include <time.h>
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
#endif
static int s_perf = -1;
static uint64_t s_perf_mod, s_perf_mod_max, s_perf_last, s_perf_gap_max, s_perf_gap_sum;
static unsigned s_perf_frames, s_perf_gaps, s_perf_slow, s_perf_hitch;

static int perf_on(void) {
    if (s_perf < 0) s_perf = getenv("TEKKEN3_ARCADE_STAGES_PERF") != NULL;
    return s_perf;
}

static void perf_frame(uint64_t mod_ns) {
    uint64_t t = now_ns();
    s_perf_mod += mod_ns;
    if (mod_ns > s_perf_mod_max) s_perf_mod_max = mod_ns;
    if (s_perf_last) {
        uint64_t gap = t - s_perf_last;
        s_perf_gap_sum += gap; s_perf_gaps++;
        if (gap > s_perf_gap_max) s_perf_gap_max = gap;
        s_perf_slow += gap > 17500000u;           /* a frame missed at 60 Hz */
        s_perf_hitch += gap > 25000000u;
        if (gap > 50000000u)
            fprintf(stdout, "arcade stages perf: %.1f ms before frame %u (state %u, phase %u)\n", gap / 1e6,
                    psx_mod_read_word(0x800A08F0u), psx_mod_read_word(0x800AE204u), psx_mod_read_half(0x800AE224u));
    }
    s_perf_last = t;
    if (++s_perf_frames == 600) {
        fprintf(stdout, "arcade stages perf: stage %d, module %.3f ms a frame (max %.3f), frame %.2f ms (max %.2f), "
                "%u over 17.5 ms, %u over 25 ms\n", s_pack.stage, s_perf_mod / 600e6, s_perf_mod_max / 1e6,
                s_perf_gaps ? s_perf_gap_sum / (s_perf_gaps * 1e6) : 0.0, s_perf_gap_max / 1e6, s_perf_slow, s_perf_hitch);
        fflush(stdout);
        s_perf_mod = s_perf_mod_max = s_perf_gap_max = s_perf_gap_sum = 0;
        s_perf_frames = s_perf_gaps = s_perf_slow = s_perf_hitch = 0;
    }
}
static uint64_t s_perf_floor;           /* the floor hook's time this frame */

/* ------------------------------------------------------------------ */
/* GTE: RTPS (sf 1, lm 0) and NCLIP, as tools/arcade_stages/gte.py     */
/* ------------------------------------------------------------------ */
typedef struct { int32_t r[3][3]; int32_t t[3]; int32_t ofx, ofy; uint32_t h; } Gte;
static uint8_t s_unr[0x101];

static void unr_init(void) {
    for (int i = 0; i < 0x101; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        s_unr[i] = (uint8_t)(v < 0 ? 0 : v);
    }
}

static uint32_t gte_divide(uint32_t h, uint32_t sz3) {
    if (h >= sz3 * 2u) return 0x1FFFF;
    int z = 0;
    while (z < 15 && !(sz3 << z & 0x8000u)) z++;
    uint64_t n = (uint64_t)h << z;
    int64_t d = (int64_t)(sz3 << z);
    int64_t u = s_unr[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    uint64_t q = (n * (uint64_t)d + 0x8000) >> 16;
    return q > 0x1FFFF ? 0x1FFFF : (uint32_t)q;
}

static void rtps(const Gte *g, int x, int y, int z, int *sx, int *sy, int *sz) {
    int64_t mac[3];
    for (int i = 0; i < 3; i++)
        mac[i] = (((int64_t)g->t[i] << 12) + (int64_t)g->r[i][0] * x + (int64_t)g->r[i][1] * y
                  + (int64_t)g->r[i][2] * z) >> 12;
    int64_t ir0 = mac[0] < -0x8000 ? -0x8000 : mac[0] > 0x7FFF ? 0x7FFF : mac[0];
    int64_t ir1 = mac[1] < -0x8000 ? -0x8000 : mac[1] > 0x7FFF ? 0x7FFF : mac[1];
    uint32_t sz3 = (uint32_t)(mac[2] < 0 ? 0 : mac[2] > 0xFFFF ? 0xFFFF : mac[2]);
    int64_t q = gte_divide(g->h, sz3);
    int64_t vx = (q * ir0 + g->ofx) >> 16, vy = (q * ir1 + g->ofy) >> 16;
    *sx = (int)(vx < -0x400 ? -0x400 : vx > 0x3FF ? 0x3FF : vx);
    *sy = (int)(vy < -0x400 ? -0x400 : vy > 0x3FF ? 0x3FF : vy);
    *sz = (int)sz3;
}

/* rtps without the GTE's clamps: the screen point as far as it really
 * lands, and the signed depth (negative behind the camera). */
static void rtps_raw(const Gte *g, int x, int y, int z, int64_t *sx, int64_t *sy, int64_t *depth) {
    int64_t mac[3];
    for (int i = 0; i < 3; i++)
        mac[i] = (((int64_t)g->t[i] << 12) + (int64_t)g->r[i][0] * x + (int64_t)g->r[i][1] * y
                  + (int64_t)g->r[i][2] * z) >> 12;
    *depth = mac[2];
    int64_t d = mac[2] < 1 ? 1 : mac[2];
    *sx = (g->ofx >> 16) + mac[0] * (int64_t)g->h / d;
    *sy = (g->ofy >> 16) + mac[1] * (int64_t)g->h / d;
}

static int64_t nclip(int x0, int y0, int x1, int y1, int x2, int y2) {
    return (int64_t)x0 * y1 + (int64_t)x1 * y2 + (int64_t)x2 * y0
         - (int64_t)x0 * y2 - (int64_t)x1 * y0 - (int64_t)x2 * y1;
}

/* ------------------------------------------------------------------ */
/* PS1 matrices: 0x8003A490 (rotation through the GTE's GPF), MulMatrix2, */
/* ApplyMatrix                                                        */
/* ------------------------------------------------------------------ */
static void trig_ps1(void) {
    if (s_trig_ps1) return;
    for (unsigned i = 0; i < 4096; i++) {
        s_sin[i] = (int16_t)psx_mod_read_half(SIN_PS1 + 2 * i);
        s_cos[i] = (int16_t)psx_mod_read_half(COS_PS1 + 2 * i);
    }
    s_trig_ps1 = 1;
}

static int gpf(int ir0, int v) { return sat16(((int64_t)ir0 * v) >> 12); }

static void rot_ps1(int ax, int ay, int az, int32_t m[3][3]) {
    int sx = s_sin[ax & 0xFFF], cx = s_cos[ax & 0xFFF];
    int sy = s_sin[ay & 0xFFF], cy = s_cos[ay & 0xFFF];
    int sz = s_sin[az & 0xFFF], cz = s_cos[az & 0xFFF];
    int a1 = gpf(sx, cz), a2 = gpf(sx, sz), a3 = gpf(sx, cy);
    int i6 = (cx * cz) >> 12;
    int b1 = gpf(sy, a1), b2 = gpf(sy, a2), b3 = gpf(sy, i6);
    int i9 = (cx * sz) >> 12;
    int c1 = gpf(cy, cz), c2 = gpf(cy, -sz), c3 = gpf(cy, cx);
    m[0][0] = (int16_t)c1; m[0][1] = (int16_t)c2; m[0][2] = (int16_t)sy;
    m[1][0] = (int16_t)(b1 + i9); m[1][1] = (int16_t)(i6 - b2); m[1][2] = (int16_t)-a3;
    m[2][0] = (int16_t)(a2 - b3); m[2][1] = (int16_t)(((i9 * sy) >> 12) + a1); m[2][2] = (int16_t)c3;
}

static void mul_matrix2(const int32_t a[3][3], int32_t b[3][3]) {      /* b = a x b */
    int32_t out[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[i][j] = sat16(((int64_t)a[i][0] * b[0][j] + (int64_t)a[i][1] * b[1][j]
                               + (int64_t)a[i][2] * b[2][j]) >> 12);
    memcpy(b, out, sizeof out);
}

static void apply_matrix(const int32_t m[3][3], int x, int y, int z, int32_t out[3]) {
    for (int i = 0; i < 3; i++)
        out[i] = (int32_t)(((int64_t)m[i][0] * x + (int64_t)m[i][1] * y + (int64_t)m[i][2] * z) >> 12);
}

/* The tiles' GTE state, as 0x8006D014 builds it for the PS1 stage, with
 * the arcade stage's units (/ 10, not >> 3) and origin height. */
static void tile_matrix(Gte *g, int ps1_units) {
    int32_t cam[3][3], axis[3][3];
    int pitch = (int)psx_mod_read_word(CAMERA), yaw = (int)psx_mod_read_word(CAMERA + 4);
    int roll = (int)psx_mod_read_word(CAMERA + 8);
    int camx = (int)psx_mod_read_word(CAMERA + 0x14), camy = (int)psx_mod_read_word(CAMERA + 0x18);
    int camz = (int)psx_mod_read_word(CAMERA + 0x1C);
    rot_ps1(pitch, yaw, -roll, cam);
    for (int k = 0; k < 3; k++) cam[1][k] = (int16_t)((cam[1][k] * 0x14) / 0xC);
    rot_ps1(0, -(int)psx_mod_read_word(AXIS), 0, axis);
    /* the point the stage follows (0x80062B04 picks the smoothed fighter) */
    int dx, dz;
    if (psx_mod_read_byte(FIXED_STAGE)) {
        dx = -camx; dz = -camz;
    } else {
        int p1x = (int)psx_mod_read_word(P1_X), p1z = (int)psx_mod_read_word(P1_Z);
        int p2x = (int)psx_mod_read_word(P2_X), p2z = (int)psx_mod_read_word(P2_Z);
        int pick = -1;
        if (psx_mod_read_half(0x800A923Eu) == 4 && psx_mod_read_byte(psx_mod_read_word(0x800A927Cu) + 0x24) == 'S')
            pick = 0;
        else if (psx_mod_read_half(0x800AAACAu) == 4 && psx_mod_read_byte(psx_mod_read_word(0x800AAB08u) + 0x24) == 'S')
            pick = 1;
        if (pick == 0) {
            int hx = 0, hz = 0;
            for (unsigned k = 0; k < 4; k++) {
                hx += (int)psx_mod_read_word(HISTORY_P1 + 16 * k);
                hz += (int)psx_mod_read_word(HISTORY_P1 + 8 + 16 * k);
            }
            dx = (((hx >> 2) + p2x) >> 1) - camx;
            dz = (((hz >> 2) + p2z) >> 1) - camz;
        } else if (pick == 1) {
            int hx = 0, hz = 0;
            for (unsigned k = 0; k < 4; k++) {
                hx += (int)psx_mod_read_word(HISTORY_P2 + 16 * k);
                hz += (int)psx_mod_read_word(HISTORY_P2 + 8 + 16 * k);
            }
            dx = (((hx >> 2) + p1x) >> 1) - camx;
            dz = (((hz >> 2) + p1z) >> 1) - camz;
        } else {
            dx = ((p2x + p1x) >> 1) - camx;
            dz = ((p2z + p1z) >> 1) - camz;
        }
    }
    int vx, vy, vz;
    if (ps1_units) {        /* the PS1's own stage (probe: the GTE it must give) */
        int y0 = (int16_t)psx_mod_read_half(0x80025138u + 4 * psx_mod_read_word(0x800B0460u));
        vx = (int16_t)(dx >> 3); vz = (int16_t)((int16_t)dz >> 3); vy = (int16_t)((int16_t)(y0 - camy) >> 3);
    } else {
        vx = (int16_t)dx / 10; vz = (int16_t)dz / 10; vy = (int16_t)(STAGE_Y - camy) / 10;
    }
    apply_matrix(cam, vx, vy, vz, g->t);
    memcpy(g->r, axis, sizeof axis);
    mul_matrix2(cam, g->r);
}

/* ------------------------------------------------------------------ */
/* the pack                                                           */
/* ------------------------------------------------------------------ */
/* mods/arcade-stages beside the game (PC and Android), where CMakeLists.txt
 * stages the setup's packs (tools/arcade_stages/install.py);
 * TEKKEN3_ARCADE_STAGES_DIR for the development benches. */
static char s_dir[1024];

static const char *pack_dir(void) {
    if (s_dir[0]) return s_dir;
    const char *e = getenv("TEKKEN3_ARCADE_STAGES_DIR");
    if (e && *e) snprintf(s_dir, sizeof s_dir, "%s", e);
    else {
        const char *base = SDL_GetBasePath();
        snprintf(s_dir, sizeof s_dir, "%smods/arcade-stages", base ? base : "");
#if !defined(PSX_SDL3)
        SDL_free((void *)base);
#endif
    }
    return s_dir;
}

static const uint8_t *section(const uint8_t *b, const char *id) {
    uint32_t n; memcpy(&n, b + 8, 4);
    for (uint32_t i = 0; i < n; i++)
        if (!memcmp(b + 16 + 12 * i, id, 4)) { uint32_t o; memcpy(&o, b + 20 + 12 * i, 4); return b + o; }
    return NULL;
}

static void pack_free(void) {
    free(s_pack.file);
    free(s_pack.apixels);
    memset(&s_pack, 0, sizeof s_pack);
    s_pack.stage = -1;
}

static void anim_reset(void);

/* E5a: the 13 arcade stages, fixed (their objects and animations: E5b);
 * Gon's (13) and Dr. B's (14) arenas keep the PS1 stage. */
static int stage_supported(int arena) { return arena >= 0 && arena <= 12; }

static int pack_load(int arena) {
    char path[1024];
    snprintf(path, sizeof path, "%s/stage%02d.ast", pack_dir(), arena);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = size > 64 ? malloc((size_t)size) : NULL;
    int ok = b && fread(b, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    uint16_t version = 0, stage = 0; uint32_t total = 0;
    if (ok) { memcpy(&version, b + 4, 2); memcpy(&stage, b + 6, 2); memcpy(&total, b + 12, 4); }
    if (!ok || memcmp(b, "ASTG", 4) || version != PACK_FORMAT || stage != arena || total != (uint32_t)size
        || !section(b, "VRAM") || !section(b, "TILE") || !section(b, "FARB") || !section(b, "FLOR") || !section(b, "TRIG")) {
        fprintf(stdout, "arcade stages: %s is not a format %u pack of stage %d\n", path, PACK_FORMAT, arena);
        free(b);
        return 0;
    }
    pack_free();
    Pack *p = &s_pack;
    p->file = b; p->size = (uint32_t)size; p->stage = arena;
    /* animated texture: its frames are kept from the VRAM rectangles */
    const uint8_t *o = section(b, "ANIM");
    const uint16_t *frames = NULL;
    if (o) {
        uint16_t h[6]; memcpy(h, o, 12);
        p->ax = h[0]; p->ay = h[1]; p->aw = h[2]; p->ah = h[3]; p->aperiod = h[4]; p->aframes = h[5];
        frames = (const uint16_t *)(o + 12);
        p->apixels = p->aframes && p->aperiod ? calloc((size_t)p->aframes * p->aw * p->ah, 2) : NULL;
        if (!p->apixels) p->aframes = 0;
    }
    /* VRAM: rectangles in the upper half */
    o = section(b, "VRAM");
    uint32_t n; memcpy(&n, o, 4); o += 4;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t r[4]; memcpy(r, o, 8); o += 8;
        if (!psx_mod_gpu_high_vram_upload(r[0], r[1], r[2], r[3], (const uint16_t *)o)) {
            fprintf(stdout, "arcade stages: upload %u,%u %ux%u refused\n", r[0], r[1], r[2], r[3]);
            pack_free();
            return 0;
        }
        for (unsigned f = 0; f < p->aframes; f++) {        /* the parts of the frames this one holds */
            int fx = frames[2 * f], fy = frames[2 * f + 1];
            for (int y = 0; y < p->ah; y++) {
                int vy = fy + y - r[1];
                if (vy < 0 || vy >= r[3]) continue;
                for (int x = 0; x < p->aw; x++) {
                    int vx = fx + x - r[0];
                    if (vx >= 0 && vx < r[2])
                        memcpy(&p->apixels[((size_t)f * p->ah + y) * p->aw + x], o + 2u * ((unsigned)vy * r[2] + vx), 2);
                }
            }
        }
        o += 2u * r[2] * r[3];
    }
    /* objects: models, then the drawing calls */
    o = section(b, "OBJS");
    p->rot = (const uint32_t *)section(b, "ROTT");
    if (o && p->rot) {
        uint32_t nm; memcpy(&nm, o, 4); o += 4;
        for (uint32_t i = 0; i < nm && i < 4; i++) {
            uint16_t c[2]; memcpy(c, o, 4); o += 4;
            Model *m = &p->models[p->nmodels++];
            m->nv = c[0]; m->np = c[1];
            m->v = (const Vert *)o; o += 8u * m->nv;
            m->p = (const Prim *)o; o += sizeof(Prim) * m->np;
        }
        memcpy(&p->nlayout, o, 4);
        p->layout = (const Layout *)(o + 4);
        if (nm > 4 || p->nlayout > 8) { pack_free(); return 0; }
        for (unsigned i = 0; i < p->nlayout; i++)
            if (p->layout[i].model >= p->nmodels || p->models[p->layout[i].model].nv > 0x100) { pack_free(); return 0; }
    }
    /* tiles */
    o = section(b, "TILE");
    memcpy(&p->tile_size, o, 4); memcpy(&p->ntiles, o + 4, 4); o += 8;
    if (p->ntiles > 256) { pack_free(); return 0; }
    for (unsigned i = 0; i < p->ntiles; i++) {
        Tile *t = &p->tiles[i];
        t->head = (const TileHead *)o; o += 8;
        t->v = (const Vert *)o; o += 8u * t->head->nv;
        t->p = (const Prim *)o; o += sizeof(Prim) * t->head->np;
    }
    /* far background */
    o = section(b, "FARB");
    int16_t h[6]; memcpy(h, o, 12); o += 12;
    p->fcols = h[0]; p->frows = h[1]; p->fring = h[2]; p->fyoff = h[3]; p->fdegrees = h[4]; p->fpitch = h[5];
    p->fcolours = o; o += 12;
    p->fdesc = (const uint16_t *)o; o += 2 * p->fcols * p->frows;
    p->fcluts = (const uint16_t *)o; o += 2 * p->fcols * p->frows;
    p->fkinds = o;
    /* floor */
    o = section(b, "FLOR");
    memcpy(&p->lkind, o, 2); memcpy(&p->lfog, o + 2, 2); memcpy(&p->lnear, o + 4, 4); o += 8;
    p->lcells = (const uint32_t *)o; p->lhalves = p->lcells + 200; o += 4000;
    p->lunique = o; o += 100;
    memcpy(&p->lnv, o, 2); p->lverts = (const int16_t *)(o + 2);
    o += (2 + 4 * p->lnv + 3) & ~3u;
    memcpy(p->lcount, o, 100); o += 100;
    for (unsigned c = 0; c < 100; c++) { p->lquads[c] = (const uint32_t *)o; o += 4u * p->lcount[c]; }
    for (unsigned k = 1; k <= 4; k <<= 1) { p->luv[k] = (const uint16_t *)o; o += 2u * 4 * k * 4; }
    if (p->lkind < 0 || p->lkind > 2 || p->lnv > 256) { pack_free(); return 0; }
    o = section(b, "TRIG");
    p->sin = (const int16_t *)o; p->cos = p->sin + 4096; p->sqrt1 = p->cos + 4096;
    p->fog = (const uint8_t *)(p->sqrt1 + 192); p->sqrt2 = (const int16_t *)(p->fog + 1024);
    fprintf(stdout, "arcade stages: stage %d loaded from %s (%ld bytes, %u textures and palettes, %u objects, "
            "%u animation frames)\n", arena, path, size, (unsigned)n, p->nlayout, p->aframes);
    fflush(stdout);
    anim_reset();
    return 1;
}

/* The arcade stage draws this frame. */
static int active(void) {
    if (!s_on || !psx_mod_gpu_high_vram_ready() || !psx_mod_game_started()) return 0;
    if (psx_mod_read_byte(NO_STAGE) || psx_mod_read_byte(DARK_ARENA)) return 0;
    uint32_t mode = psx_mod_read_word(MODE);
    if (mode == 7 || mode == 8) return 0;
    int arena = psx_mod_read_half(ARENA);
    if (arena != s_pack.stage) {
        if (arena == s_tried) return 0;
        s_tried = arena;
        if (!stage_supported(arena) || !pack_load(arena)) return 0;
    }
    trig_ps1();
    return 1;
}

/* ------------------------------------------------------------------ */
/* packets                                                            */
/* ------------------------------------------------------------------ */
static uint32_t s_cursor, s_end;

static uint32_t alloc_words(unsigned words) {
    uint32_t a = s_cursor;
    if (a + 4u * words > s_end) return 0;
    s_cursor += 4u * words;
    return a;
}

static void add_prim(uint32_t ot, uint32_t p, unsigned words) {
    uint32_t e = psx_mod_read_word(ot);
    psx_mod_write_word(p, (uint32_t)words << 24 | (e & 0xFFFFFFu));
    psx_mod_write_word(ot, (e & 0xFF000000u) | (p & 0xFFFFFFu));
}

static uint32_t main_ot(unsigned rank) {
    return psx_mod_read_word(psx_mod_read_word(DRAWENV) + 4) + 4u * rank;
}

static unsigned buffer(void) { return psx_mod_read_word(OT_BUFFER) & 1u; }

static void tile_rect(uint32_t ot, int x, int y, int w, int h, const uint8_t c[3]) {
    if (w <= 0 || h <= 0) return;
    uint32_t p = alloc_words(4);
    if (!p) return;
    psx_mod_write_word(p + 4, 0x60000000u | rgbw(c));
    psx_mod_write_word(p + 8, xy(x, y));
    psx_mod_write_word(p + 12, (uint32_t)h << 16 | (uint32_t)w);
    add_prim(ot, p, 3);
    s_stat_far++;
}

/* ------------------------------------------------------------------ */
/* far background (0x801A7688), arcade 512 x 240 mapped on the PS1      */
/* screen at the same angle per pixel                                 */
/* ------------------------------------------------------------------ */
typedef struct { int ofx, ofy, h, margin; } Screen;

static int map_x(const Screen *s, int x) { return s->ofx + (int)(((int64_t)(x - 256) * s->h) / 600); }
static int map_y(const Screen *s, int y) { return s->ofy + (int)(((int64_t)(y - 120) * s->h) / 225); }

static int per_degree(int degrees) {
    int64_t v = (int64_t)degrees << 12;
    int64_t hi = (v * ((int64_t)0xB60B60B7 - ((int64_t)1 << 32))) >> 32;
    return (int)(((hi + v) >> 8) - (v >> 31));
}

static void band(uint32_t ot, const Screen *s, int y0, int y1, const uint8_t c[3]) {
    int a = map_y(s, y0), b = map_y(s, y1);
    if (a < 0) a = 0;
    tile_rect(ot, -s->margin, a, SCREEN_W + 2 * s->margin, b - a, c);
}

static void far_background(const Screen *s, int yaw, int pitch) {
    const Pack *p = &s_pack;
    uint32_t ot = main_ot(1023);
    const uint8_t *cell_rgb = p->fcolours, *top_rgb = p->fcolours + 3, *bottom_rgb = p->fcolours + 7;
    int top_on = p->fcolours[6], bottom_on = p->fcolours[10];
    const int H = 0xF0, step = 0x20;
    int a = tdiv((int64_t)((-yaw) & 0xFFF) << 9, per_degree(p->fdegrees));
    int scroll = tmod(a, p->fcols << 6);
    int col0 = tdiv(scroll, 64), kind0 = tdiv(tmod(a, p->fring << 6), 64);
    int y = tdiv((int64_t)-s12(pitch) * p->fpitch, 256) + p->fyoff;
    int st = p->stage;
    if (st == 6) { band(ot, s, 0, H, top_rgb); return; }
    if (st == 3) {      /* a gradient, then the bottom colour */
        if (y > 0) y = 0;
        int y2 = y + 0x60;
        uint32_t q = alloc_words(9);
        if (q) {
            int ya = map_y(s, y), yb = map_y(s, y2), xa = -s->margin, xb = SCREEN_W + s->margin;
            const uint32_t top = 0x661400u;                       /* (0, 0x14, 0x66) */
            psx_mod_write_word(q + 4, 0x38000000u | top); psx_mod_write_word(q + 8, xy(xa, ya));
            psx_mod_write_word(q + 12, top); psx_mod_write_word(q + 16, xy(xb, ya));
            psx_mod_write_word(q + 20, 0); psx_mod_write_word(q + 24, xy(xa, yb));
            psx_mod_write_word(q + 28, 0); psx_mod_write_word(q + 32, xy(xb, yb));
            add_prim(ot, q, 8);
        }
        band(ot, s, y2, H * 75 / 100, bottom_rgb);
        return;
    }
    if ((st == 11 || st == 0 || st == 5) && y > 0) y = 0;
    if (st == 11 && y < -0x60) y = -0x60;
    if (y > 0 && top_on) band(ot, s, 0, y, top_rgb);
    /* columns: the arcade's 9 from the left edge, more for the 16:9 margins */
    int left = (int)(((int64_t)(s->margin + s->ofx) * 600) / s->h) - 256;   /* arcade pixels left of 0 */
    int extra = left > 0 ? (left + 63) / 64 : 0;
    int count = 9 + 2 * extra;
    for (int r = 0; r < p->frows; r++) {
        int x = tdiv(scroll, 64) * 64 - scroll - 64 * extra;
        for (int c = -extra; c < count - extra; c++) {
            if (!(y < -0x40 || y > H)) {
                int kind = p->fkinds[r * p->fring + pmod(kind0 + c, p->fring)];
                int k = pmod(col0 + c, p->fcols);
                int xa = map_x(s, x), xb = map_x(s, x + 0x40), ya = map_y(s, y), yb = map_y(s, y + step);
                if (kind == 0x31) {
                    uint16_t d = p->fdesc[r * p->fcols + k];
                    int tx = (d >> 2) & 0x3C0, ty = (d >> 4) & 0x300;
                    uint32_t tp = (uint32_t)(((ty & 0x100) >> 4 | tx >> 6) | (ty & 0x200) << 2);
                    int u = (d & 3) << 6, v = (d << 4) & 0xC0;
                    int v0 = v, v1 = v + 0x3F;
                    if (st == 5 && r == 3) { v0 = v + 0x3F; v1 = v; }
                    uint32_t clut = p->fcluts[r * p->fcols + k];
                    uint32_t q = alloc_words(10);
                    if (q) {
                        psx_mod_write_word(q + 4, 0x2C808080u);
                        psx_mod_write_word(q + 8, xy(xa, ya));
                        psx_mod_write_word(q + 12, clut << 16 | (uint32_t)v0 << 8 | (uint32_t)u);
                        psx_mod_write_word(q + 16, xy(xb, ya));
                        psx_mod_write_word(q + 20, tp << 16 | (uint32_t)v0 << 8 | (uint32_t)(u + 0x3F));
                        psx_mod_write_word(q + 24, xy(xa, yb));
                        psx_mod_write_word(q + 28, (uint32_t)v1 << 8 | (uint32_t)u);
                        psx_mod_write_word(q + 32, xy(xb, yb));
                        psx_mod_write_word(q + 36, (uint32_t)v1 << 8 | (uint32_t)(u + 0x3F));
                        add_prim(ot, q, 9);
                        s_stat_far++;
                    }
                } else if (kind == 0x74) {
                    tile_rect(ot, xa, ya, xb - xa, yb - ya, cell_rgb);
                } else if (kind == 0x64) {
                    tile_rect(ot, xa, ya, xb - xa, yb - ya, bottom_rgb);
                }
            }
            x += 0x40;
        }
        y += step;
    }
    if (y < H && bottom_on) band(ot, s, y < 0 ? 0 : y, H * 75 / 100, bottom_rgb);
}

/* ------------------------------------------------------------------ */
/* tiles (0x801D911C, 0x801DB560)                                     */
/* ------------------------------------------------------------------ */
static const unsigned PRIM_WORDS[7] = {7, 9, 5, 9, 12, 6, 8};   /* FT3 FT4 F4 GT3 GT4 G3 G4 */
static const unsigned PRIM_VERTS[7] = {3, 4, 4, 3, 4, 3, 4};

static uint32_t tex_window(const uint8_t w[4]) {
    unsigned x = w[0], y = w[1], ww = w[2], hh = w[3];
    if (!(ww || hh || x || y)) return 0xE2000000u;
    return 0xE2000000u | ((~(ww - 1u) & 0xFFu) >> 3) | ((~(hh - 1u) & 0xFFu) >> 3) << 5
         | (x >> 3) << 10 | (y >> 3) << 15;
}

static int emit_prim(uint32_t ot, const Prim *pr, const int *sx, const int *sy) {
    unsigned kind = pr->kind, n = PRIM_VERTS[kind], words = PRIM_WORDS[kind];
    int win = (pr->header & 0x80000000u) != 0;
    uint32_t q = alloc_words(1 + words + 2 * win);
    if (!q) return 0;
    uint32_t o = q + 4;
    if (win) { psx_mod_write_word(o, tex_window(pr->win)); o += 4; }
    int gouraud = kind >= 3, textured = kind != 2 && kind != 5 && kind != 6;
    for (unsigned k = 0; k < n; k++) {
        unsigned i = pr->idx[k];
        if (k == 0 || gouraud) {
            uint32_t c = rgbw(pr->rgb[gouraud ? k : 0]);
            psx_mod_write_word(o, k == 0 ? (uint32_t)pr->code << 24 | c : c); o += 4;
        }
        psx_mod_write_word(o, xy(sx[i], sy[i])); o += 4;
        if (textured) {
            uint32_t w = (uint32_t)pr->uv[k][1] << 8 | pr->uv[k][0];
            if (k == 0) w |= (uint32_t)pr->clut << 16;
            if (k == 1) w |= (uint32_t)pr->tpage << 16;
            psx_mod_write_word(o, w); o += 4;
        }
    }
    if (win) psx_mod_write_word(o, 0xE2000000u);
    add_prim(ot, q, words + 2 * win);
    return 1;
}

/* 0x801DA734 (scene.tile_visibility): the tiles the arcade draws, from a
 * 2D cone on the 16 x 16 grid. dir(a) is the (sine, cosine) of the ray at
 * angle a; x, z the camera in the grid's frame; size the world units of a
 * tile. Every tile as near to the camera's as the nearest centre tile is
 * dropped: nothing between the camera and the fighters. */
static void tile_visibility(int fov, int yaw, int x, int z, int size, int8_t vis[256]) {
    memset(vis, 0, 256);
    if (size <= 0) return;
    int step = tdiv((int64_t)size * 14, 20);
    if (x < -7 * size) x = -8 * size;
    if (!(x < 7 * size)) x = 8 * size - 1;
    if (z < -7 * size) z = -8 * size;
    if (!(z < 7 * size)) z = 8 * size - 1;
    int end[2][2] = {{0, 0}, {0, 0}};
    for (int k = 0; k < 2; k++) {
        int a = (-yaw + (k ? 1 : -1) * tdiv(fov, 2)) & 0xFFF;
        int dx = (step * (s_sin[a] << 3)) >> 15, dz = (step * (s_cos[a] << 3)) >> 15;   /* 4.12 for the arcade's 1.15 */
        int px = x + 8 * size, pz = z + 8 * size;
        for (;;) {
            int c = tdiv(px, size), r = tdiv(pz, size);
            if (c < 0 || c > 15 || r < 0 || r > 15) break;
            vis[16 * r + c] = 1;
            end[k][0] = c; end[k][1] = r;
            px += dx; pz += dz;
        }
    }
    int c1 = end[0][0], r1 = end[0][1], c2 = end[1][0], r2 = end[1][1];
    for (int n = 0; n < 64; n++) {          /* the border walk, 64 steps go round */
        if (r1 == 15 && c1 < 15) { vis[0xF0 + ++c1] = 1; if (c1 == c2 && r1 == r2) break; }
        if (c1 == 15 && r1 > 0) { vis[16 * --r1 + 15] = 1; if (c1 == c2 && r1 == r2) break; }
        if (r1 == 0 && c1 > 0) { vis[--c1] = 1; if (c1 == c2 && r1 == r2) break; }
        if (c1 == 0 && r1 < 15) { vis[16 * ++r1] = 1; if (c1 == c2 && r1 == r2) break; }
    }
    for (int r = 0; r < 16; r++) {
        int first = -1, last = -1;
        for (int c = 0; c < 16; c++) if (vis[16 * r + c]) { if (first < 0) first = c; last = c; }
        for (int c = first; c >= 0 && c < last; c++) vis[16 * r + c] = 1;
    }
    int cc = tdiv(x + 8 * size, size), cr = tdiv(z + 8 * size, size);
    vis[16 * cr + cc] = -2;
    int near = 0x7FFF;
    static const int centre[4][2] = {{7, 7}, {7, 8}, {8, 7}, {8, 8}};
    for (int k = 0; k < 4; k++) {
        int c = centre[k][0], r = centre[k][1];
        if (vis[16 * r + c] && abs(c - cc) + abs(r - cr) < near) near = abs(c - cc) + abs(r - cr);
    }
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++)
            if (vis[16 * r + c] && abs(c - cc) + abs(r - cr) <= near) vis[16 * r + c] = 0;
}

/* The cone's inputs on the PS1, as 0x801D911C builds them: the camera
 * minus the fighters' midpoint (the world origin when the stage is fixed),
 * turned by the axis; its width from the PS1 screen (GTE H, OFX and the
 * 16:9 margin) instead of the arcade's per-stage table (0x80186858), with
 * the arcade's slack (its cone is 1.17 to 1.29 times its screen). */
static void visible_tiles(const Screen *s, int8_t vis[256]) {
    int axis = (int)psx_mod_read_word(AXIS);
    int camx = (int)psx_mod_read_word(CAMERA + 0x14), camz = (int)psx_mod_read_word(CAMERA + 0x1C);
    int vx, vz;
    if (psx_mod_read_byte(FIXED_STAGE)) {
        vx = (int16_t)camx; vz = (int16_t)camz;
    } else {
        int p1x = (int)psx_mod_read_word(P1_X), p1z = (int)psx_mod_read_word(P1_Z);
        int p2x = (int)psx_mod_read_word(P2_X), p2z = (int)psx_mod_read_word(P2_Z);
        vx = (int16_t)(camx - (p1x + (int16_t)tdiv((int64_t)p2x - p1x, 2)));
        vz = (int16_t)(camz - (p1z + (int16_t)tdiv((int64_t)p2z - p1z, 2)));
    }
    int32_t m[3][3], v[3];
    rot_ps1(0, axis, 0, m);
    apply_matrix(m, vx, 0, vz, v);
    int half_w = s->ofx > SCREEN_W - s->ofx ? s->ofx : SCREEN_W - s->ofx;
    half_w += s->margin;
    int a = 0;      /* half the screen's angle: the first a with tan a >= half_w / H */
    while (a < 0x3FF && (int64_t)s_sin[a] * s->h < (int64_t)half_w * s_cos[a]) a++;
    int fov = 2 * a * 13 / 10;
    tile_visibility(fov, (int)psx_mod_read_word(CAMERA + 4) - axis, v[0], v[2], 10 * (int)s_pack.tile_size, vis);
    if (s_probe) {          /* stage_check.py probe: against scene.tile_visibility */
        fprintf(s_probe, "cone %u fov %d yaw %d x %d z %d size %d tiles ", psx_mod_read_word(0x800A08F0u), fov,
                (int)psx_mod_read_word(CAMERA + 4) - axis, v[0], v[2], 10 * (int)s_pack.tile_size);
        for (int r = 0; r < 16; r++) {
            unsigned bits = 0;
            for (int c = 0; c < 16; c++) bits |= (vis[16 * r + c] > 0) << c;
            fprintf(s_probe, "%04X", bits);
        }
        fprintf(s_probe, "\n");
    }
}

static void tiles(const Gte *g, const Screen *s, uint32_t ot_a, uint32_t ot_b) {
    static int sx[256], sy[256], sz[256];
    int8_t vis[256];
    visible_tiles(s, vis);
    for (unsigned t = 0; t < s_pack.ntiles; t++) {
        const Tile *tl = &s_pack.tiles[t];
        unsigned nv = tl->head->nv;
        s_stat_cone += vis[t] > 0;
        if (!tl->head->np || tl->head->skipped || nv > 0x100 || vis[t] <= 0) continue;
        for (unsigned i = 0; i < nv; i++) rtps(g, tl->v[i].x, tl->v[i].y, tl->v[i].z, &sx[i], &sy[i], &sz[i]);
        uint32_t ot = (tl->head->layer_b ? ot_b : ot_a) + 4u * tl->head->slot;
        for (unsigned k = 0; k < tl->head->np; k++) {
            const Prim *pr = &tl->p[k];
            if (pr->kind > 6) continue;
            unsigned a = pr->idx[0], b = pr->idx[1], c = pr->idx[pr->cull];
            if (a >= nv || b >= nv || c >= nv) continue;
            if (nclip(sx[a], sy[a], sx[b], sy[b], sx[c], sy[c]) <= 0) continue;
            /* not in the arcade: a vertex behind the camera gives a stray
             * polygon (never seen in the arcade's fights: its cone drops the
             * tiles near the camera) */
            int behind = 0;
            for (unsigned q = 0; q < PRIM_VERTS[pr->kind]; q++)
                if (pr->idx[q] < nv && sz[pr->idx[q]] < 16) behind = 1;
            if (behind) { s_stat_behind++; continue; }
            s_stat_tiles += emit_prim(ot, pr, sx, sy);
        }
    }
}

/* ------------------------------------------------------------------ */
/* the objects' animation (0x801E3D0C, tools/arcade_stages/anim.py):    */
/* stage 3's turning model (0x801E5AD0), stage 11's helicopter          */
/* (0x801E3F68 / 0x801E40C0 and its three flights). The arcade's memory  */
/* as s16 halves, by address, so the port reads as the reference.       */
/* ------------------------------------------------------------------ */
static int16_t s_st[0x40], s_ang[16], s_pos[16];
#define ST(a)  s_st[((a) - 0x8021F9F8u) >> 1]      /* 0x8021F9F8..0x8021FA78 */
#define ANG(a) s_ang[((a) - 0x802FECE4u) >> 1]     /* 4 SVECTORs: angles */
#define POS(a) s_pos[((a) - 0x8031E134u) >> 1]     /* 4 SVECTORs: positions */
#define U16(v) ((int)(uint16_t)(v))
static unsigned s_anim_count;           /* animated texture: 0x8021F9CC */
static unsigned s_anim_frame;           /* 0x8021F9D0 */
static int s_was_over;                  /* the round was decided last frame */
static unsigned s_stat_objects, s_stat_obj_behind;

/* 0x801E59FC */
static void stage3_init(void) {
    for (int i = 0; i < 3; i++) {
        POS(0x8031E134u + 8 * i) = 0; POS(0x8031E136u + 8 * i) = -300; POS(0x8031E138u + 8 * i) = 0xC1C;
    }
    ANG(0x802FECE6u) = 0x955; ANG(0x802FECE4u) = 0; ANG(0x802FECE8u) = 0;
    ANG(0x802FECECu) = 0; ANG(0x802FECEEu) = 0; ANG(0x802FECF0u) = 0;
    ANG(0x802FECF4u) = 0; ANG(0x802FECF6u) = 0x800; ANG(0x802FECF8u) = 0;
    ST(0x8021FA74u) = 0;
    POS(0x8031E14Cu) = 0; POS(0x8031E14Eu) = 0; POS(0x8031E150u) = 0;
    ANG(0x802FECFCu) = 0; ANG(0x802FECFEu) = 0x800; ANG(0x802FED00u) = 0;
}

/* 0x801E5AD0: model 0 turns by -5 a frame, back to 0x955 every 0x88 */
static void stage3_step(void) {
    ANG(0x802FECE6u) -= 5;
    ST(0x8021FA74u) += 1;
    if (ST(0x8021FA74u) == 0x88) { ANG(0x802FECE6u) = 0x955; ST(0x8021FA74u) = 0; }
}

/* 0x801E3DE0; mode: the buttons held (0x8021FBF4) or the vsync count & 3 in
 * the arcade */
static void stage11_init(int mode) {
    ANG(0x802FECE4u) = -0x9F; ANG(0x802FECE6u) = 0x71C;
    POS(0x8031E136u) = -0x190; POS(0x8031E13Eu) = -0x12C; POS(0x8031E146u) = 0x118; POS(0x8031E148u) = -0x4CE;
    ST(0x8021FA04u) = 0x80; ST(0x8021FA14u) = -2;
    ANG(0x802FECE8u) = 0; ANG(0x802FECECu) = 0; ANG(0x802FECEEu) = 0; ANG(0x802FECF0u) = 0;
    ANG(0x802FECF4u) = 0; ANG(0x802FECF6u) = 0; ANG(0x802FECF8u) = 0;
    POS(0x8031E134u) = 0; POS(0x8031E138u) = 0x1068; POS(0x8031E13Cu) = 0; POS(0x8031E140u) = 0; POS(0x8031E144u) = 0;
    ST(0x8021FA1Cu) = 0; ST(0x8021FA4Cu) = 0; ST(0x8021FA00u) = -1; ST(0x8021FA6Cu) = 0x20; ST(0x8021FA08u) = -1;
    ST(0x8021FA0Cu) = 0x20; ST(0x8021FA10u) = -1; ST(0x8021FA18u) = 0; ST(0x8021FA20u) = 0; ST(0x8021FA24u) = 0;
    ST(0x8021FA28u) = 0; ST(0x8021FA54u) = 0; ST(0x8021FA34u) = 0; ST(0x8021FA48u) = 0; ST(0x8021FA5Cu) = -0x46;
    ST(0x8021FA60u) = 0x46; ST(0x8021FA50u) = 0; ST(0x8021FA64u) = 1; ST(0x8021FA30u) = 1; ST(0x8021FA70u) = 0x1068;
    ST(0x8021F9FEu) = 1;
    ST(0x8021FA44u) = (int16_t)(mode & 3);
}

/* x, z on the circle: sine[-angle], sine[-angle + 0x400] x radius */
static void circle(int angle, int radius) {
    const int16_t *sine = s_pack.sin;
    int a = -angle;
    int v = sine[a & 0xFFF] * radius;
    POS(0x8031E134u) = (int16_t)((v < 0 ? v + 0xFFF : v) >> 12);
    v = sine[(a + 0x400) & 0xFFF] * radius;
    POS(0x8031E138u) = (int16_t)((v < 0 ? v + 0xFFF : v) >> 12);
}

static void f3f68(int yaw) {
    ANG(0x802FECE6u) = (int16_t)(U16(ANG(0x802FECE6u)) & 0xFFF);
    ST(0x8021FA20u) = (int16_t)(U16(ST(0x8021FA20u)) & 0xFFF);
    int mode = ST(0x8021FA44u);
    if (mode >= 1 && mode <= 3) {
        ST(0x8021FA50u) += 1;
        ST(0x8021FA1Cu) = (int16_t)(U16(ST(0x8021FA1Cu)) | 0x1000);
        return;
    }
    if (mode != 0) return;
    yaw = (int16_t)yaw;
    ST(0x8021FA58u) = (int16_t)yaw;
    if (ST(0x8021FA1Cu) & 2) return;
    int last = ST(0x8021FA54u), d, bits;
    if (last < yaw) { d = yaw - last; bits = 7; } else { d = last - yaw; bits = 0x23; }
    if (d > 0x17C) {
        uint32_t table = d > 0x352 ? 0x802045F4u : 0x802045ECu;   /* read by 0x801E42DC only: never in mode 0 */
        ST(0x8021F9F8u) = (int16_t)(table & 0xFFFF); ST(0x8021F9FAu) = (int16_t)(table >> 16);
        ST(0x8021FA1Cu) = (int16_t)(U16(ST(0x8021FA1Cu)) | bits);
        ST(0x8021FA54u) = (int16_t)yaw;
        ST(0x8021FA20u) = ANG(0x802FECE6u);
        ST(0x8021FA18u) = 0;
        return;
    }
    ST(0x8021FA50u) += 1;
}

/* mode 1 (0x801E4A0C) */
static void f4a0c(void) {
    int t = ST(0x8021FA50u), st = ST(0x8021FA4Cu);
    switch (st) {
    case 0:
        if (t < 200) { ANG(0x802FECE6u) += 4; return; }
        break;
    case 1:
        if (t < 0x118) { ANG(0x802FECE6u) += 4; return; }
        break;
    case 2: {
        ST(0x8021FA38u) = (int16_t)((ST(0x8021FA34u) >> 3) | 1);
        if (t == 500) ST(0x8021F9FCu) = 0;
        if (ST(0x8021F9FCu) == 0) {
            if (t < 0x212) ANG(0x802FECE8u) += (int16_t)(ST(0x8021FA30u) * 6);
            if (t < 0x230) ANG(0x802FECE4u) += 4;
            ST(0x8021FA34u) += ST(0x8021F9FEu) == 0 ? 1 : -1;
            if (ST(0x8021FA34u) == 0) { ST(0x8021FA4Cu) = 5; ST(0x8021FA50u) = 0; }
        } else {
            if (t < 0x1F) ANG(0x802FECE8u) -= (int16_t)(ST(0x8021FA30u) * 6);
            if (t < 0x3D) ANG(0x802FECE4u) -= 1;
            if (abs(ST(0x8021FA38u)) < 8) ST(0x8021FA34u) += ST(0x8021F9FEu) != 0 ? 1 : -1;
            else ST(0x8021FA38u) = ST(0x8021F9FEu) != 0 ? 8 : -8;
        }
        int step = ST(0x8021FA38u), angle = (int16_t)(ST(0x8021FA24u) + step);
        ST(0x8021FA24u) = (int16_t)angle;
        circle(angle, 0x1068);
        ANG(0x802FECE6u) -= (int16_t)step;
        return;
    }
    case 4:
        if (t < 0x118) { ANG(0x802FECE6u) -= 4; return; }
        break;
    case 5:
        if (t < 0x1E) return;
        if (t < 0xD2) ANG(0x802FECE4u) -= 1;
        if (t > 0x137) {
            if (ST(0x8021FA34u) == 0) ST(0x8021F9FEu) = (int16_t)(1 - ST(0x8021F9FEu));
            ST(0x8021F9FCu) = 1;
            ST(0x8021FA30u) = (int16_t)-ST(0x8021FA30u);
            ST(0x8021FA4Cu) = 2;
            ST(0x8021FA50u) = 0;
            return;
        }
        ANG(0x802FECE6u) += ST(0x8021F9FEu) != 0 ? -4 : 4;
        return;
    default:
        return;
    }
    ST(0x8021FA4Cu) = 2; ST(0x8021FA50u) = 0; ST(0x8021F9FCu) = 1;
}

/* mode 2 (0x801E4E04): radius in 0x8021FA70 */
static void radius_circle(int a) {
    const int16_t *sine = s_pack.sin;
    int r = ST(0x8021FA70u);
    int v = r * sine[a & 0xFFF];
    POS(0x8031E134u) = (int16_t)((v < 0 ? v + 0xFFF : v) >> 12);
    v = r * sine[(a + 0x400) & 0xFFF];
    POS(0x8031E138u) = (int16_t)((v < 0 ? v + 0xFFF : v) >> 12);
}

static void f4e04(void) {
    int st = ST(0x8021FA4Cu), t = ST(0x8021FA50u), next;
    switch (st) {
    case 0:
        if (t < 0x1E) ANG(0x802FECE8u) -= 8;
        if ((uint32_t)(U16(ST(0x8021FA50u)) - 0x82) < 0x1Eu) ANG(0x802FECE8u) += 8;
        if (t < 0xA0) ANG(0x802FECE6u) -= 0xB;
        if (t == 0x8C) ST(0x8021F9FCu) = 1;
        if (t < 0x8D) return;
        ST(0x8021FA38u) = ST(0x8021FA34u) > 1 ? 0x19 : 5;
        if (t == 0x15E) ST(0x8021F9FCu) = 0;
        if (ST(0x8021F9FCu) == 0) {
            ST(0x8021FA34u) -= 1;
            if (ST(0x8021FA34u) == 0) { ST(0x8021FA4Cu) = 2; ST(0x8021FA50u) = 0; }
        } else {
            if (ST(0x8021FA38u) > 200) ST(0x8021FA38u) = 200;
            ST(0x8021FA34u) += 1;
        }
        ST(0x8021FA70u) += ST(0x8021FA38u);
        radius_circle(-U16(ST(0x8021FA24u)));
        break;
    case 2:
        if (t < 0x50) { ANG(0x802FECE6u) -= 0xB; ANG(0x802FECE8u) -= 3; }
        else {
            ST(0x8021F9FCu) = 1; ST(0x8021FA4Cu) = 3; ST(0x8021FA50u) = 0; ST(0x8021FA24u) = 0; ST(0x8021FA34u) = 0;
            ANG(0x802FECE6u) -= 0xB; ANG(0x802FECE8u) -= 5;
        }
        break;
    case 3: {
        if (t < 0x14) { ANG(0x802FECE6u) -= 0x16; ANG(0x802FECE8u) -= 5; }
        else ANG(0x802FECE6u) -= ST(0x8021FA38u);
        ST(0x8021FA38u) = (int16_t)((ST(0x8021FA34u) >> 3) | 1);
        if (ST(0x8021FA50u) == 0x578) ST(0x8021F9FCu) = 0;
        if (ST(0x8021F9FCu) == 0) {
            next = ST(0x8021FA34u) - 1;
            if (ST(0x8021FA34u) == 1) { ST(0x8021FA4Cu) = 4; ST(0x8021FA50u) = 0; }
        } else {
            next = ST(0x8021FA34u) + 1;
            if (ST(0x8021FA38u) > 8) { ST(0x8021FA38u) = 8; next = ST(0x8021FA34u); }
        }
        ST(0x8021FA34u) = (int16_t)next;
        int old = U16(ST(0x8021FA24u));
        ST(0x8021FA70u) -= 10;
        ST(0x8021FA24u) = (int16_t)(old + ST(0x8021FA38u));
        int a = -(old + U16(ST(0x8021FA38u)));
        if (ST(0x8021FA70u) < 0x1068) ST(0x8021FA70u) = 0x1068;
        radius_circle(a);
        break;
    }
    case 4:
        if (t < 100) ANG(0x802FECE8u) += 3;
        if (t > 0xA0) { ST(0x8021FA50u) = 0; ST(0x8021FA4Cu) = 5; }
        break;
    case 5:
        if (t < 0x15E) ANG(0x802FECE6u) -= 5;
        else if (t < 0x1D6) ANG(0x802FECE8u) += 2;
        if (t < 0x3C) ANG(0x802FECE4u) += 1;
        if (t < 0x78) ANG(0x802FECE8u) -= 2;
        if (t > 0x208) { ST(0x8021FA4Cu) = 6; ST(0x8021FA50u) = 0; ST(0x8021F9FCu) = 1; }
        break;
    case 6: {
        if (t < 0x1E) ANG(0x802FECE4u) -= 1;
        if (t < 0x50) ANG(0x802FECE8u) += 4;
        ST(0x8021FA38u) = (int16_t)((ST(0x8021FA34u) >> 3) | 1);
        if (ST(0x8021F9FCu) == 0) {
            next = ST(0x8021FA34u) - 1;
            if (ST(0x8021FA34u) == 1) { ST(0x8021FA4Cu) = 7; ST(0x8021FA50u) = 0; }
        } else {
            next = ST(0x8021FA34u) + 1;
            if (ST(0x8021FA38u) > 8) { ST(0x8021FA38u) = 8; next = ST(0x8021FA34u); }
        }
        ST(0x8021FA34u) = (int16_t)next;
        int angle = (int16_t)(ST(0x8021FA24u) - ST(0x8021FA38u));
        ST(0x8021FA24u) = (int16_t)angle;
        circle(angle, 0x1068);
        ANG(0x802FECE6u) += ST(0x8021FA38u);
        break;
    }
    default:
        break;
    }
}

/* mode 3 (0x801E54B4) */
static void f54b4(void) {
    int st = ST(0x8021FA4Cu), t = ST(0x8021FA50u), next;
    switch (st) {
    case 0:
        if (t < 0x1E) ANG(0x802FECE4u) -= 1;
        if (t < 0x10E) POS(0x8031E136u) += 10;
        else {
            ST(0x8021FA4Cu) = 1; ST(0x8021FA50u) = 0;
            ST(0x8021FA68u) = ST(0x8021FA64u) ? ST(0x8021FA5Cu) : ST(0x8021FA60u);
        }
        break;
    case 1:
        if (t < 0x79) {
            if (ST(0x8021FA64u)) POS(0x8031E138u) += ST(0x8021FA68u);
            else POS(0x8031E134u) += ST(0x8021FA68u);
        } else {
            ST(0x8021FA50u) = 0;
            if (ST(0x8021FA64u)) ST(0x8021FA5Cu) = (int16_t)-ST(0x8021FA5Cu);
            else ST(0x8021FA60u) = (int16_t)-ST(0x8021FA60u);
            ST(0x8021FA4Cu) = 2;
            ST(0x8021FA64u) = (int16_t)(1 - ST(0x8021FA64u));
        }
        break;
    case 2:
        if (t < 0x41) ANG(0x802FECE6u) += 0x20;
        else { ST(0x8021FA4Cu) = 3; ST(0x8021FA50u) = 0; ST(0x8021FA24u) += 0x800; }
        break;
    case 3:
        if ((uint32_t)(U16(ST(0x8021FA50u)) - 0xF1) < 0x1Du) ANG(0x802FECE4u) += 1;
        if (t < 0x10E) POS(0x8031E136u) -= 10;
        else { ST(0x8021FA4Cu) = 4; ST(0x8021FA50u) = 0; ST(0x8021FA1Cu) = (int16_t)(U16(ST(0x8021FA1Cu)) & 0xFFFD); }
        break;
    case 4:
        if (t < 0x79) return;
        ST(0x8021FA4Cu) = 5; ST(0x8021FA1Cu) = (int16_t)(U16(ST(0x8021FA1Cu)) | 2); ST(0x8021FA50u) = 0;
        break;
    case 5:
        if (t < 0x32) { ANG(0x802FECE4u) += 3; ANG(0x802FECE6u) -= 1; POS(0x8031E136u) -= 1; return; }
        if (t < 0x118) { ANG(0x802FECE6u) -= 5; POS(0x8031E136u) -= 6; return; }
        ST(0x8021FA4Cu) = 6; ST(0x8021F9FCu) = 1;
        ST(0x8021FA1Cu) = (int16_t)(U16(ST(0x8021FA1Cu)) & 0xFFFD); ST(0x8021FA50u) = 0;
        break;
    case 6: {
        int d = (ST(0x8021FA34u) >> 4) * 2 + 1;
        ST(0x8021FA38u) = (int16_t)d;
        if (t == 0x66) ST(0x8021F9FCu) = 0;
        if (ST(0x8021F9FCu) == 0) {
            next = ST(0x8021FA34u) - 1;
            if (ST(0x8021FA34u) == 1) {
                ST(0x8021FA4Cu) = 7; ST(0x8021FA50u) = 0;
                ST(0x8021FA1Cu) = (int16_t)(U16(ST(0x8021FA1Cu)) & 0xFFFD); ST(0x8021FA38u) = 5;
            }
        } else {
            next = ST(0x8021FA34u) + 1;
            if (d > 10) { ST(0x8021FA38u) = 10; next = ST(0x8021FA34u); }
        }
        ST(0x8021FA34u) = (int16_t)next;
        int angle = (int16_t)(ST(0x8021FA24u) - ST(0x8021FA38u));
        ST(0x8021FA24u) = (int16_t)angle;
        circle(angle, 0x1068);
        ANG(0x802FECE6u) += ST(0x8021FA38u);
        break;
    }
    case 7:
        if (t < 300) { ANG(0x802FECE6u) += 4; if (t < 0x11E) POS(0x8031E136u) += 5; }
        if ((uint32_t)(U16(ST(0x8021FA50u)) - 0xC9) < 0x95u) ANG(0x802FECE4u) -= 1;
        if (t < 0x15F) return;
        ST(0x8021FA4Cu) = 8; ST(0x8021FA1Cu) = (int16_t)(U16(ST(0x8021FA1Cu)) & 0xFFFD); ST(0x8021FA50u) = 0;
        break;
    case 8:
        if (t < 0x79) return;
        ST(0x8021FA4Cu) = 0; ST(0x8021FA1Cu) = (int16_t)(U16(ST(0x8021FA1Cu)) | 2); ST(0x8021FA50u) = 0;
        break;
    default:
        break;
    }
}

static void f40c0(void) {
    ANG(0x802FECEEu) -= 0x29F;
    ANG(0x802FECF4u) += 0x29F;
    int flags = U16(ST(0x8021FA1Cu)), mode = ST(0x8021FA44u);
    if (!(flags & 1) || mode != 2) {
        int v = U16(ST(0x8021FA04u));
        if (v & 2) ANG(0x802FECE6u) += ST(0x8021FA08u);
        ST(0x8021FA04u) = (int16_t)(v + 1);
        if (ST(0x8021FA04u) == 0x100) { ST(0x8021FA04u) = 0; ST(0x8021FA08u) = (int16_t)-ST(0x8021FA08u); }
    }
    /* else 0x801E42DC / 0x801E4674: bit 0 is only set in mode 0, never with mode 2 */
    flags = U16(ST(0x8021FA1Cu));
    if (!(flags & 1) || !(flags & 0x1000)) {
        POS(0x8031E136u) += ST(0x8021FA00u) > 0 ? 1 : -1;
        ST(0x8021FA6Cu) += 1;
        if (ST(0x8021FA6Cu) == 0x40) { ST(0x8021FA6Cu) = 0; ST(0x8021FA00u) = (int16_t)-ST(0x8021FA00u); }
        ANG(0x802FECE8u) += ST(0x8021FA10u);
        ST(0x8021FA0Cu) += 1;
        if (ST(0x8021FA0Cu) == 0x40) { ST(0x8021FA10u) = (int16_t)-ST(0x8021FA10u); ST(0x8021FA0Cu) = 0; }
        if (!(U16(ST(0x8021FA1Cu)) & 0x1000)) return;
    }
    switch (ST(0x8021FA44u)) {
    case 1: f4a0c(); break;
    case 2: f4e04(); break;
    case 3: f54b4(); break;
    default: break;
    }
}

/* 0x801E3D0C for stage 11 (while the objects show) */
static void stage11_step(int yaw) {
    if (ST(0x8021FA48u) > 0x78) f3f68(yaw);
    else ST(0x8021FA48u) += 1;
    f40c0();
}

/* the arcade starts the objects again at each round (0x801E3D94) */
static void anim_round_start(void) {
    memset(s_st, 0, sizeof s_st); memset(s_ang, 0, sizeof s_ang); memset(s_pos, 0, sizeof s_pos);
    if (s_pack.stage == 3) stage3_init();
    else if (s_pack.stage == 11) stage11_init((int)psx_mod_read_word(0x800A08F0u));   /* as the vsync count */
}

static void anim_reset(void) {
    s_anim_count = s_anim_frame = 0;            /* 0x801E2090 */
    s_was_over = 0;
    anim_round_start();
}

/* The round is decided: a fighter's life at 0 (KO, until the next round
 * gives it back) or the timer (frames left, 2400 at the start) at 0; or
 * the match is over (phase u16 0x800AE224 past 8, the fight's; it stays 8
 * through the KO and the round winner's pose). The stage 11 objects show
 * as in the arcade (0x801E2A2C: 0x8021F0C0 >= 6, round decided, or a
 * replay): only after the fight, the user's choice (2026-10-13). */
#define PHASE   0x800AE224u
#define PAUSED  0x80095494u     /* 1 while the pause menu is open (measured; the TTT1 mod's wings read it) */
#define LIFE_P1 0x800A961Eu
#define LIFE_P2 0x800AAEAAu
#define TIMER   0x800ADBCCu
static int round_over(void) {
    int phase = psx_mod_read_half(PHASE);
    if (phase != 8) return phase > 8;           /* before 8: the round is set up, lives not given yet */
    return (int16_t)psx_mod_read_half(LIFE_P1) <= 0 || (int16_t)psx_mod_read_half(LIFE_P2) <= 0
        || psx_mod_read_half(TIMER) == 0;
}

/* Once per frame of the stage (0x801E2114 every 4th call; 0x801E3D0C),
 * not in the pause: the PS1 draws the stage under its pause menu, the
 * arcade's fight loop skips both while paused */
static void anim_tick(void) {
    Pack *p = &s_pack;
    if (psx_mod_read_word(PAUSED)) return;
    if (p->aframes && ++s_anim_count >= p->aperiod) {
        unsigned f = s_anim_frame % p->aframes;
        psx_mod_gpu_high_vram_upload(p->ax, p->ay, p->aw, p->ah, p->apixels + (size_t)f * p->aw * p->ah);
        if (s_probe) fprintf(s_probe, "tex %u %d %u\n", psx_mod_read_word(0x800A08F0u), p->stage, f);
        s_anim_count = 0;
        s_anim_frame = (s_anim_frame + 1) & 0xF;
    }
    if (!p->nlayout) return;
    int over = round_over();
    if (s_was_over && !over) anim_round_start();      /* the next round starts */
    s_was_over = over;
    if (s_probe) {          /* anim.py probe: the C port against the reference */
        fprintf(s_probe, "anim %u %d %d %d %d", psx_mod_read_word(0x800A08F0u), p->stage,
                (int)psx_mod_read_word(CAMERA + 4), over, (int)psx_mod_read_half(PHASE));
        for (int i = 0; i < 0x40; i++) fprintf(s_probe, " %d", s_st[i]);
        for (int i = 0; i < 16; i++) fprintf(s_probe, " %d", s_ang[i]);
        for (int i = 0; i < 16; i++) fprintf(s_probe, " %d", s_pos[i]);
        fprintf(s_probe, "\n");
    }
    if (p->stage == 3) stage3_step();
    else if (p->stage == 11 && over) stage11_step((int)psx_mod_read_word(CAMERA + 4));
}

/* RotMatrix (0x801F0D64) and RotMatrixYXZ (0x801F0034), table 0x8021688C:
 * a negative angle reads entry -a and negates the sine */
static void rot_sc(int a, int *s, int *c) {
    a = (int16_t)a;
    uint32_t w = s_pack.rot[(a < 0 ? -a : a) & 0xFFF];
    int sv = (int16_t)(w & 0xFFFF), cv = (int16_t)(w >> 16);
    *s = a < 0 ? -sv : sv;
    *c = cv;
}

static void rot_matrix(const int16_t *ang, int yxz, int32_t m[3][3]) {
    int sx, cx, sy, cy, sz, cz, t;
    rot_sc(ang[0], &sx, &cx); rot_sc(ang[1], &sy, &cy); rot_sc(ang[2], &sz, &cz);
    if (!yxz) {
        m[0][2] = (int16_t)sy;
        m[1][2] = (int16_t)(-(cy * sx) >> 12);
        m[2][2] = (int16_t)((cy * cx) >> 12);
        m[0][0] = (int16_t)((cz * cy) >> 12);
        m[0][1] = (int16_t)(-(sz * cy) >> 12);
        t = (cz * -sy) >> 12;
        m[1][0] = (int16_t)((int16_t)((sz * cx) >> 12) - (int16_t)((t * sx) >> 12));
        m[2][0] = (int16_t)((int16_t)((sz * sx) >> 12) + (int16_t)((t * cx) >> 12));
        t = (sz * -sy) >> 12;
        m[1][1] = (int16_t)((int16_t)((cz * cx) >> 12) + (int16_t)((t * sx) >> 12));
        m[2][1] = (int16_t)((int16_t)((cz * sx) >> 12) - (int16_t)((t * cx) >> 12));
    } else {
        m[1][2] = (int16_t)-sx;
        m[0][2] = (int16_t)((sy * cx) >> 12);
        m[2][2] = (int16_t)((cy * cx) >> 12);
        m[1][0] = (int16_t)((sz * cx) >> 12);
        m[1][1] = (int16_t)((cz * cx) >> 12);
        t = (sy * sx) >> 12;
        m[0][0] = (int16_t)((int16_t)((cy * cz) >> 12) + (int16_t)((t * sz) >> 12));
        m[0][1] = (int16_t)((int16_t)((t * cz) >> 12) - (int16_t)((cy * sz) >> 12));
        t = (cy * sx) >> 12;
        m[2][1] = (int16_t)((int16_t)((sy * sz) >> 12) + (int16_t)((t * cz) >> 12));
        m[2][0] = (int16_t)((int16_t)((t * sz) >> 12) - (int16_t)((sy * cz) >> 12));
    }
}

/* 0x801E2C64 (stage 11: model 1 hangs from model 0) and 0x801E2E80 (stage
 * 3): each model's GTE from the tiles' (anim.object_matrices) */
static void object_gte(const Gte *tile, unsigned k, Gte *out) {
    const int16_t *ang = &s_ang[4 * k], *pos = &s_pos[4 * k];
    int32_t r[3][3], v[3];
    *out = *tile;
    if (s_pack.stage == 11 && k == 1) {
        Gte parent;
        object_gte(tile, 0, &parent);
        rot_matrix(ang, 1, r);
        memcpy(out->r, r, sizeof r);
        mul_matrix2(parent.r, out->r);
        apply_matrix(parent.r, pos[0], pos[1], pos[2], v);
        for (int i = 0; i < 3; i++) out->t[i] = parent.t[i] + v[i];
        return;
    }
    rot_matrix(ang, s_pack.stage == 11, r);
    memcpy(out->r, r, sizeof r);
    mul_matrix2(tile->r, out->r);
    apply_matrix(tile->r, pos[0], pos[1], pos[2], v);
    for (int i = 0; i < 3; i++) out->t[i] = tile->t[i] + v[i];
}

/* 0x801E2A68 (scene.object_commands): each drawing call's model through
 * its GTE, the primitives facing the camera (vertices 0, 1, 2) sorted by
 * AVSZ (ZSF4 0x100, ZSF3 0x155 of SZ >> 5) into a private OT linked at its
 * main rank: 128 slots, slot 0 overwritten by the link (lost), or (size
 * 1) one slot before the link */
static void objects(const Gte *tile) {
    const Pack *p = &s_pack;
    static int sx[256], sy[256], sz[256];
    uint32_t ots[8] = {0};
    unsigned sizes[8] = {0}, ranks[8] = {0}, nots = 0;
    if (p->stage == 11 && !s_was_over) return;      /* round_over() of this frame's tick */
    for (unsigned c = 0; c < p->nlayout; c++) {
        const Layout *l = &p->layout[c];
        const Model *m = &p->models[l->model];
        unsigned o = 0;
        while (o < nots && ranks[o] != l->rank) o++;
        if (o == nots) {        /* ClearOTagR: each entry leads to the one before */
            unsigned n = l->size > 1 ? l->size : 2;
            if (l->size > 128 || nots == 8 || !(ots[o] = alloc_words(n))) return;
            for (unsigned i = 0; i < n; i++)
                psx_mod_write_word(ots[o] + 4 * i, i ? (ots[o] + 4 * (i - 1)) & 0xFFFFFFu : 0xFFFFFFu);
            sizes[o] = l->size; ranks[o] = l->rank; nots++;
        }
        Gte g;
        object_gte(tile, l->model, &g);
        for (unsigned i = 0; i < m->nv; i++) rtps(&g, m->v[i].x, m->v[i].y, m->v[i].z, &sx[i], &sy[i], &sz[i]);
        for (unsigned k = 0; k < m->np; k++) {
            const Prim *pr = &m->p[k];
            if (pr->kind > 6) continue;
            unsigned nvp = PRIM_VERTS[pr->kind], a = pr->idx[0], b = pr->idx[1], c2 = pr->idx[2], bad = 0;
            for (unsigned q = 0; q < nvp; q++) bad |= pr->idx[q] >= m->nv;
            if (bad) continue;
            int cross = (int16_t)(sx[b] - sx[a]) * (int16_t)(sy[c2] - sy[b])
                      - (int16_t)(sy[b] - sy[a]) * (int16_t)(sx[c2] - sx[b]);
            if (cross <= 0) continue;
            unsigned slot = 1;
            if (sizes[o] > 1) {
                int z = 0;
                for (unsigned q = 0; q < nvp; q++) z += sz[pr->idx[q]] >> 5;
                slot = (unsigned)((z * (nvp == 4 ? 0x100 : 0x155)) >> 12);
                if (slot > 0xFFFF) slot = 0xFFFF;
                if (slot >= sizes[o] || slot == 0) continue;      /* outside the table, or lost with slot 0 */
            }
            int behind = 0;         /* not in the arcade, as the tiles' guard */
            for (unsigned q = 0; q < nvp; q++) if (sz[pr->idx[q]] < 16) behind = 1;
            if (behind) { s_stat_obj_behind++; continue; }
            s_stat_objects += emit_prim(ots[o] + 4 * slot, pr, sx, sy);
        }
    }
    for (unsigned o = 0; o < nots; o++) {     /* entry 0 to the main rank, the main rank to the last entry */
        unsigned last = sizes[o] > 1 ? sizes[o] - 1 : 1;
        uint32_t r = main_ot(ranks[o]);
        psx_mod_write_word(ots[o], (psx_mod_read_word(ots[o]) & 0xFF000000u) | (psx_mod_read_word(r) & 0xFFFFFFu));
        psx_mod_write_word(r, (psx_mod_read_word(r) & 0xFF000000u) | ((ots[o] + 4 * last) & 0xFFFFFFu));
    }
}

/* ------------------------------------------------------------------ */
/* floor (0x801A91A8, 0x801A941C, 0x801AA9C0: scene.floor_commands)     */
/* ------------------------------------------------------------------ */
typedef struct {
    Gte world;                  /* 0x800ADF70: rotation, translation */
    int pitch, yaw, cam[3], p1[2], p2[2];
} View;

static int sqrt0(int64_t a, const int16_t *table) {
    if (a <= 0) return 0;
    uint32_t v32 = (uint32_t)a;
    int bits = 0;
    while (bits < 32 && (v32 >> bits)) bits++;
    int lz = (32 - bits) & ~1;
    uint32_t v = lz < 24 ? v32 >> (24 - lz) : v32 << (lz - 24);
    int t = table[v - 0x40];
    return (int)(((uint32_t)t << ((31 - lz) >> 1)) >> 12);
}

static int floor_center_high(const View *w, int *ox, int *oz) {
    const Pack *p = &s_pack;
    int cx = w->cam[0], cy = w->cam[1], cz = w->cam[2];
    int u = (w->pitch + 0xEF) & 0xFFF;
    int i8 = tdiv((int64_t)abs(cy) * p->cos[u], p->sin[u]);
    int i2 = sqrt0((int64_t)i8 * i8 + (int64_t)cy * cy, p->sqrt2);
    int a = (w->yaw + 0x400) & 0xFFF;
    int c = p->cos[a], s = p->sin[a];
    i2 = tdiv((int64_t)i2 << 8, 0x218);
    int x0 = shift12((int64_t)i8 * c) + cx, z0 = shift12((int64_t)i8 * s) + cz;
    int q = (w->yaw + 0x600) & 0xFFF, quarter = q >> 10, odd = q >> 9 & 1;
    int d = shift12((int64_t)(-0x4CE - cy) * 0x8555);
    if (d > 0x30D4) d = 0x30D4;
    if (quarter == 1 || quarter == 3) {
        int t = shift12((int64_t)i2 * c);
        t = odd ? t : -t;
        int z = quarter == 1 ? z0 + t - 0x4E2 + d : z0 + t + 0x4E2 - d;
        if (!s) return 0;
        *ox = tdiv((int64_t)c * (z - cz), s) + cx; *oz = z;
        return 1;
    }
    int t = shift12((int64_t)i2 * s);
    t = odd ? -t : t;
    int x = quarter == 0 ? x0 + t - 0x4E2 + d : x0 + t + 0x4E2 - d;
    if (!c) return 0;
    *ox = x; *oz = tdiv((int64_t)s * (x - cx), c) + cz;
    return 1;
}

static int floor_center(const View *w, int *ox, int *oz) {
    const Pack *p = &s_pack;
    if (p->sin[(w->pitch + 0xEF) & 0xFFF] <= 0) return 0;
    int x = tdiv((int64_t)w->p1[0] + w->p2[0], 2), z = tdiv((int64_t)w->p1[1] + w->p2[1], 2);
    if (p->lkind == 0) {
        if ((w->pitch & 0xFFF) && !(w->pitch & 0x800) && w->cam[1] < -0x6B8)
            return floor_center_high(w, ox, oz);
        if (s12(w->pitch) < -0x25) {
            int a = (w->yaw + 0x400) & 0xFFF;
            x -= shift12((int64_t)p->cos[a] * 0x9C4);
            z -= shift12((int64_t)p->sin[a] * 0x9C4);
        }
    }
    *ox = x; *oz = z;
    return 1;
}

static int snap(int v) {
    int q = tdiv((int64_t)v + 0x4E2, 0x9C4);
    return v + 0x4E2 < 0 ? q - 1 : q;
}

typedef struct { int xy[4][2]; uint8_t c[4]; } Quad;

static void floor_quad(uint32_t ot, const int xyv[4][2], const uint8_t c[4], uint32_t w0, uint32_t w1,
                       const uint16_t uvo_in[4]) {
    uint16_t o[4] = {uvo_in[0], uvo_in[1], uvo_in[2], uvo_in[3]};
    if (w0 & 0x2000000u) { uint16_t t = o[0]; o[0] = o[1]; o[1] = t; t = o[2]; o[2] = o[3]; o[3] = t; }
    if (w0 & 0x4000000u) { uint16_t t = o[0]; o[0] = o[2]; o[2] = t; t = o[1]; o[1] = o[3]; o[3] = t; }
    uint32_t base = w0 & 0xFFFFu, clut = w1 >> 16 & 0xFFFFu, tpage = (w0 & 0x39FF0000u) >> 16 & 0xFFFFu;
    uint32_t q = alloc_words(13);
    if (!q) return;
    for (unsigned k = 0; k < 4; k++) {
        uint32_t uv = (base + o[k]) & 0xFFFFu;
        uint32_t col = (uint32_t)c[k] * 0x010101u;
        psx_mod_write_word(q + 4 + 12 * k, k == 0 ? 0x3C000000u | col : col);
        psx_mod_write_word(q + 8 + 12 * k, xy(xyv[k][0], xyv[k][1]));
        psx_mod_write_word(q + 12 + 12 * k, uv | (k == 0 ? clut << 16 : k == 1 ? tpage << 16 : 0));
    }
    add_prim(ot, q, 12);
    s_stat_floor++;
}

static void floor_split(uint32_t ot, int rr, int cc, const int xyv[4][2], const uint8_t c[4], const uint16_t uvo[4]) {
    const Pack *p = &s_pack;
    int m01[2] = {tdiv(xyv[0][0] + xyv[1][0], 2), tdiv(xyv[0][1] + xyv[1][1], 2)};
    int m02[2] = {tdiv(xyv[0][0] + xyv[2][0], 2), tdiv(xyv[0][1] + xyv[2][1], 2)};
    int m23[2] = {tdiv(xyv[2][0] + xyv[3][0], 2), tdiv(xyv[2][1] + xyv[3][1], 2)};
    int m13[2] = {tdiv(xyv[1][0] + xyv[3][0], 2), tdiv(xyv[1][1] + xyv[3][1], 2)};
    int mid[2] = {tdiv(m02[0] + m13[0], 2), tdiv(m01[1] + m23[1], 2)};
    uint8_t k01 = (uint8_t)((c[0] + c[1]) >> 1), k02 = (uint8_t)((c[0] + c[2]) >> 1);
    uint8_t k13 = (uint8_t)((c[1] + c[3]) >> 1), k23 = (uint8_t)((c[2] + c[3]) >> 1);
    uint8_t kc = (uint8_t)((k01 + k02 + k13 + k23) >> 2);
    const int *pts[4][4] = {{xyv[0], m01, m02, mid}, {m01, xyv[1], mid, m13},
                            {m02, mid, xyv[2], m23}, {mid, m13, m23, xyv[3]}};
    const uint8_t cols[4][4] = {{c[0], k01, k02, kc}, {k01, c[1], kc, k13},
                                {k02, kc, c[2], k23}, {kc, k13, k23, c[3]}};
    for (int part = 0; part < 4; part++) {
        int hr = part >> 1, hc = part & 1;
        int pxy[4][2];
        for (int k = 0; k < 4; k++) { pxy[k][0] = pts[part][k][0]; pxy[k][1] = pts[part][k][1]; }
        const uint32_t *h = &p->lhalves[2 * ((2 * rr + hr) * 20 + (2 * cc + hc))];
        floor_quad(ot, pxy, cols[part], h[0], h[1], uvo);
    }
}

/* Not in the arcade, whose camera never comes this low: a floor cell with
 * a corner behind the camera was dropped and the old frame showed there
 * (the Embu's low shots); only those cells, the others are drawn as before.
 * Split in four in the world, texture and shade interpolated, until every
 * piece is wholly in front and drawable, as src/tekken3_cine_floor.c does
 * for the PS1 floor; pieces wholly behind are dropped. Corners in GT4
 * order (0 1 on one edge, 2 3 on the other). */
typedef struct { int x, z, u, v, c; } Corner;
#define NEAR_Z 16
#define NEAR_DEPTH 5
#define NEAR_PIECES 384
static unsigned s_near_pieces;

static Corner mix(const Corner *c, int s, int t, int n) {   /* bilinear at (s, t) / n */
    int64_t w[4] = {(int64_t)(n - s) * (n - t), (int64_t)s * (n - t), (int64_t)(n - s) * t, (int64_t)s * t}, nn = (int64_t)n * n;
    int64_t x = 0, z = 0, u = 0, v = 0, k = 0;
    for (int i = 0; i < 4; i++) { x += w[i] * c[i].x; z += w[i] * c[i].z; u += w[i] * c[i].u; v += w[i] * c[i].v; k += w[i] * c[i].c; }
    Corner o = {(int)((x + nn / 2) / nn), (int)((z + nn / 2) / nn), (int)((u + nn / 2) / nn), (int)((v + nn / 2) / nn),
                (int)((k + nn / 2) / nn)};
    return o;
}

static void near_piece(const Gte *g, const Screen *s, uint32_t ot, const Corner q[4], int depth, uint32_t clut,
                       uint32_t tpage) {
    int64_t sx[4], sy[4], sz[4], minx = INT64_MAX, maxx = INT64_MIN, miny = INT64_MAX, maxy = INT64_MIN;
    int near = 0, behind = 0;
    for (int k = 0; k < 4; k++) {
        rtps_raw(g, q[k].x, 0, q[k].z, &sx[k], &sy[k], &sz[k]);
        if (sz[k] < NEAR_Z) { near = 1; behind++; }
        if (sx[k] < minx) minx = sx[k];
        if (sx[k] > maxx) maxx = sx[k];
        if (sy[k] < miny) miny = sy[k];
        if (sy[k] > maxy) maxy = sy[k];
    }
    if (behind == 4) return;
    if (!near && (maxx < -s->margin || minx >= SCREEN_W + s->margin || maxy < 0 || miny >= SCREEN_H)) return;
    if (!near && maxx - minx < 1000 && maxy - miny < 500 && minx >= -0x400 && maxx <= 0x3FF && miny >= -0x400 && maxy <= 0x3FF) {
        uint32_t a = alloc_words(13);
        if (!a) return;
        /* one pixel bigger on every side: the pieces meet each other and
         * the whole cells at T-junctions, hairline cracks otherwise */
        int64_t cx = (sx[0] + sx[1] + sx[2] + sx[3]) / 4, cy = (sy[0] + sy[1] + sy[2] + sy[3]) / 4;
        for (int k = 0; k < 4; k++) {
            int x = (int)(sx[k] + (sx[k] > cx) - (sx[k] < cx)), y = (int)(sy[k] + (sy[k] > cy) - (sy[k] < cy));
            int u = q[k].u < 0 ? 0 : q[k].u > 255 ? 255 : q[k].u, v = q[k].v < 0 ? 0 : q[k].v > 255 ? 255 : q[k].v;
            uint32_t col = (uint32_t)(q[k].c & 0xFF) * 0x010101u;
            psx_mod_write_word(a + 4 + 12 * k, k == 0 ? 0x3C000000u | col : col);
            psx_mod_write_word(a + 8 + 12 * k, xy(x, y));
            psx_mod_write_word(a + 12 + 12 * k, (uint32_t)(v << 8 | u) | (k == 0 ? clut << 16 : k == 1 ? tpage << 16 : 0));
        }
        add_prim(ot, a, 12);
        s_stat_floor++;
        s_near_pieces++;
        return;
    }
    if (depth >= NEAR_DEPTH || s_near_pieces >= NEAR_PIECES) return;
    Corner m[9];                        /* 3 x 3: corners, edge middles, centre */
    for (int t = 0; t < 3; t++) for (int u = 0; u < 3; u++) m[3 * t + u] = mix(q, u, t, 2);
    static const int parts[4][4] = {{0, 1, 3, 4}, {1, 2, 4, 5}, {3, 4, 6, 7}, {4, 5, 7, 8}};
    for (int i = 0; i < 4; i++) {
        Corner c[4] = {m[parts[i][0]], m[parts[i][1]], m[parts[i][2]], m[parts[i][3]]};
        near_piece(g, s, ot, c, depth + 1, clut, tpage);
    }
}

/* A whole cell for near_piece: its corners' texels as floor_quad lays them. */
static void near_cell(const Gte *g, const Screen *s, uint32_t ot, const unsigned vi[4], const uint8_t col[4], uint32_t w0,
                      uint32_t w1, const uint16_t uvo_in[4]) {
    const Pack *p = &s_pack;
    uint16_t o[4] = {uvo_in[0], uvo_in[1], uvo_in[2], uvo_in[3]};
    if (w0 & 0x2000000u) { uint16_t t = o[0]; o[0] = o[1]; o[1] = t; t = o[2]; o[2] = o[3]; o[3] = t; }
    if (w0 & 0x4000000u) { uint16_t t = o[0]; o[0] = o[2]; o[2] = t; t = o[1]; o[1] = o[3]; o[3] = t; }
    uint32_t base = w0 & 0xFFFFu;
    Corner q[4];
    for (int k = 0; k < 4; k++) {
        uint32_t uv = (base + o[k]) & 0xFFFFu;
        q[k].x = p->lverts[2 * vi[k]]; q[k].z = p->lverts[2 * vi[k] + 1];
        q[k].u = (int)(uv & 0xFF); q[k].v = (int)(uv >> 8); q[k].c = col[k];
    }
    near_piece(g, s, ot, q, 0, w1 >> 16 & 0xFFFFu, (w0 & 0x39FF0000u) >> 16 & 0xFFFFu);
}

static void arcade_floor(const View *w, const Screen *s, int emit) {
    const Pack *p = &s_pack;
    int fx, fz;
    if (!floor_center(w, &fx, &fz)) return;
    int cx = snap(fx), cz = snap(fz);
    int col0 = pmod(tmod(cx, 10), 10), row0 = 9 - pmod(tmod(cz, 10), 10);
    Gte g = w->world;
    int32_t off[3];
    apply_matrix(w->world.r, cx * 0x9C4, 0, cz * 0x9C4, off);
    for (int i = 0; i < 3; i++) g.t[i] = off[i] + w->world.t[i];
    static int sx[256], sy[256], sz[256];
    for (unsigned i = 0; i < p->lnv; i++) rtps(&g, p->lverts[2 * i], 0, p->lverts[2 * i + 1], &sx[i], &sy[i], &sz[i]);
    s_near_pieces = 0;
    if (!emit) {            /* probe: the grid's screen points */
        if (s_probe) {
            fprintf(s_probe, "floor %u centre %d %d points", psx_mod_read_word(0x800A08F0u), cx, cz);
            for (unsigned i = 0; i < p->lnv; i++) if (sz[i]) fprintf(s_probe, " %d,%d", sx[i], sy[i]);
            fprintf(s_probe, "\n");
            /* the same transform on the PS1 floor's own grid (its cell size
             * 0x8009E780, its TMD at *0x800ADD14: u32 z | x << 16 each) */
            uint32_t tmd = psx_mod_read_word(0x800ADD14u), cell = psx_mod_read_word(0x8009E780u);
            uint32_t vp = tmd ? psx_mod_read_word(tmd + 0xC) : 0, nv = tmd ? psx_mod_read_word(tmd + 0x10) : 0;
            if (cell && nv <= 256) {
                int qx = tdiv((int64_t)fx + (int)cell / 2, cell), qz = tdiv((int64_t)fz + (int)cell / 2, cell);
                if (fx + (int)cell / 2 < 0) qx--;
                if (fz + (int)cell / 2 < 0) qz--;
                Gte g2 = w->world;
                apply_matrix(w->world.r, qx * (int)cell, 0, qz * (int)cell, off);
                for (int i = 0; i < 3; i++) g2.t[i] = off[i] + w->world.t[i];
                fprintf(s_probe, "ps1grid %u cell %u points", psx_mod_read_word(0x800A08F0u), cell);
                for (uint32_t i = 0; i < nv; i++) {
                    uint32_t v = psx_mod_read_word(vp + 4 * i);
                    int a, b, c;
                    rtps(&g2, (int16_t)(v >> 16), 0, (int16_t)(v & 0xFFFF), &a, &b, &c);
                    if (c) fprintf(s_probe, " %d,%d", a, b);
                }
                fprintf(s_probe, "\n");
            }
        }
        return;
    }
    int mx = tdiv((int64_t)w->p1[0] + w->p2[0], 2), mz = tdiv((int64_t)w->p1[1] + w->p2[1], 2);
    int64_t ddx = w->cam[0] - mx, ddz = w->cam[2] - mz;
    int e0 = sqrt0(ddx * ddx + ddz * ddz, p->sqrt1);
    int sp = p->sin[(w->pitch + 0xEF) & 0xFFF];
    int e8 = (sp ? tdiv((int64_t)abs(w->cam[1]) << 12, sp) : 0) + p->lnear;
    int shade[256];
    for (unsigned i = 0; i < p->lnv; i++) shade[i] = -1;
    unsigned split = 0;
    int rr = row0;
    for (int r = 0; r < 10; r++) {
        int cc = col0;
        for (int c = 0; c < 10; c++) {
            uint32_t w0 = p->lcells[2 * (10 * rr + cc)], w1 = p->lcells[2 * (10 * rr + cc) + 1];
            unsigned nq = p->lcount[10 * r + c];
            uint32_t ot = main_ot(1021u - p->lunique[10 * rr + cc]);
            for (unsigned q = 0; q < nq; q++) {
                uint32_t idx = p->lquads[10 * r + c][q];
                unsigned vi[4];
                int ok = 1, near = 0;
                for (int k = 0; k < 4; k++) {
                    vi[k] = idx >> (8 * k) & 0xFF;
                    if (vi[k] >= p->lnv) ok = 0;
                    else if (!sz[vi[k]]) near = 1;     /* behind the camera: near_cell, dropped before */
                }
                if (!ok) continue;
                int xyv[4][2], iny = 0, inx = 0;
                for (int k = 0; k < 4; k++) {
                    xyv[k][0] = sx[vi[k]]; xyv[k][1] = sy[vi[k]];
                    if (xyv[k][1] >= 0 && xyv[k][1] < SCREEN_H) iny = 1;
                    if ((unsigned)((xyv[k][0] + s->margin) & 0xFFFF) < (unsigned)(SCREEN_W + 2 * s->margin)) inx = 1;
                }
                if (!near && (!iny || !inx)) continue;
                if (nq != 1 && nq != 2 && nq != 4) continue;
                const uint16_t *uvo = p->luv[nq] + 4 * (nq * (w0 >> 30) + q);
                uint8_t col[4];
                for (int k = 0; k < 4; k++) {
                    unsigned i = vi[k];
                    if (p->lkind == 0 && !p->lfog) { col[k] = 0x80; continue; }
                    if (shade[i] < 0) {
                        int x = p->lverts[2 * i], z = p->lverts[2 * i + 1], v;
                        if (p->lkind == 0) {
                            int64_t d = sz[i] - e0; if (d < 0) d = 0;
                            int64_t kk = (d * 8 * 0x1E0 + d * -0x1E5) * 2;
                            kk = (kk < 0 ? kk + 0xFFFF : kk) >> 16;
                            if (kk > 0x3FF) kk = 0x3FF;
                            int64_t cv = (int64_t)p->fog[kk] * p->lfog;
                            cv = ((cv < 0 ? cv + 0x7F : cv) >> 7) + 0x80;
                            v = cv < 0 ? 0 : cv > 0xFE ? 0xFE : (int)cv;
                        } else if (p->lkind == 1) {
                            int64_t a1 = w->p1[0] - (int64_t)cx * 0x9C4 - x, b1 = w->p1[1] - (int64_t)cz * 0x9C4 - z;
                            int64_t a2 = w->p2[0] - (int64_t)cx * 0x9C4 - x, b2 = w->p2[1] - (int64_t)cz * 0x9C4 - z;
                            int64_t d1 = a1 * a1 + b1 * b1, d2 = a2 * a2 + b2 * b2;
                            if (!d1) d1 = 1;
                            if (!d2) d2 = 1;
                            int64_t cv = 0xF200000 / d1 + 0xF200000 / d2;
                            v = cv > 0xFE ? 0xFE : (int)cv;
                        } else {
                            int64_t a = mx - (x + (int64_t)cx * 0x9C4), b = mz - (z + (int64_t)cz * 0x9C4);
                            int64_t d = a * a + b * b;
                            if (!d) d = 1;
                            int64_t cv = 0x7A120000 / d;
                            v = cv > 0xC0 ? 0xC0 : (int)cv;
                        }
                        shade[i] = v & 0xFF;
                    }
                    col[k] = (uint8_t)shade[i];
                }
                if (near) {
                    near_cell(&g, s, ot, vi, col, w0 & 0x3FFFFFFFu, w1, uvo);
                    continue;
                }
                int nearq = ((uint32_t)(w->pitch - 0x400)) < 0x201u;
                for (int k = 0; k < 4; k++) if (sz[vi[k]] < e8) nearq = 1;
                if (split < 11 && nearq) {
                    floor_split(ot, rr, cc, xyv, col, uvo);
                    split++;
                } else {
                    floor_quad(ot, xyv, col, w0 & 0x3FFFFFFFu, w1, uvo);
                }
            }
            cc = cc == 9 ? 0 : cc + 1;
        }
        rr = rr == 0 ? 9 : rr - 1;
    }
}

static void read_gte_screen(const CPUState *cpu, Screen *s, Gte *g) {
    s->ofx = (int)cpu->gte_ctrl[24] >> 16;
    s->ofy = (int)cpu->gte_ctrl[25] >> 16;
    s->h = (int)(cpu->gte_ctrl[26] & 0xFFFF);
    s->margin = psx_mod_widescreen_x_margin();
    if (s->margin < 0) s->margin = 0;
    g->ofx = (int32_t)cpu->gte_ctrl[24];
    g->ofy = (int32_t)cpu->gte_ctrl[25];
    g->h = cpu->gte_ctrl[26] & 0xFFFF;
}

/* The PS1 floor, kinds 0 (0x80048E58) and 1-3 (0x80049848): a0, a1 the
 * fighters, the world matrix 0x800ADF70 just built by 0x80045CA8. */
static int floor_draw(CPUState *cpu) {
    if (!active()) return 0;
    int pitch = (int)psx_mod_read_word(CAMERA);
    if (!s_shadow) psx_mod_write_word(FLOOR_SHOWN, s_sin[(pitch + 0xEF) & 0xFFF] > 0);   /* as 0x80048E58 / 0x80049848 */
    View w;
    Screen s;
    read_gte_screen(cpu, &s, &w.world);
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) w.world.r[i][k] = (int16_t)psx_mod_read_half(WORLD + 6 * i + 2 * k);
    for (int i = 0; i < 3; i++) w.world.t[i] = (int32_t)psx_mod_read_word(WORLD + 0x14 + 4 * i);
    w.pitch = pitch;
    w.yaw = (int)psx_mod_read_word(CAMERA + 4);
    for (int i = 0; i < 3; i++) w.cam[i] = (int)psx_mod_read_word(CAMERA + 0x14 + 4 * i);
    w.p1[0] = (int)psx_mod_read_word(cpu->gpr[4] + 0xF68); w.p1[1] = (int)psx_mod_read_word(cpu->gpr[4] + 0xF70);
    w.p2[0] = (int)psx_mod_read_word(cpu->gpr[5] + 0xF68); w.p2[1] = (int)psx_mod_read_word(cpu->gpr[5] + 0xF70);
    s_cursor = s_memory + BUFFER_BYTES * buffer();
    s_end = s_cursor + FLOOR_BYTES;
    s_stat_floor = 0;
    arcade_floor(&w, &s, !s_shadow);
    return !s_shadow;
}

static int floor_hook(CPUState *cpu) {
    if (!perf_on()) return floor_draw(cpu);
    uint64_t t = now_ns();
    int r = floor_draw(cpu);
    s_perf_floor = now_ns() - t;
    return r;
}

extern void __real_func_80048E58(CPUState *cpu);
void __wrap_func_80048E58(CPUState *cpu) {
    if ((cpu->pc == 0 || cpu->pc == 0x80048E58u) && floor_hook(cpu)) { cpu->pc = cpu->gpr[31]; return; }
    __real_func_80048E58(cpu);
}

extern void __real_func_80049848(CPUState *cpu);
void __wrap_func_80049848(CPUState *cpu) {
    if ((cpu->pc == 0 || cpu->pc == 0x80049848u) && floor_hook(cpu)) { cpu->pc = cpu->gpr[31]; return; }
    __real_func_80049848(cpu);
}

/* The Embu's floor. 0x8004808C, which picks the floor routine at each
 * stage load, gives the Embu (state 6) the overlay's 0x800D494C: a grid of
 * 24 FT4 that only meets the PS1 tiles, so under the arcade tiles it left
 * slits and gaps (the screen is never cleared: old frames showed through)
 * and the arcade tiles' own ground showed around it in another shade. With
 * the arcade stage drawing, the pointer is turned to 0x80048E58 for the
 * Embu, whose hook draws the arcade floor; put back if the stage stops
 * drawing (the next stage load sets it anyway). */
static int s_embu_floor;

static void embu_floor(CPUState *cpu, uint32_t entry) {
    if (cpu->pc != 0 && cpu->pc != entry) return;
    uint32_t fn = psx_mod_read_word(FLOOR_FN);
    if (fn == EMBU_FLOOR && !s_shadow && active()) {
        psx_mod_write_word(FLOOR_FN, KIND0_FLOOR);
        s_embu_floor = 1;
        if (log_on()) { fprintf(stdout, "arcade stages: the Embu's floor drawn by the arcade floor\n"); fflush(stdout); }
    } else if (s_embu_floor) {
        if (fn == KIND0_FLOOR && (int16_t)psx_mod_read_half(0x800AE204u) == 6 && !active())
            psx_mod_write_word(FLOOR_FN, EMBU_FLOOR);
        if (fn != KIND0_FLOOR || (int16_t)psx_mod_read_half(0x800AE204u) != 6) s_embu_floor = 0;
    }
}

extern void __real_func_80048240(CPUState *cpu);
void __wrap_func_80048240(CPUState *cpu) {
    embu_floor(cpu, 0x80048240u);
    __real_func_80048240(cpu);
}

extern void __real_func_80048208(CPUState *cpu);
void __wrap_func_80048208(CPUState *cpu) {
    embu_floor(cpu, 0x80048208u);
    __real_func_80048208(cpu);
}

/* ------------------------------------------------------------------ */
/* the tiles: after 0x8006D014 returns                                */
/* ------------------------------------------------------------------ */
static uint32_t s_ret, s_sp;
static int s_axis_before, s_running, s_probe_gte;
static int32_t s_ps1_gte[8];

static void probe_line(const Gte *mine, const Gte *ps1) {
    if (!s_probe) return;
    fprintf(s_probe, "frame %u arena %u axis %d cam", psx_mod_read_word(0x800A08F0u), psx_mod_read_half(ARENA),
            (int)psx_mod_read_word(AXIS));
    for (int i = 0; i < 8; i++) fprintf(s_probe, " %d", (int)psx_mod_read_word(CAMERA + 4 * i));
    fprintf(s_probe, " | gte_ps1 %d", s_probe_gte);
    for (int i = 0; i < 8; i++) fprintf(s_probe, " %08X", (unsigned)s_ps1_gte[i]);
    fprintf(s_probe, " | port_ps1");
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) fprintf(s_probe, " %d", ps1->r[i][k]);
    for (int i = 0; i < 3; i++) fprintf(s_probe, " %d", ps1->t[i]);
    fprintf(s_probe, " | arcade");
    for (int i = 0; i < 3; i++) fprintf(s_probe, " %d", mine->t[i]);
    fprintf(s_probe, " | screen %d %d %u\n", mine->ofx >> 16, mine->ofy >> 16, mine->h);
    fflush(s_probe);
}

static void stage_draw(CPUState *cpu) {
    Screen s;
    Gte g;
    read_gte_screen(cpu, &s, &g);
    tile_matrix(&g, 0);
    if (s_probe) {
        Gte ps1 = g;
        tile_matrix(&ps1, 1);
        probe_line(&g, &ps1);
    }
    if (s_shadow) {         /* the PS1 floor this frame: its GT4 quads at ranks 1021..982 */
        if (!s_probe) return;
        uint32_t end = main_ot(981) & 0xFFFFFFu, lo = main_ot(0) & 0xFFFFFFu, hi = main_ot(1024) & 0xFFFFFFu;
        uint32_t a = psx_mod_read_word(main_ot(1021)) & 0xFFFFFFu;
        fprintf(s_probe, "ps1floor %u points", psx_mod_read_word(0x800A08F0u));
        for (unsigned n = 0; a != end && a != 0xFFFFFFu && n < 20000; n++) {
            uint32_t tag = psx_mod_read_word(0x80000000u | a);
            if ((a < lo || a >= hi) && tag >> 24 == 12 && (psx_mod_read_byte(0x80000007u | a) & 0xFC) == 0x3C)
                for (unsigned k = 0; k < 4; k++) {
                    uint32_t v = psx_mod_read_word(0x80000000u | (a + 8 + 12 * k));
                    fprintf(s_probe, " %d,%d", (int16_t)(v & 0xFFFF), (int16_t)(v >> 16));
                }
            a = tag & 0xFFFFFFu;
        }
        fprintf(s_probe, "\n");
        return;
    }
    unsigned b = buffer();
    s_cursor = s_memory + BUFFER_BYTES * b + FLOOR_BYTES;
    s_end = s_memory + BUFFER_BYTES * (b + 1);
    s_stat_tiles = s_stat_far = s_stat_cone = s_stat_behind = s_stat_objects = s_stat_obj_behind = 0;
    anim_tick();
    /* two private OTs of 16 (A then B), ClearOTagR'd, linked as 0x801D8FD0
     * links them: A at main rank 1010, B at 1021, every stage */
    uint32_t ot_a = alloc_words(16), ot_b = alloc_words(16);
    if (!ot_a || !ot_b) return;
    for (unsigned i = 1; i < 16; i++) {
        psx_mod_write_word(ot_a + 4 * i, (ot_a + 4 * (i - 1)) & 0xFFFFFFu);
        psx_mod_write_word(ot_b + 4 * i, (ot_b + 4 * (i - 1)) & 0xFFFFFFu);
    }
    uint32_t r1010 = main_ot(1010), r1021 = main_ot(1021);
    psx_mod_write_word(ot_a, psx_mod_read_word(r1010) & 0xFFFFFFu);
    psx_mod_write_word(ot_b, psx_mod_read_word(r1021) & 0xFFFFFFu);
    tiles(&g, &s, ot_a, ot_b);
    objects(&g);
    psx_mod_write_word(r1010, (psx_mod_read_word(r1010) & 0xFF000000u) | ((ot_a + 60) & 0xFFFFFFu));
    psx_mod_write_word(r1021, (psx_mod_read_word(r1021) & 0xFF000000u) | ((ot_b + 60) & 0xFFFFFFu));
    /* the PS1 tiles dropped: its private OT (2 slots, linked by 0x8006D7CC
     * at rank 1010, arena 11: 1021, once we return) left empty, slot 1
     * leading straight to slot 0 */
    uint32_t priv = PRIVATE_OT + 8u * b;
    psx_mod_write_word(priv + 4, (psx_mod_read_word(priv + 4) & 0xFF000000u) | (priv & 0xFFFFFFu));
    far_background(&s, (int)psx_mod_read_word(CAMERA + 4) - s_axis_before, (int)psx_mod_read_word(CAMERA));
    if (log_on() && (++s_frames % 120) == 1) {
        fprintf(stdout, "arcade stages: frame %u, %u tiles in the cone, %u tile primitives (%u behind the camera "
                "dropped), %u object primitives (%u dropped), %u floor quads, %u far background, %u bytes, phase %d\n",
                psx_mod_read_word(0x800A08F0u), s_stat_cone, s_stat_tiles, s_stat_behind, s_stat_objects,
                s_stat_obj_behind, s_stat_floor, s_stat_far, (unsigned)(s_cursor - (s_memory + BUFFER_BYTES * b)),
                (int)psx_mod_read_half(PHASE));
        fflush(stdout);
    }
}

static void stage_frame(CPUState *cpu) {
    if (!perf_on()) { stage_draw(cpu); return; }
    uint64_t t = now_ns();
    stage_draw(cpu);
    perf_frame(s_perf_floor + (now_ns() - t));
    s_perf_floor = 0;
}

extern void __real_func_8006D014(CPUState *cpu);
void __wrap_func_8006D014(CPUState *cpu) {
    if (cpu->pc == 0 || cpu->pc == 0x8006D014u) {
        s_running = active();
        if (s_running) {
            s_ret = cpu->gpr[31];
            s_sp = cpu->gpr[29];
            s_axis_before = (int)psx_mod_read_word(AXIS);
            s_probe_gte = 0;
        }
    }
    __real_func_8006D014(cpu);
    if (!s_running) return;
    if (s_probe && !s_probe_gte && cpu->pc == 0x80036AF8u) {       /* first tile: the PS1's GTE */
        for (int i = 0; i < 8; i++) s_ps1_gte[i] = (int32_t)cpu->gte_ctrl[i];
        s_probe_gte = 1;
    }
    if (cpu->pc == s_ret && cpu->gpr[29] == s_sp) {
        s_running = 0;
        stage_frame(cpu);
    }
}

/* 0x80048374 (a0 = buffer): a TILE of the arena's colour (0x80097A64)
 * over the whole screen (368 x 480 in fights), main OT rank 1023, drawn
 * first; 0x8006D7CC calls it after the stage for arena 11 only (and for
 * the dark arenas, which keep the PS1 stage). Not part of the arcade
 * stage: skipped while ours draws. */
extern void __real_func_80048374(CPUState *cpu);
void __wrap_func_80048374(CPUState *cpu) {
    if ((cpu->pc == 0 || cpu->pc == 0x80048374u) && !s_shadow && s_pack.stage == 11 && active()) {
        cpu->pc = cpu->gpr[31];
        return;
    }
    __real_func_80048374(cpu);
}

/* ------------------------------------------------------------------ */
/* Any pack of the 13 present (the setup makes them all from tekken3.zip). */
static int packs_present(void) {
    char path[1024];
    for (int arena = 0; arena <= 12; arena++) {
        snprintf(path, sizeof path, "%s/stage%02d.ast", pack_dir(), arena);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}

static void tekken3_arcade_stages_activate(void) {
    /* No packs (no tekken3.zip at setup): nothing allocated, the game as
     * with the feature off. */
    if (!packs_present()) {
        fprintf(stdout, "arcade stages: no stage packs in %s, PS1 stages kept\n", pack_dir());
        return;
    }
    if (!psx_mod_gpu_high_vram_enable()) {
        fprintf(stdout, "arcade stages: no upper VRAM half, PS1 stages kept\n");
        return;
    }
    s_memory = psx_mod_alloc_gpu_dma_memory(2 * BUFFER_BYTES, 16);
    if (!s_memory || !psx_mod_gpu_high_vram_source(s_memory, 2 * BUFFER_BYTES)) {
        fprintf(stdout, "arcade stages: no packet memory, PS1 stages kept\n");
        return;
    }
    unr_init();
    s_pack.stage = -1;
    const char *probe = getenv("TEKKEN3_ARCADE_STAGES_PROBE");
    if (probe && *probe) s_probe = fopen(probe, "w");
    s_shadow = s_probe && getenv("TEKKEN3_ARCADE_STAGES_SHADOW");
    s_on = 1;
    fprintf(stdout, "arcade stages: on, packs from %s\n", pack_dir());
    fflush(stdout);
}

PSX_MOD_CONSTRUCTOR(tekken3_register_arcade_stages) {
    (void)psx_mod_register_activation_plugin("tekken3.arcade-stages",
                                             tekken3_arcade_stages_activate);
}
