/* vr_settings.c — VR settings file, the in-headset settings menu, and the
 * controller -> PS1 pad mapping. See vr_settings.h. */
#include "vr_settings.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- settings ------------------------------------------------------------- */
typedef struct {
    const char *key;
    size_t      offset;
    int         min, max, def;
} SettingField;

#define FIELD(name, lo, hi, d) { #name, offsetof(PsxVrSettings, name), lo, hi, d }
static const SettingField FIELDS[] = {
    FIELD(stereo,       0,    1,    1),
    FIELD(strength,     0,    60,   18),    /* 1.8 % of the width at the far plane */
    FIELD(convergence,  40,   250,  100),
    FIELD(flat_depth,  -100,  100, -100),   /* 2D layers sit with the far background */
    FIELD(fov,          30,   120,  70),
    FIELD(distance_cm,  50,   800,  250),
    FIELD(height_cm,   -150,  150,  0),
    FIELD(follow_head,  0,    1,    0),
    FIELD(eye_width,    640,  2560, 1600),
    FIELD(refresh_hz,   0,    144,  0),
    FIELD(swap_eyes,    0,    1,    0),
    FIELD(quality,      4,    48,   20),
};
#define NFIELDS ((int)(sizeof FIELDS / sizeof FIELDS[0]))

static int *field_ptr(PsxVrSettings *s, int i) { return (int *)((char *)s + FIELDS[i].offset); }

void psx_vr_settings_defaults(PsxVrSettings *s) {
    for (int i = 0; i < NFIELDS; i++) *field_ptr(s, i) = FIELDS[i].def;
}

void psx_vr_settings_clamp(PsxVrSettings *s) {
    for (int i = 0; i < NFIELDS; i++) {
        int *v = field_ptr(s, i);
        if (*v < FIELDS[i].min) *v = FIELDS[i].min;
        if (*v > FIELDS[i].max) *v = FIELDS[i].max;
    }
}

int psx_vr_settings_load(PsxVrSettings *s, const char *path) {
    psx_vr_settings_defaults(s);
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';') continue;
        *eq = '\0';
        char *key = line;
        while (*key == ' ' || *key == '\t') key++;
        char *end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        for (int i = 0; i < NFIELDS; i++)
            if (strcmp(key, FIELDS[i].key) == 0)
                *field_ptr(s, i) = atoi(eq + 1);
    }
    fclose(f);
    psx_vr_settings_clamp(s);
    return 1;
}

int psx_vr_settings_save(const PsxVrSettings *s, const char *path) {
    FILE *f = path ? fopen(path, "w") : NULL;
    if (!f) return 0;
    fputs("# Tekken 3 Expanded VR settings (the in-headset menu writes this file)\n", f);
    for (int i = 0; i < NFIELDS; i++)
        fprintf(f, "%s=%d\n", FIELDS[i].key, *field_ptr((PsxVrSettings *)s, i));
    return fclose(f) == 0;
}

/* ---- menu ----------------------------------------------------------------- */
static const int EYE_WIDTHS[] = { 960, 1280, 1440, 1600, 1920, 2160, 2560 };
#define NWIDTHS ((int)(sizeof EYE_WIDTHS / sizeof EYE_WIDTHS[0]))

static int step_int(int v, int dir, int step, int lo, int hi) {
    v += dir * step;
    return v < lo ? lo : v > hi ? hi : v;
}

