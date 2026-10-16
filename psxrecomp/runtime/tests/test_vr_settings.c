/* vr_settings.c: the VR settings file, the in-headset menu's steps and the
 * controller -> PS1 pad mapping (PSX_VR builds). */
#include "vr_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void test_file_roundtrip(void) {
    PsxVrSettings a, b;
    psx_vr_settings_defaults(&a);
    a.strength = 30; a.fov = 95; a.follow_head = 1; a.refresh_hz = 90; a.height_cm = -40;
    char path[64];
    snprintf(path, sizeof path, "vr_settings_test_%d.ini", (int)rand());
    CHECK(psx_vr_settings_save(&a, path));
    psx_vr_settings_defaults(&b);
    CHECK(psx_vr_settings_load(&b, path));
    CHECK(memcmp(&a, &b, sizeof a) == 0);
    remove(path);

    /* Out-of-range values are clamped; unknown keys and junk are ignored. */
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fprintf(f, "# comment\nfov=500\nstrength=-3\nbogus=1\nnot a line\ndistance_cm=10\n");
    fclose(f);
    psx_vr_settings_defaults(&b);
    CHECK(psx_vr_settings_load(&b, path));
    CHECK(b.fov == 120);
    CHECK(b.strength == 0);
    CHECK(b.distance_cm == 50);
    remove(path);
    CHECK(!psx_vr_settings_load(&b, path));   /* missing file */
}

static void test_menu_steps(void) {
    PsxVrSettings s;
    psx_vr_settings_defaults(&s);
    const int fov = s.fov;
    CHECK(psx_vr_menu_step(&s, PSX_VR_ITEM_FOV, 1, NULL) == PSX_VR_MENU_CHANGED);
    CHECK(s.fov == fov + 5);
    for (int i = 0; i < 40; i++) psx_vr_menu_step(&s, PSX_VR_ITEM_FOV, 1, NULL);
    CHECK(s.fov == 120);
    CHECK(psx_vr_menu_step(&s, PSX_VR_ITEM_FOV, 1, NULL) == PSX_VR_MENU_NONE);   /* at the top */

    const int stereo = s.stereo;
    CHECK(psx_vr_menu_step(&s, PSX_VR_ITEM_STEREO, 0, NULL) == PSX_VR_MENU_CHANGED);
    CHECK(s.stereo == !stereo);

    CHECK(psx_vr_menu_step(&s, PSX_VR_ITEM_RESUME, 0, NULL) == PSX_VR_MENU_CLOSE);
    CHECK(psx_vr_menu_step(&s, PSX_VR_ITEM_RESUME, 1, NULL) == PSX_VR_MENU_NONE);
    CHECK(psx_vr_menu_step(&s, PSX_VR_ITEM_RECENTER, 0, NULL) == PSX_VR_MENU_RECENTER);

    /* Resolution walks the fixed widths. */
    s.eye_width = 1600;
    psx_vr_menu_step(&s, PSX_VR_ITEM_RESOLUTION, 1, NULL);
    CHECK(s.eye_width == 1920);
    psx_vr_menu_step(&s, PSX_VR_ITEM_RESOLUTION, -1, NULL);
    psx_vr_menu_step(&s, PSX_VR_ITEM_RESOLUTION, -1, NULL);
    CHECK(s.eye_width == 1440);

    /* Refresh: default (0), then the offered rates; hidden without any. */
    PsxVrRates rates = { 3, { 72, 90, 120 } };
    s.refresh_hz = 0;
    CHECK(psx_vr_menu_step(&s, PSX_VR_ITEM_REFRESH, 1, &rates) == PSX_VR_MENU_CHANGED);
    CHECK(s.refresh_hz == 72);
    psx_vr_menu_step(&s, PSX_VR_ITEM_REFRESH, 1, &rates);
    psx_vr_menu_step(&s, PSX_VR_ITEM_REFRESH, 1, &rates);
    CHECK(s.refresh_hz == 120);
    CHECK(psx_vr_menu_step(&s, PSX_VR_ITEM_REFRESH, 1, &rates) == PSX_VR_MENU_NONE);
    psx_vr_menu_step(&s, PSX_VR_ITEM_REFRESH, 0, &rates);   /* accept wraps to default */
    CHECK(s.refresh_hz == 0);
    PsxVrRates none = { 0, { 0 } };
    CHECK(psx_vr_menu_step(&s, PSX_VR_ITEM_REFRESH, 1, &none) == PSX_VR_MENU_NONE);

    PsxVrSettings d;
    psx_vr_settings_defaults(&d);
    CHECK(psx_vr_menu_step(&s, PSX_VR_ITEM_DEFAULTS, 0, NULL) == PSX_VR_MENU_CHANGED);
    CHECK(memcmp(&s, &d, sizeof s) == 0);

    /* Every item has a label. */
    for (int i = 0; i < PSX_VR_ITEM_COUNT; i++) {
        char label[48], value[32];
        psx_vr_menu_text(&s, i, label, sizeof label, value, sizeof value);
        CHECK(label[0] != '\0');
    }
}