PsxVrMenuAction psx_vr_menu_step(PsxVrSettings *s, int item, int dir, const PsxVrRates *rates) {
    PsxVrSettings before = *s;
    switch (item) {
    case PSX_VR_ITEM_RESUME:   return dir == 0 ? PSX_VR_MENU_CLOSE : PSX_VR_MENU_NONE;
    case PSX_VR_ITEM_RECENTER: return dir == 0 ? PSX_VR_MENU_RECENTER : PSX_VR_MENU_NONE;
    case PSX_VR_ITEM_DEFAULTS:
        if (dir != 0) return PSX_VR_MENU_NONE;
        psx_vr_settings_defaults(s);
        break;
    case PSX_VR_ITEM_STEREO:      s->stereo = !s->stereo; break;
    case PSX_VR_ITEM_FOLLOW_HEAD: s->follow_head = !s->follow_head; break;
    case PSX_VR_ITEM_SWAP_EYES:   s->swap_eyes = !s->swap_eyes; break;
    case PSX_VR_ITEM_STRENGTH:    s->strength = step_int(s->strength, dir ? dir : 1, 2, 0, 60); break;
    case PSX_VR_ITEM_CONVERGENCE: s->convergence = step_int(s->convergence, dir ? dir : 1, 10, 40, 250); break;
    case PSX_VR_ITEM_FLAT_DEPTH:  s->flat_depth = step_int(s->flat_depth, dir ? dir : 1, 10, -100, 100); break;
    case PSX_VR_ITEM_FOV:         s->fov = step_int(s->fov, dir ? dir : 1, 5, 30, 120); break;
    case PSX_VR_ITEM_DISTANCE:    s->distance_cm = step_int(s->distance_cm, dir ? dir : 1, 25, 50, 800); break;
    case PSX_VR_ITEM_HEIGHT:      s->height_cm = step_int(s->height_cm, dir ? dir : 1, 10, -150, 150); break;
    case PSX_VR_ITEM_QUALITY:     s->quality = step_int(s->quality, dir ? dir : 1, 4, 4, 48); break;
    case PSX_VR_ITEM_RESOLUTION: {
        int cur = 0;
        for (int i = 0; i < NWIDTHS; i++) if (EYE_WIDTHS[i] <= s->eye_width) cur = i;
        int d = dir ? dir : 1;
        cur += d;
        if (cur < 0) cur = dir ? 0 : NWIDTHS - 1;
        if (cur >= NWIDTHS) cur = dir ? NWIDTHS - 1 : 0;
        s->eye_width = EYE_WIDTHS[cur];
        break;
    }
    case PSX_VR_ITEM_REFRESH: {
        if (!rates || rates->count <= 0) return PSX_VR_MENU_NONE;
        /* 0 (headset default) comes before the offered rates. */
        int cur = -1;
        for (int i = 0; i < rates->count; i++) if (rates->hz[i] == s->refresh_hz) cur = i;
        int d = dir ? dir : 1;
        cur += d;
        if (cur >= rates->count) cur = dir ? rates->count - 1 : -1;
        if (cur < -1) cur = -1;
        s->refresh_hz = cur < 0 ? 0 : rates->hz[cur];
        break;
    }
    default: return PSX_VR_MENU_NONE;
    }
    psx_vr_settings_clamp(s);
    return memcmp(&before, s, sizeof before) ? PSX_VR_MENU_CHANGED : PSX_VR_MENU_NONE;
}

void psx_vr_menu_text(const PsxVrSettings *s, int item, char *label, int label_cap,
                      char *value, int value_cap) {
    const char *l = "";
    value[0] = '\0';
    switch (item) {
    case PSX_VR_ITEM_RESUME:      l = "Resume game"; break;
    case PSX_VR_ITEM_STEREO:      l = "3D (stereo)";
        snprintf(value, value_cap, "%s", s->stereo ? "On" : "Off"); break;
    case PSX_VR_ITEM_STRENGTH:    l = "Depth strength";
        snprintf(value, value_cap, "%d.%d %%", s->strength / 10, s->strength % 10); break;
    case PSX_VR_ITEM_CONVERGENCE: l = "Screen plane depth";
        snprintf(value, value_cap, "%d %%", s->convergence); break;
    case PSX_VR_ITEM_FLAT_DEPTH:  l = "2D layers (HUD) depth";
        snprintf(value, value_cap, "%s%d", s->flat_depth > 0 ? "+" : "", s->flat_depth); break;
    case PSX_VR_ITEM_FOV:         l = "Field of view (screen size)";
        snprintf(value, value_cap, "%d deg", s->fov); break;
    case PSX_VR_ITEM_DISTANCE:    l = "Screen distance";
        snprintf(value, value_cap, "%d.%02d m", s->distance_cm / 100, s->distance_cm % 100); break;
    case PSX_VR_ITEM_HEIGHT:      l = "Screen height";
        snprintf(value, value_cap, "%s%d cm", s->height_cm > 0 ? "+" : "", s->height_cm); break;
    case PSX_VR_ITEM_FOLLOW_HEAD: l = "Screen follows head";
        snprintf(value, value_cap, "%s", s->follow_head ? "On" : "Off"); break;
    case PSX_VR_ITEM_RESOLUTION:  l = "Resolution (per eye)";
        snprintf(value, value_cap, "%d px", s->eye_width); break;
    case PSX_VR_ITEM_REFRESH:     l = "Target FPS (refresh)";
        if (s->refresh_hz > 0) snprintf(value, value_cap, "%d Hz", s->refresh_hz);
        else snprintf(value, value_cap, "Default");
        break;
    case PSX_VR_ITEM_QUALITY:     l = "Depth quality";
        snprintf(value, value_cap, "%d", s->quality); break;
    case PSX_VR_ITEM_SWAP_EYES:   l = "Swap eyes";
        snprintf(value, value_cap, "%s", s->swap_eyes ? "On" : "Off"); break;
    case PSX_VR_ITEM_RECENTER:    l = "Recenter view"; break;
    case PSX_VR_ITEM_DEFAULTS:    l = "Reset VR settings"; break;
    default: break;
    }
    snprintf(label, label_cap, "%s", l);
}

/* Public-domain 8x8 ASCII (32..126), row bitmasks LSB = left pixel, from the
 * font8x8_basic set (https://github.com/d7samurai/font8x8), as host_osd.c. */
static const uint8_t FONT8X8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* # */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* % */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 9 */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* : */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* ; */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* < */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* D */
    {0x7F,0x06,0x06,0x3E,0x06,0x06,0x7F,0x00}, /* E */
    {0x7F,0x06,0x06,0x3E,0x06,0x06,0x06,0x00}, /* F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* K */
    {0x06,0x06,0x06,0x06,0x06,0x06,0x7F,0x00}, /* L */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x06,0x00}, /* P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* [ */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* c */
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, /* d */
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, /* e */
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0f,0x00}, /* f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* v */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* z */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};

static void fill(uint8_t *px, int w, int h, int x0, int y0, int rw, int rh, uint32_t rgba) {
    for (int y = y0 < 0 ? 0 : y0; y < y0 + rh && y < h; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x0 + rw && x < w; x++) {
            uint8_t *p = px + ((size_t)y * w + x) * 4;
            p[0] = (uint8_t)(rgba >> 24); p[1] = (uint8_t)(rgba >> 16);
            p[2] = (uint8_t)(rgba >> 8);  p[3] = (uint8_t)rgba;
        }
}

static int text(uint8_t *px, int w, int h, int x, int y, const char *s, int scale, uint32_t rgba) {
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) c = '?';
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++)
                if (FONT8X8[c - 32][yy] & (1u << xx))
                    fill(px, w, h, x + (i * 8 + xx) * scale, y + yy * scale, scale, scale, rgba);
    }
    return (int)strlen(s) * 8 * scale;
}

void psx_vr_menu_draw(const PsxVrSettings *s, int selected, uint8_t *rgba, int w, int h) {
    const uint32_t bg = 0x0E1016F0u, panel = 0x181B24FFu, hi = 0x6E56CFFFu;
    const uint32_t ink = 0xECECF2FFu, dim = 0x9AA0B4FFu;
    fill(rgba, w, h, 0, 0, w, h, bg);
    fill(rgba, w, h, 0, 0, w, 4, hi);
    const int scale = h >= 700 ? 3 : 2, glyph = 8 * scale;
    const int margin = glyph, title_h = glyph * 3;
    text(rgba, w, h, margin, glyph, "VR settings", scale, ink);
    const int footer_h = glyph * 2;
    int row_h = (h - title_h - footer_h) / PSX_VR_ITEM_COUNT;
    if (row_h < glyph + 4) row_h = glyph + 4;
    for (int i = 0; i < PSX_VR_ITEM_COUNT; i++) {
        char label[48], value[32];
        psx_vr_menu_text(s, i, label, sizeof label, value, sizeof value);
        int y = title_h + i * row_h;
        if (i == selected) fill(rgba, w, h, margin / 2, y, w - margin, row_h - 2, hi);
        else if (i & 1) fill(rgba, w, h, margin / 2, y, w - margin, row_h - 2, panel);
        int ty = y + (row_h - glyph) / 2;
        text(rgba, w, h, margin, ty, label, scale, ink);
        if (value[0]) {
            int vw = (int)strlen(value) * glyph;
            char arrows[40];
            snprintf(arrows, sizeof arrows, "< %s >", value);
            if (i == selected) vw = (int)strlen(arrows) * glyph;
            text(rgba, w, h, w - margin - vw, ty, i == selected ? arrows : value, scale,
                 i == selected ? ink : dim);
        }
    }
    text(rgba, w, h, margin, h - footer_h + glyph / 2,
         "Stick: choose/change  A: select  B or stick click: close", scale > 2 ? 2 : scale, dim);
}