static void test_menu_draw(void) {
    enum { W = 1024, H = 768 };
    uint8_t *px = calloc((size_t)W * H, 4);
    CHECK(px != NULL);
    if (!px) return;
    PsxVrSettings s;
    psx_vr_settings_defaults(&s);
    psx_vr_menu_draw(&s, 3, px, W, H);
    int opaque = 0;
    for (int i = 0; i < W * H; i++) opaque += px[i * 4 + 3] != 0;
    CHECK(opaque == W * H);   /* the whole panel is drawn */
    free(px);
}

enum { UP = 0x0010, DOWN = 0x0040, LEFT = 0x0080, START = 0x0008, SELECT = 0x0001,
       SQUARE = 0x8000, CROSS = 0x4000, CIRCLE = 0x2000, TRIANGLE = 0x1000,
       L1 = 0x0400, R1 = 0x0800, L2 = 0x0100, R2 = 0x0200 };

static void test_pad_word(void) {
    PsxVrControls c;
    memset(&c, 0, sizeof c);
    CHECK(psx_vr_pad_word_from(&c) == 0xFFFF);
    c.stick_ly = 0.9f;
    CHECK(psx_vr_pad_word_from(&c) == (uint16_t)~UP);
    c.stick_ly = -0.9f; c.stick_lx = -0.8f;
    CHECK(psx_vr_pad_word_from(&c) == (uint16_t)~(DOWN | LEFT));
    memset(&c, 0, sizeof c);
    c.trigger_r = 1.0f; c.b = 1; c.grip_r = 1.0f; c.a = 1;
    CHECK(psx_vr_pad_word_from(&c) == (uint16_t)~(SQUARE | TRIANGLE | CROSS | CIRCLE));
    memset(&c, 0, sizeof c);
    c.x = 1; c.y = 1; c.trigger_l = 0.7f; c.grip_l = 0.7f; c.menu = 1; c.stick_click_r = 1;
    CHECK(psx_vr_pad_word_from(&c) == (uint16_t)~(L1 | R1 | L2 | R2 | START | SELECT));
    memset(&c, 0, sizeof c);
    c.stick_click_l = 1;   /* the VR menu button never reaches the game */
    c.trigger_r = 0.2f;    /* a light touch is not a press */
    CHECK(psx_vr_pad_word_from(&c) == 0xFFFF);
}

static void test_nav(void) {
    PsxVrNavState st;
    memset(&st, 0, sizeof st);
    PsxVrControls c;
    memset(&c, 0, sizeof c);
    CHECK(psx_vr_nav(&st, &c, 0.0) == PSX_VR_NAV_NONE);
    c.stick_click_l = 1;
    CHECK(psx_vr_nav(&st, &c, 0.1) == PSX_VR_NAV_TOGGLE);
    CHECK(psx_vr_nav(&st, &c, 0.2) == PSX_VR_NAV_NONE);   /* held: one edge only */
    c.stick_click_l = 0;
    psx_vr_nav(&st, &c, 0.3);
    c.a = 1;
    CHECK(psx_vr_nav(&st, &c, 0.4) == PSX_VR_NAV_ACCEPT);
    c.a = 0;
    psx_vr_nav(&st, &c, 0.5);
    c.stick_ly = -1.0f;
    CHECK(psx_vr_nav(&st, &c, 1.0) == PSX_VR_NAV_DOWN);
    CHECK(psx_vr_nav(&st, &c, 1.2) == PSX_VR_NAV_NONE);   /* before the repeat delay */
    CHECK(psx_vr_nav(&st, &c, 1.45) == PSX_VR_NAV_DOWN);  /* repeat */
    CHECK(psx_vr_nav(&st, &c, 1.50) == PSX_VR_NAV_NONE);
    CHECK(psx_vr_nav(&st, &c, 1.60) == PSX_VR_NAV_DOWN);
    c.stick_ly = 0.0f;
    CHECK(psx_vr_nav(&st, &c, 1.7) == PSX_VR_NAV_NONE);
}

int main(void) {
    test_file_roundtrip();
    test_menu_steps();
    test_menu_draw();
    test_pad_word();
    test_nav();
    if (g_fail) { fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    printf("vr_settings: all checks passed\n");
    return 0;
}