/* ---- controllers ------------------------------------------------------------ */
enum {
    PAD_SELECT = 0x0001, PAD_L3 = 0x0002, PAD_R3 = 0x0004, PAD_START = 0x0008,
    PAD_UP = 0x0010, PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080,
    PAD_L2 = 0x0100, PAD_R2 = 0x0200, PAD_L1 = 0x0400, PAD_R1 = 0x0800,
    PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000,
};
#define STICK_ON  0.5f
#define PULL_ON   0.5f

uint16_t psx_vr_pad_word_from(const PsxVrControls *c) {
    uint16_t held = 0;
    if (c->stick_ly >  STICK_ON) held |= PAD_UP;
    if (c->stick_ly < -STICK_ON) held |= PAD_DOWN;
    if (c->stick_lx >  STICK_ON) held |= PAD_RIGHT;
    if (c->stick_lx < -STICK_ON) held |= PAD_LEFT;
    if (c->trigger_r > PULL_ON) held |= PAD_SQUARE;
    if (c->b)                   held |= PAD_TRIANGLE;
    if (c->grip_r > PULL_ON)    held |= PAD_CROSS;
    if (c->a)                   held |= PAD_CIRCLE;
    if (c->x)                   held |= PAD_L1;
    if (c->y)                   held |= PAD_R1;
    if (c->trigger_l > PULL_ON) held |= PAD_L2;
    if (c->grip_l > PULL_ON)    held |= PAD_R2;
    if (c->menu)                held |= PAD_START;
    if (c->stick_click_r)       held |= PAD_SELECT;
    return (uint16_t)~held;
}

static int stick_dir(const PsxVrControls *c) {
    /* Either stick navigates; the stronger axis wins. */
    float x = c->stick_lx, y = c->stick_ly;
    if (c->stick_rx * c->stick_rx + c->stick_ry * c->stick_ry > x * x + y * y) {
        x = c->stick_rx; y = c->stick_ry;
    }
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (ax < STICK_ON && ay < STICK_ON) return 0;
    if (ay >= ax) return y > 0 ? PSX_VR_NAV_UP : PSX_VR_NAV_DOWN;
    return x > 0 ? PSX_VR_NAV_RIGHT : PSX_VR_NAV_LEFT;
}

PsxVrNav psx_vr_nav(PsxVrNavState *st, const PsxVrControls *c, double now) {
    PsxVrNav out = PSX_VR_NAV_NONE;
    if (c->stick_click_l && !st->prev.stick_click_l) out = PSX_VR_NAV_TOGGLE;
    else if (c->a && !st->prev.a) out = PSX_VR_NAV_ACCEPT;
    else if (c->b && !st->prev.b) out = PSX_VR_NAV_BACK;
    else {
        int dir = stick_dir(c);
        if (dir != st->held_dir) {
            st->held_dir = dir;
            st->next_repeat = now + 0.40;
            out = (PsxVrNav)dir;
        } else if (dir && now >= st->next_repeat) {
            st->next_repeat = now + 0.12;
            out = (PsxVrNav)dir;
        }
    }
    st->prev = *c;
    return out;
}
